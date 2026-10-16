#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <exception>

namespace ldap {
inline constexpr std::size_t MaxInputBytes{5'242'880};
inline constexpr std::size_t MaxOutputBytes{5'242'880};
inline constexpr std::size_t MaxDepth{128};
inline constexpr std::size_t MaxCoreBytes{33'554'432};
class BudgetExceeded final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override { return "Core memory budget exceeded"; }
};

// One instance is shared by the core, snapshots and pending deliveries.
// Construct with std::make_shared<CoreBudget>(); reservations retain shared ownership.
class CoreBudget final : public std::enable_shared_from_this<CoreBudget> {
public:
    class Reservation final {
    public:
        Reservation() noexcept = default;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation(Reservation&& other) noexcept;
        Reservation& operator=(Reservation&& other) noexcept;
        ~Reservation();
        [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
        [[nodiscard]] const CoreBudget* budget() const noexcept { return owner_.get(); }
    private:
        friend class CoreBudget;
        Reservation(std::shared_ptr<CoreBudget> owner, std::size_t bytes) noexcept;
        std::shared_ptr<CoreBudget> owner_{};
        std::size_t bytes_{};
    };
    [[nodiscard]] std::optional<Reservation> reserve(std::size_t bytes);
    [[nodiscard]] std::size_t used() const noexcept { return used_.load(); }
    [[nodiscard]] std::size_t peak() const noexcept { return peak_.load(); }
    [[nodiscard]] std::size_t remaining() const noexcept { return MaxCoreBytes - used(); }
private:
    // Conservative bound for this object and its shared ownership control block.
    static constexpr std::size_t MetadataBytes{256};
    template<class T> friend class BudgetAllocator;
    // Allocator hooks: acquisition precedes allocation; release follows destruction.
    [[nodiscard]] bool acquire(std::size_t bytes) noexcept;
    void release(std::size_t bytes) noexcept;
    std::atomic<std::size_t> used_{MetadataBytes};
    std::atomic<std::size_t> peak_{MetadataBytes};
};

template<class T> class BudgetAllocator {
public:
    using value_type = T;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    BudgetAllocator() noexcept = default;
    // A moved-from Debug container still owns iterator bookkeeping to release.
    BudgetAllocator(const BudgetAllocator&) noexcept = default;
    BudgetAllocator& operator=(const BudgetAllocator&) noexcept = default;
    explicit BudgetAllocator(std::shared_ptr<CoreBudget> budget) noexcept : budget_{std::move(budget)} {}
    template<class U> BudgetAllocator(const BudgetAllocator<U>& other) noexcept : budget_{other.budget()} {}
    [[nodiscard]] T* allocate(std::size_t count) {
        if (count > MaxCoreBytes / sizeof(T) || !budget_ || !budget_->acquire(count * sizeof(T))) {
            throw BudgetExceeded{};
        }
        try { return std::allocator<T>{}.allocate(count); }
        catch (...) { budget_->release(count * sizeof(T)); throw; }
    }
    void deallocate(T* pointer, std::size_t count) noexcept {
        std::allocator<T>{}.deallocate(pointer, count);
        budget_->release(count * sizeof(T));
    }
    [[nodiscard]] const std::shared_ptr<CoreBudget>& budget() const noexcept { return budget_; }
    template<class U> bool operator==(const BudgetAllocator<U>& other) const noexcept { return budget_ == other.budget(); }
private:
    std::shared_ptr<CoreBudget> budget_{};
};
using BudgetString = std::basic_string<char, std::char_traits<char>, BudgetAllocator<char>>;
// Preserve comparisons with ordinary strings used by existing callers.
inline bool operator==(const BudgetString& a, const std::string& b) noexcept {
    return std::string_view{a} == std::string_view{b};
}
inline bool operator==(const std::string& a, const BudgetString& b) noexcept { return b == a; }
} // namespace ldap
