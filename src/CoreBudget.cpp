#include "CoreBudget.hpp"

namespace ldap {
CoreBudget::Reservation::Reservation(std::shared_ptr<CoreBudget> owner, std::size_t bytes) noexcept
    : owner_{std::move(owner)}, bytes_{bytes} {}
CoreBudget::Reservation::Reservation(Reservation&& other) noexcept
    : owner_{std::move(other.owner_)}, bytes_{std::exchange(other.bytes_, 0)} {}
CoreBudget::Reservation& CoreBudget::Reservation::operator=(Reservation&& other) noexcept {
    if (this != &other) {
        if (owner_) { owner_->release(bytes_); }
        owner_ = std::move(other.owner_);
        bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
}
CoreBudget::Reservation::~Reservation() { if (owner_) { owner_->release(bytes_); } }
bool CoreBudget::acquire(std::size_t bytes) noexcept {
    // Supported shared-control metadata has a conservative 128-byte allowance;
    // the rest of the fixed baseline must fit the budget object itself.
    static_assert(sizeof(CoreBudget) + 128 <= MetadataBytes);
    auto current{used_.load()};
    do {
        if (bytes > MaxCoreBytes - current) { return false; }
    } while (!used_.compare_exchange_weak(current, current + bytes));
    return true;
}
void CoreBudget::release(std::size_t bytes) noexcept { used_.fetch_sub(bytes); }
std::optional<CoreBudget::Reservation> CoreBudget::reserve(std::size_t bytes) {
    auto owner{shared_from_this()};
    if (!acquire(bytes)) { return std::nullopt; }
    return Reservation{std::move(owner), bytes};
}
} // namespace ldap
