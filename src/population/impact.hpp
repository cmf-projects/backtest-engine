#pragma once

#include "population/order.hpp"
#include "population/types.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace population {

struct ImpactKey {
    Side side = Side::Unknown;
    long long price_ticks = 0;

    [[nodiscard]] bool operator==(const ImpactKey& other) const noexcept {
        return side == other.side && price_ticks == other.price_ticks;
    }
};

struct ImpactKeyHash {
    [[nodiscard]] std::size_t operator()(const ImpactKey& key) const noexcept {
        std::size_t h = std::hash<long long>{}(key.price_ticks);
        // boost::hash_combine(h, side)
        h ^= static_cast<std::size_t>(key.side) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

// Per-slot eaten liquidity. λ == 0 → all ops no-op (impact off).
class ImpactState {
public:
    explicit ImpactState(double lambda = 0.0) : lambda_(lambda) {}

    [[nodiscard]] double lambda() const noexcept { return lambda_; }
    [[nodiscard]] bool enabled() const noexcept { return lambda_ > 0.0; }

    // On file step at t_new: recover eaten on levels still in file; drop missing.
    void Recover(const std::vector<Level>& file_bids, const std::vector<Level>& file_asks,
                 std::uint64_t t_new_ns, double tick_size);

    // effective_qty = max(0, file - eaten); mutates level copies in place.
    void ApplyTo(std::vector<Level>& bids, std::vector<Level>& asks, double tick_size) const;

    // After a fill at match time t_ns. `order_side` = aggressor; eaten is stored
    // on the opposite book side (buy lifts asks, sell hits bids).
    void OnFill(Side order_side, long long price_ticks, std::uint64_t qty,
                std::uint64_t t_ns);

    [[nodiscard]] double EatenAt(Side side, long long price_ticks) const;

private:
    struct LevelState {
        double eaten = 0.0;
        std::uint64_t t_last_ns = 0;
    };

    void ApplySide(Side side, std::vector<Level>& levels, double tick_size) const;

    double lambda_ = 0.0;
    std::unordered_map<ImpactKey, LevelState, ImpactKeyHash> levels_;
};

}  // namespace population
