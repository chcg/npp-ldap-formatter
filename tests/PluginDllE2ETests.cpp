#include "support/SimulatedNppHost.hpp"
#include <cwchar>
#include <iostream>
#include <memory>
#include <source_location>
#include <string_view>

namespace {
struct ModuleCloser { void operator()(HINSTANCE__* module) const noexcept { if (module) { FreeLibrary(module); } } };
struct Observation {
    std::size_t count{};
    HWND owner{};
    UINT flags{};
    std::wstring title{}, diagnostic{};
} observation;
int WINAPI observeMessageBox(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags) {
    ++observation.count; observation.owner = owner; observation.flags = flags;
    observation.title = title ? title : L""; observation.diagnostic = text ? text : L""; return IDOK;
}
// Instrument only the loaded production module. All RVAs are bounded by its
// validated PE image; import strings and thunk arrays require a terminator.
class MessageBoxImport {
    ULONG_PTR* slot_{};
    ULONG_PTR original_{};
public:
    explicit MessageBoxImport(HMODULE module) {
        auto* base{reinterpret_cast<unsigned char*>(module)};
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(base, &memory, sizeof(memory)) || memory.AllocationBase != module || memory.RegionSize < sizeof(IMAGE_DOS_HEADER)) { throw std::runtime_error{"PE allocation"}; }
        const auto* dos{reinterpret_cast<const IMAGE_DOS_HEADER*>(base)};
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || memory.RegionSize < sizeof(IMAGE_NT_HEADERS) || static_cast<std::size_t>(dos->e_lfanew) > memory.RegionSize - sizeof(IMAGE_NT_HEADERS)) { throw std::runtime_error{"PE DOS"}; }
        const auto* nt{reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew)};
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) { throw std::runtime_error{"PE NT"}; }
        const auto size{static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage)};
        const auto bounded = [size, base, module](std::size_t offset, std::size_t count) {
            if (offset > size || count > size - offset) { return false; }
            while (count) {
                MEMORY_BASIC_INFORMATION region{};
                if (!VirtualQuery(base + offset, &region, sizeof(region)) || region.AllocationBase != module || region.State != MEM_COMMIT
                    || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) { return false; }
                const auto available{region.RegionSize - static_cast<std::size_t>(base + offset - static_cast<unsigned char*>(region.BaseAddress))};
                const auto portion{std::min<std::size_t>(count, available)}; if (!portion) { return false; } offset += portion; count -= portion;
            }
            return true;
        };
        const auto directory{nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]};
        if (!directory.VirtualAddress || !bounded(directory.VirtualAddress, directory.Size)) { throw std::runtime_error{"PE imports"}; }
        bool ended{};
        for (std::size_t offset{directory.VirtualAddress}; bounded(offset, sizeof(IMAGE_IMPORT_DESCRIPTOR)) && offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= static_cast<std::size_t>(directory.VirtualAddress) + directory.Size; offset += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            const auto& descriptor{*reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + offset)};
            if (!descriptor.Name) { ended = true; break; }
            if (!descriptor.OriginalFirstThunk || !descriptor.FirstThunk) { throw std::runtime_error{"PE thunk missing"}; }
            for (std::size_t index{};; ++index) {
                const auto original{static_cast<std::size_t>(descriptor.OriginalFirstThunk) + index * sizeof(IMAGE_THUNK_DATA)};
                const auto address{static_cast<std::size_t>(descriptor.FirstThunk) + index * sizeof(IMAGE_THUNK_DATA)};
                if (!bounded(original, sizeof(IMAGE_THUNK_DATA)) || !bounded(address, sizeof(IMAGE_THUNK_DATA))) { throw std::runtime_error{"PE thunk range"}; }
                const auto& thunk{*reinterpret_cast<const IMAGE_THUNK_DATA*>(base + original)};
                if (!thunk.u1.AddressOfData) { break; }
                if (IMAGE_SNAP_BY_ORDINAL(thunk.u1.Ordinal)) { continue; }
                const auto nameOffset{static_cast<std::size_t>(thunk.u1.AddressOfData)};
                if (!bounded(nameOffset, sizeof(WORD) + 1)) { throw std::runtime_error{"PE import name"}; }
                const auto* name{reinterpret_cast<const char*>(base + nameOffset + sizeof(WORD))};
                const auto capacity{std::min<std::size_t>(size - nameOffset - sizeof(WORD), 256)};
                if (!bounded(nameOffset + sizeof(WORD), capacity)) { throw std::runtime_error{"PE import string range"}; }
                const auto* end{static_cast<const char*>(std::memchr(name, '\0', capacity))};
                if (!end) { throw std::runtime_error{"PE import terminator"}; }
                if (std::string_view{name, static_cast<std::size_t>(end - name)} == "MessageBoxW") {
                    if (slot_) { throw std::runtime_error{"PE duplicate MessageBoxW"}; }
                    slot_ = &reinterpret_cast<IMAGE_THUNK_DATA*>(base + address)->u1.Function;
                }
            }
        }
        if (!ended || !slot_) { throw std::runtime_error{"MessageBoxW seam unavailable"}; }
        original_ = *slot_;
        DWORD previous{};
        if (!VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &previous)) { throw std::runtime_error{"IAT protection"}; }
        *slot_ = reinterpret_cast<ULONG_PTR>(&observeMessageBox);
        DWORD ignored{};
        if (!VirtualProtect(slot_, sizeof(*slot_), previous, &ignored)) { *slot_ = original_; throw std::runtime_error{"IAT protection restoration"}; }
    }
    ~MessageBoxImport() {
        if (!slot_) { return; }
        DWORD previous{}, ignored{};
        if (!VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &previous)) { std::terminate(); }
        *slot_ = original_;
        if (!VirtualProtect(slot_, sizeof(*slot_), previous, &ignored)) { std::terminate(); }
    }
    MessageBoxImport(const MessageBoxImport&) = delete;
    MessageBoxImport& operator=(const MessageBoxImport&) = delete;
};
struct Session {
    e2e::SimulatedNppHost& host;
    PFUNCSETINFO info{};
    PBENOTIFIED notify{};
    Session(e2e::SimulatedNppHost& h, PFUNCSETINFO i, PBENOTIFIED n) : host{h}, info{i}, notify{n} { host.notify = notify; info(host.data()); host.notification(NPPN_READY); }
    ~Session() { host.notification(NPPN_SHUTDOWN); e2e::SimulatedNppHost::drain(); host.notify = nullptr; }
};
}
int wmain(int argc, wchar_t* argv[]) {
    if (argc != 2) { return 1; }
    try {
        const std::unique_ptr<HINSTANCE__, ModuleCloser> module{LoadLibraryW(argv[1])};
        if (!module) { throw std::runtime_error{"DLL load"}; }
        for (const auto* name : {"isUnicode", "setInfo", "getName", "getFuncsArray", "beNotified", "messageProc"}) {
            if (!GetProcAddress(module.get(), name)) { throw std::runtime_error{"DLL export"}; }
        }
        MessageBoxImport interception{module.get()};
        e2e::SimulatedNppHost host{};
        const auto info{reinterpret_cast<PFUNCSETINFO>(GetProcAddress(module.get(), "setInfo"))};
        const auto notify{reinterpret_cast<PBENOTIFIED>(GetProcAddress(module.get(), "beNotified"))};
        const auto functions{reinterpret_cast<PFUNCGETFUNCSARRAY>(GetProcAddress(module.get(), "getFuncsArray"))};
        { Session initial{host, info, notify}; }
        int count{}; auto* commands{functions(&count)};
        if (!commands || count != 3 || !commands[0]._pFunc || !commands[1]._pFunc || std::wcscmp(commands[0]._itemName, L"Format LDAP (Pretty)") || std::wcscmp(commands[1]._itemName, L"Format LDAP (Compact)")) { throw std::runtime_error{"DLL commands"}; }
        int failures{}; std::size_t cases{}, containment{};
        const auto check = [&](bool passed, std::source_location location = std::source_location::current()) {
            if (!passed) { ++failures; std::cerr << "E2E assertion line " << location.line() << '\n'; }
        };
        const std::string input{"( & (cn=Jos\xc3\xa9) (sn=b\\2a) )"};
        constexpr int requiredFlags{SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT | SC_PERFORMED_UNDO | SC_PERFORMED_REDO | SC_MOD_CONTAINER | SC_LASTSTEPINUNDOREDO};
        check(host.flags == requiredFlags);
        const std::string compact{"(&(cn=Jos\xc3\xa9)(sn=b\\2a))"};
        for (int view{}; view != 2; ++view) { for (int command{}; command != 2; ++command) {
          for (int direction{}; direction != 3; ++direction) { for (const int eol : {SC_EOL_CRLF, SC_EOL_LF, SC_EOL_CR}) { for (const bool first : {false, true}) {
            host.documents = {}; host.active = view; auto& d{host.documents[static_cast<std::size_t>(view)]};
            d.eol = eol; d.containerFirst = first;
            const std::string newline{eol == SC_EOL_LF ? "\n" : eol == SC_EOL_CR ? "\r" : "\r\n"};
            const auto formatted{command ? compact : "(&" + newline + "  (cn=Jos\xc3\xa9)" + newline + "  (sn=b\\2a)" + newline + ")"};
            const bool full{direction == 2}; const Sci_Position begin{full ? 0 : 4};
            const auto end{begin + static_cast<Sci_Position>(input.size())};
            d.image = {full ? input : "pre " + input + " post", direction == 1 ? end : full ? 3 : begin, direction == 1 ? begin : full ? 3 : end};
            const auto original{d.image}; const e2e::Image previous{"synthetic independent edit", 0, 0};
            d.history.push_back({previous, original, 0}); d.cursor = 1;
            host.documents[static_cast<std::size_t>(1 - view)].image = {"inactive synthetic bytes", 2, 5};
            const auto inactive{host.documents[static_cast<std::size_t>(1 - view)].image};
            Session session{host, info, notify}; const auto registrations{host.registrations}; host.notification(NPPN_READY);
            check(host.registrations == registrations); commands[command]._pFunc();
            check(e2e::SimulatedNppHost::pumpUntil([&] { return d.ends == 1; }));
            const auto outputEnd{begin + static_cast<Sci_Position>(formatted.size())};
            const e2e::Image expected{full ? formatted : "pre " + formatted + " post", direction == 1 ? outputEnd : begin, direction == 1 ? begin : outputEnd};
            check(d.image == expected && d.replacements == 1 && d.begins == 1 && d.ends == 1 && d.cursor == 2);
            check(host.documents[static_cast<std::size_t>(1 - view)].image == inactive);
            host.undo(static_cast<std::size_t>(view)); check(d.image == original && d.cursor == 1);
            host.undo(static_cast<std::size_t>(view), true); check(d.image == expected && d.cursor == 2);
            host.undo(static_cast<std::size_t>(view)); check(d.image == original && d.cursor == 1);
            host.undo(static_cast<std::size_t>(view), true); check(d.image == expected && d.cursor == 2);
            host.undo(static_cast<std::size_t>(view)); host.undo(static_cast<std::size_t>(view)); check(d.image.bytes == previous.bytes && d.cursor == 0);
            ++cases;
          } } }
        } }
        // Rejections exercise the real worker and deferred production presenter.
        for (int rejection{}; rejection != 18; ++rejection) {
            host.documents = {}; host.active = 0; auto& d{host.documents[0]};
            d.image = {input, 3, 3}; std::wstring code{};
            switch (rejection) {
            case 0: d.image.bytes = std::string{"\xff", 1}; d.image.anchor = d.image.caret = 0; code = L"InvalidUtf8"; break;
            case 1: d.image.bytes = std::string{"a\0b", 3}; d.image.anchor = d.image.caret = 0; code = L"RawNul"; break;
            case 2: d.image.bytes = "(cn="; code = L"UnbalancedParenthesis"; break;
            case 3: d.image.bytes = "(cn=\\zz)"; code = L"InvalidEscape"; break;
            case 4: d.readonly = true; code = L"UnsupportedState"; break;
            case 5: d.collect = false; code = L"UnsupportedState"; break;
            case 6: d.selections = 2; code = L"UnsupportedState"; break;
            case 7: d.mode = SC_SEL_RECTANGLE; code = L"UnsupportedState"; break;
            case 8: d.virtualSpace = true; code = L"UnsupportedState"; break;
            case 9: d.codepage = 0; code = L"Encoding"; break;
            case 10: d.badCount = true; code = L"Api"; break;
            case 11: d.badNul = true; code = L"Api"; break;
            case 12: d.image.bytes.assign(5242881, 'a'); code = L"Length"; break;
            case 13: d.image.bytes = std::string(129, '('); for (int i{}; i != 129; ++i) { d.image.bytes.insert(static_cast<std::size_t>(i * 2 + 1), "!"); } d.image.bytes += "(cn=a)" + std::string(129, ')'); code = L"DepthExceeded"; break;
            case 14: d.nullDocument = true; code = L"Api"; break;
            case 15: d.image.bytes = "(|(cn=" + std::string(5242880 - 14, 'a') + ")(sn=b))"; code = L"OutputTooLarge"; break;
            case 16: d.image.bytes = "(|"; for (int i{}; i != 600000; ++i) { d.image.bytes += "(cn=a)"; } d.image.bytes += ")"; code = L"OutputTooLarge"; break;
            case 17: d.image = {"pre (cn=" + std::string{"\xff", 1} + ")", 4, 10}; code = L"InvalidUtf8"; break;
            }
            const auto original{d.image}; const auto history{d.history}; const auto notices{observation.count};
            Session session{host, info, notify}; commands[0]._pFunc(); check(observation.count == notices);
            check(e2e::SimulatedNppHost::pumpUntil([&] { return observation.count > notices; }));
            if (observation.diagnostic.find(L"[" + code + L"]") == std::wstring::npos) {
                std::cerr << "Diagnostic mismatch for rejection " << rejection << '\n';
            }
            check(observation.count == notices + 1 && observation.owner == host.host && observation.flags == (MB_OK | MB_ICONERROR)
                && observation.title == L"LDAP Formatter" && observation.diagnostic.find(L"[" + code + L"]") != std::wstring::npos);
            check(d.image == original && d.history == history && d.cursor == 0 && d.replacements == 0 && d.begins == 0 && d.ends == 0); ++cases;
            if (rejection == 0) { check(observation.diagnostic.find(L"UTF-8 inválido") != std::wstring::npos && observation.diagnostic.find(L"Byte de entrada 0") != std::wstring::npos); }
            if (rejection == 17) { check(observation.diagnostic.find(L"Byte de entrada 4") != std::wstring::npos && observation.diagnostic.find(L"byte documental 8") != std::wstring::npos); }
        }
        // Eligible selection in an otherwise oversized document.
        { host.documents = {}; auto& d{host.documents[0]}; host.active = 0;
          d.image = {std::string(5242881, 'x') + input, 5242881, 5242881 + static_cast<Sci_Position>(input.size())};
          const auto prefix{d.image.bytes.substr(0, 5242881)}; Session session{host, info, notify}; commands[1]._pFunc();
          check(e2e::SimulatedNppHost::pumpUntil([&] { return d.ends == 1; })); check(d.image.bytes == prefix + compact && d.replacements == 1); ++cases; }
        // Early rejection and irreversible host effects remain distinct. The
        // controller must never attempt a second replacement or blind Undo.
        std::size_t integrityLoss{};
        struct FaultObservation { bool text{}, selection{}, history{}; std::size_t replacements{}, undoCalls{}; };
        std::array<FaultObservation, 6> faults{};
        for (int fault{}; fault != 6; ++fault) {
            host.documents = {}; host.active = 0; auto& d{host.documents[0]}; d.image = {input, 3, 3};
            const auto original{d.image}; const auto history{d.history}; const auto notices{observation.count};
            Session session{host, info, notify};
            if (fault == 0) { d.earlyFailure = true; }
            if (fault == 1) { d.lateFailure = true; }
            if (fault == 2) { d.deleteFailure = true; }
            if (fault == 3) { d.onEnd = [&] { d.image.anchor = 1; }; }
            if (fault == 4) { d.onToken = [&] { d.nullDocument = true; }; }
            if (fault == 5) { d.onSelect = [&] { d.image.bytes[0] = 'X'; d.selectionFailure = true; }; }
            commands[1]._pFunc(); check(e2e::SimulatedNppHost::pumpUntil([&] { return d.ends == 1; }));
            // Identity-loss diagnostics are intentionally silent.
            if (fault != 4 && fault != 5) { check(e2e::SimulatedNppHost::pumpUntil([&] { return observation.count > notices; }));
                check(observation.diagnostic.find(L"[CommitIndeterminate]") != std::wstring::npos); }
            const bool textEqual{d.image.bytes == original.bytes}, selectionEqual{d.image.anchor == original.anchor && d.image.caret == original.caret}, historyEqual{d.history == history};
            faults[static_cast<std::size_t>(fault)] = {textEqual, selectionEqual, historyEqual, d.replacements, d.undoCalls};
            if (!textEqual || !selectionEqual || !historyEqual) { ++integrityLoss; }
            if (fault <= 2) { check(textEqual && selectionEqual); }
            const std::size_t expectedUndos{fault == 1 || fault == 2 ? 1U : 0U};
            check(d.replacements <= 1 && d.undoCalls == expectedUndos && d.begins == d.ends);
            const auto blocked{d.image}; const auto blockedHistory{d.history}; const auto replacements{d.replacements};
            commands[0]._pFunc(); e2e::SimulatedNppHost::drain();
            check(d.image == blocked && d.history == blockedHistory && d.replacements == replacements && d.undoCalls == expectedUndos);
            ++cases; ++containment;
        }
        // Partial deletion, including an internal exception, must preserve the
        // selection orientation and leave the preceding independent edit intact.
        for (const bool reverse : {false, true}) { for (const bool exception : {false, true}) {
            host.documents = {}; host.active = 0; auto& d{host.documents[0]};
            const Sci_Position begin{4}, end{begin + static_cast<Sci_Position>(input.size())};
            d.image = {"pre " + input + " post", reverse ? end : begin, reverse ? begin : end};
            const auto original{d.image}; const e2e::Image previous{"independent edit", 0, 0};
            d.history.push_back({previous, original, 0}); d.cursor = 1;
            d.deleteFailure = !exception; d.deleteException = exception;
            Session session{host, info, notify}; commands[1]._pFunc();
            check(e2e::SimulatedNppHost::pumpUntil([&] { return d.ends == 1; }));
            check(d.image == original && d.undoCalls == 1 && d.cursor == 1
                && d.history.front() == e2e::Action{previous, original, 0} && d.begins == d.ends);
            ++cases;
        } }
        for (int failure{}; failure != 5; ++failure) {
            host.documents = {}; host.active = 0; auto& d{host.documents[0]};
            d.image = {input, 3, 3}; const auto original{d.image};
            const e2e::Image previous{"independent edit", 0, 0};
            d.history.push_back({previous, original, 0}); d.cursor = 1;
            if (failure == 0) { d.earlyFailure = true; }
            else { d.deleteFailure = true; }
            if (failure == 1) { d.onEnd = [&] { d.nullDocument = true; }; }
            if (failure == 2) { d.undoFailure = true; }
            if (failure == 3) { d.onEnd = [&] { d.status = SC_STATUS_FAILURE; }; }
            if (failure == 4) { d.onEnd = [&] { SendMessageW(host.views[0], SCI_BEGINUNDOACTION, 0, 0); }; }
            Session session{host, info, notify}; commands[1]._pFunc();
            check(e2e::SimulatedNppHost::pumpUntil([&] { return d.ends == 1; }));
            check(d.history.front() == e2e::Action{previous, original, 0}
                && d.cursor == 2 && d.undoCalls == (failure == 2 ? 1U : 0U));
            if (failure == 0) { check(d.image == original); }
            else { check(d.image.bytes.empty()); }
            ++cases;
        }
        // Reject a host-owned group before inserting a token or changing selection.
        { host.documents = {}; host.active = 0; auto& d{host.documents[0]};
          d.image = {input, 3, 3}; const auto original{d.image};
          d.history.push_back({{"independent edit", 0, 0}, original, 0}); d.cursor = 1;
          const auto history{d.history}; Session session{host, info, notify};
          SendMessageW(host.views[0], SCI_BEGINUNDOACTION, 0, 0);
          const auto notices{observation.count}; commands[1]._pFunc();
          check(e2e::SimulatedNppHost::pumpUntil([&] { return observation.count > notices; }));
          check(d.image == original && d.history == history && d.cursor == 1 && d.replacements == 0
              && d.undoCalls == 0 && d.begins == 1 && d.ends == 0 && d.groupDepth == 1
              && observation.diagnostic.find(L"[UnsupportedState]") != std::wstring::npos);
          SendMessageW(host.views[0], SCI_ENDUNDOACTION, 0, 0); ++cases; }
        // A token is its own raw action even if replacement fails before editing;
        // inserting it drops an existing Redo tail, which cannot be recovered.
        { host.documents = {}; host.active = 0; auto& d{host.documents[0]};
          d.image = {input, 3, 3}; const auto original{d.image};
          const e2e::Action previous{{"independent edit", 0, 0}, original, 0};
          d.history = {previous, {original, {"old Redo", 0, 0}, 0}}; d.cursor = 1; d.earlyFailure = true;
          Session session{host, info, notify}; commands[1]._pFunc();
          check(e2e::SimulatedNppHost::pumpUntil([&] { return d.ends == 1; }));
          check(d.image == original && d.cursor == 2 && d.history.size() == 2 && d.history.front() == previous
              && d.history.back().token > 0 && !d.history.back().content && d.undoCalls == 0
              && SendMessageW(host.views[0], SCI_GETUNDOCURRENT, 0, 0) == 2);
          host.undo(0); check(d.image == original && d.cursor == 1);
          host.undo(0, true); check(d.image == original && d.cursor == 2); ++cases; }
        // Two distinct format commands retain their own before/after selection.
        for (const bool reverse : {false, true}) { for (const bool first : {false, true}) {
            host.documents = {}; host.active = 0; auto& d{host.documents[0]}; d.containerFirst = first;
            const Sci_Position begin{4}, end{begin + static_cast<Sci_Position>(input.size())};
            d.image = {"pre " + input + " post", reverse ? end : begin, reverse ? begin : end};
            const auto original{d.image}; Session session{host, info, notify};
            commands[0]._pFunc(); check(e2e::SimulatedNppHost::pumpUntil([&] { return d.ends == 1; }));
            const auto pretty{d.image};
            commands[1]._pFunc(); check(e2e::SimulatedNppHost::pumpUntil([&] { return d.ends == 2; }));
            const auto compactImage{d.image};
            check(compactImage.bytes == "pre " + compact + " post");
            host.undo(0); check(d.image == pretty && d.cursor == 1);
            host.undo(0); check(d.image == original && d.cursor == 0);
            host.undo(0, true); check(d.image == pretty && d.cursor == 1);
            host.undo(0, true); check(d.image == compactImage && d.cursor == 2);
            ++cases;
        } }
        // Cancellation uses production worker scheduling; no internal test ABI.
        for (int stale{}; stale != 3; ++stale) {
            host.documents = {}; host.active = 0; auto& d{host.documents[0]};
            d.image = {"( & (cn=" + std::string(300 * 1024, 'a') + ") (sn=b) )", 0, 0};
            d.history.push_back({{"synthetic independent edit", 0, 0}, d.image, 0}); d.cursor = 1;
            auto& inactive{host.documents[1]}; inactive.image = {"inactive synthetic bytes", 2, 5};
            inactive.history.push_back({{"other independent edit", 0, 0}, inactive.image, 0}); inactive.cursor = 1;
            auto expected{d.image}; const auto history{d.history};
            const auto inactiveImage{inactive.image}; const auto inactiveHistory{inactive.history};
            const auto notices{observation.count}; Session session{host, info, notify};
            const auto messages{e2e::SimulatedNppHost::workerMessages}; commands[0]._pFunc();
            if (stale == 0) { host.active = 1; host.notification(NPPN_BUFFERACTIVATED); }
            if (stale == 1) { SCNotification close{}; close.nmhdr.hwndFrom = host.host;
                close.nmhdr.code = NPPN_FILEBEFORECLOSE; close.nmhdr.idFrom = 41; notify(&close); }
            if (stale == 2) { SCNotification update{}; update.nmhdr.hwndFrom = host.views[0]; update.nmhdr.code = SCN_UPDATEUI; update.updated = SC_UPDATE_SELECTION;
                expected.anchor = expected.caret = 1; d.image = expected; notify(&update); }
            // The worker posts completion even after cancellation; dispatch it before checking silence.
            check(e2e::SimulatedNppHost::pumpUntil([&] { return e2e::SimulatedNppHost::workerMessages > messages; }));
            check(e2e::SimulatedNppHost::workerMessages == messages + 1);
            if (d.image != expected || d.history != history || d.cursor != 1 || d.replacements || d.begins || d.ends || d.undoCalls || observation.count != notices) {
                std::cerr << "Stale event " << stale << " image_equal=" << (d.image == expected) << " history_equal=" << (d.history == history)
                    << " cursor=" << d.cursor << " replacements=" << d.replacements << " notices=" << observation.count - notices
                    << " document_changed=" << (observation.diagnostic.find(L"[DocumentChanged]") != std::wstring::npos) << '\n';
            }
            check(d.image == expected && d.history == history && d.cursor == 1
                && d.replacements == 0 && d.begins == 0 && d.ends == 0 && d.undoCalls == 0 && observation.count == notices);
            check(inactive.image == inactiveImage && inactive.history == inactiveHistory && inactive.cursor == 1
                && inactive.replacements == 0 && inactive.begins == 0 && inactive.ends == 0 && inactive.undoCalls == 0); ++cases;
        }
        // Shutdown independently proves cancellation and quiescence without masking stale-event delivery.
        { host.documents = {}; host.active = 0; auto& d{host.documents[0]};
          d.image = {"( & (cn=" + std::string(300 * 1024, 'a') + ") (sn=b) )", 0, 0};
          d.history.push_back({{"synthetic independent edit", 0, 0}, d.image, 0}); d.cursor = 1;
          const auto original{d.image}; const auto history{d.history}; const auto notices{observation.count};
          Session session{host, info, notify}; commands[0]._pFunc();
          host.notification(NPPN_SHUTDOWN); e2e::SimulatedNppHost::drain();
          check(d.image == original && d.history == history && d.cursor == 1
              && d.replacements == 0 && d.begins == 0 && d.ends == 0 && d.undoCalls == 0 && observation.count == notices); ++cases; }
        // New command invalidates a queued error before MessageBox delivery.
        { host.documents = {}; host.active = 0; auto& d{host.documents[0]}; d.image = {std::string{"\xff", 1}, 0, 0};
          const auto notices{observation.count}; Session session{host, info, notify}; commands[0]._pFunc();
          d.image = {input, 0, 0}; commands[1]._pFunc(); check(e2e::SimulatedNppHost::pumpUntil([&] { return d.ends == 1; }));
          check(d.image.bytes == compact && observation.count == notices); ++cases; }
        // A new request while the worker runs publishes only the latest mode.
        { host.documents = {}; host.active = 0; auto& d{host.documents[0]};
          const auto value{std::string(300 * 1024, 'a')}; d.image = {"( & (cn=" + value + ") (sn=b) )", 0, 0};
          const auto notices{observation.count}; Session session{host, info, notify}; commands[0]._pFunc(); commands[1]._pFunc();
          check(e2e::SimulatedNppHost::pumpUntil([&] { return d.ends == 1; }));
          check(d.image.bytes == "(&(cn=" + value + ")(sn=b))" && d.replacements == 1 && observation.count == notices); ++cases; }
        const auto preservationGate = [](std::size_t losses) -> std::string_view { return losses ? "failed" : "passed"; };
        check(preservationGate(0) == "passed" && preservationGate(1) == "failed");
        check(!host.wrongThread && containment == 6 && e2e::SimulatedNppHost::workerMessages > 0);
        std::cout << "{\"schema\":1,\"suite\":\"simulated-host\",\"cases\":" << cases
            << ",\"failures\":" << failures << ",\"containment_cases\":" << containment
            << ",\"integrity_loss_cases\":" << integrityLoss
            << ",\"preservation_gate\":\"" << preservationGate(integrityLoss)
            << "\",\"native_gates\":\"pending\",\"release_blocked\":true,\"fault_observations\":[";
        for (std::size_t i{}; i != faults.size(); ++i) {
            const auto& fault{faults[i]}; if (i) { std::cout << ','; }
            std::cout << "{\"case\":" << i << ",\"text_equal\":" << (fault.text ? "true" : "false")
                << ",\"selection_equal\":" << (fault.selection ? "true" : "false") << ",\"history_equal\":" << (fault.history ? "true" : "false")
                << ",\"replacements\":" << fault.replacements << ",\"undo_calls\":" << fault.undoCalls << '}';
        }
        std::cout << "]}\n";
        return failures ? 3 : 0;
    } catch (const std::exception& error) { std::cerr << "E2E infrastructure: " << error.what() << '\n'; return 2; }
}
