#include "PluginController.hpp"
#include "PluginMessages.hpp"
#include <limits>
#include <cwchar>

namespace ldap {
VisibleDiagnostic describeError(CaptureError error) noexcept {
    VisibleDiagnostic result{}; result.error = error;
    const wchar_t* code{L"Api"}; const wchar_t* cause{L"Fallo de la API del editor"};
    if (error.core) {
        constexpr std::array codes{L"InvalidUtf8", L"InvalidSyntax", L"InvalidAttribute", L"InvalidEscape", L"UnsupportedFilter",
            L"EmptyGroup", L"InvalidGroupArity", L"UnbalancedParenthesis", L"DepthExceeded", L"RawNul", L"ResourceFailure",
            L"InternalFailure", L"InputTooLarge", L"OutputTooLarge", L"MemoryExceeded", L"Cancelled"};
        constexpr std::array causes{L"UTF-8 inválido", L"Sintaxis LDAP inválida", L"Atributo inválido", L"Escape inválido",
            L"Filtro no admitido", L"Grupo vacío", L"Número de filtros inválido", L"Paréntesis incompletos",
            L"Profundidad superior a 128 niveles", L"Byte NUL no admitido", L"Recursos insuficientes", L"Fallo interno",
            L"Entrada superior a 5 MiB; seleccione el filtro", L"Salida superior a 5 MiB", L"Cuota de memoria agotada", L"Cancelado"};
        const auto index{static_cast<std::size_t>(error.core->code)};
        if (index < codes.size()) { code = codes[index]; cause = causes[index]; }
    } else {
        constexpr std::array codes{L"Api", L"DocumentChanged", L"UnsupportedState", L"Encoding", L"InvalidUtf8", L"RawNul",
            L"Length", L"Resources", L"Core", L"CommitIndeterminate"};
        constexpr std::array causes{L"Fallo de la API del editor", L"El documento o el ámbito cambió", L"Estado del editor no admitido",
            L"El documento debe usar UTF-8", L"UTF-8 inválido", L"Byte NUL no admitido", L"Límite de longitud excedido; seleccione el filtro",
            L"Recursos insuficientes", L"Fallo del núcleo LDAP", L"Resultado no confirmable; formateo bloqueado hasta reinicialización"};
        const auto index{static_cast<std::size_t>(error.code)};
        if (index < codes.size()) { code = codes[index]; cause = causes[index]; }
    }
    if (error.hostBlocked) { cause = L"Estado del host no confirmable; formateo bloqueado hasta reinicialización"; }
    const auto* scope{error.scope && error.scope->anchor != error.scope->caret ? L"selección capturada" : L"documento capturado"};
    std::array<wchar_t, 192> position{};
    if (error.position == ErrorPosition::Input) {
        (void)swprintf_s(position.data(), position.size(), L"Byte de entrada %zu (desde cero, %ls)", error.offset, scope);
        if (error.scope && error.scope->begin >= 0 && error.scope->end >= error.scope->begin
            && error.offset <= static_cast<uptr_t>(error.scope->end - error.scope->begin)
            && error.offset <= static_cast<uptr_t>(std::numeric_limits<Sci_Position>::max() - error.scope->begin)) {
            const auto documentByte{static_cast<std::size_t>(error.scope->begin) + error.offset};
            (void)swprintf_s(position.data(), position.size(), L"Byte de entrada %zu (desde cero, %ls); byte documental %zu",
                error.offset, scope, documentByte);
        }
    } else if (error.position == ErrorPosition::Output) {
        (void)swprintf_s(position.data(), position.size(), L"Byte de salida %zu (desde cero, %ls); posición de entrada no disponible", error.offset, scope);
    } else if (error.scope) {
        (void)swprintf_s(position.data(), position.size(), L"Posición no disponible (%ls)", scope);
    } else { (void)swprintf_s(position.data(), position.size(), L"Posición no disponible"); }
    (void)swprintf_s(result.text.data(), result.text.size(), L"%ls [%ls].\n%ls.", cause, code, position.data());
    return result;
}
CaptureError PluginController::coreError(const FormatDelivery& delivery) noexcept {
    const auto code{delivery.result.error->code};
    const bool input{code != LdapErrorCode::OutputTooLarge && code != LdapErrorCode::ResourceFailure
        && code != LdapErrorCode::MemoryExceeded && code != LdapErrorCode::InternalFailure && code != LdapErrorCode::Cancelled};
    return CaptureError{CaptureErrorCode::Core, delivery.result.error->offset, delivery.result.error,
        delivery.snapshot.metadata, input ? ErrorPosition::Input : code == LdapErrorCode::OutputTooLarge ? ErrorPosition::Output : ErrorPosition::Unavailable};
}
void PluginController::queueDiagnostic() noexcept {
    if (!requested_ || closing_ || !error_ || diagnostic_ || diagnosedGeneration_ == generation_
        || (error_->core && error_->core->code == LdapErrorCode::Cancelled)) { return; }
    diagnosedGeneration_ = generation_;
    auto visible{*error_};
    if (hostGateFailed_ && visible.code != CaptureErrorCode::CommitIndeterminate) {
        visible = CaptureError{CaptureErrorCode::UnsupportedState, 0};
        visible.hostBlocked = true;
    }
    diagnostic_ = QueuedDiagnostic{visible, generation_};
    if (!PostMessageW(window_, showDiagnostic, 0, 0)) { diagnostic_.reset(); }
}
void PluginController::presentDiagnostic() noexcept {
    if (presenting_ || applying_ || restoring_ || undoRedo_ || notificationDepth_ || closing_ || !diagnostic_) { return; }
    const auto queued{*diagnostic_}; diagnostic_.reset();
    presenting_ = true;
    const auto owner{gateway_.host()._nppHandle};
    bool valid{queued.generation == generation_ && IsWindow(owner)
        && GetWindowThreadProcessId(owner, nullptr) == uiThread_};
    if (valid && queued.error.scope) {
        const auto current{gateway_.identity()};
        valid = current && current->view == queued.error.scope->view && current->buffer == queued.error.scope->buffer
            && current->document == queued.error.scope->document;
    }
    // Identity queries call host code, which may start a new request or close
    // the session. Revalidate after those calls before exposing the old error.
    valid = valid && !closing_ && queued.generation == generation_ && owner == gateway_.host()._nppHandle && IsWindow(owner);
    if (valid && presenter_) {
        const auto described{describeError(queued.error)};
        (void)presenter_(owner, described, presenterContext_);
    }
    presenting_ = false;
    if (diagnostic_ && !closing_) { (void)PostMessageW(window_, showDiagnostic, 0, 0); }
    if (restoreBatch_ && !closing_) { (void)PostMessageW(window_, restoreSelection, 0, 0); }
    poll();
}
}
