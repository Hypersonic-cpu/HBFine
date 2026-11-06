#pragma once

#include "types.hpp"

namespace hbfine {
// Observes completion events without scheduling work or changing device state.
class BandwidthSampler {
public:
    BandwidthSampler(Time interval_ps, std::vector<BandwidthBin>& bins)
        : interval_(interval_ps),
          bins_(bins) {}

    void add(Time now, std::uint64_t BandwidthBin::* field,
             std::uint64_t value) {
        if (!interval_) return;
        const auto index = static_cast<std::size_t>(now / interval_);
        while (bins_.size() <= index) {
            const auto start = bins_.size() * interval_;
            bins_.push_back({start, start + interval_});
        }
        bins_[index].*field += value;
    }

private:
    Time interval_;
    std::vector<BandwidthBin>& bins_;
};
} // namespace hbfine
