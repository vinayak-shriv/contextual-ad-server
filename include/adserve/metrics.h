#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "adserve/types.h"

namespace adserve {

// Fixed-bucket latency histogram. Recording is one relaxed atomic increment, so it is
// cheap enough to call on every request from every thread. Percentiles are approximate:
// they report the upper bound of the bucket the percentile falls in.
class LatencyHistogram {
public:
    // Upper bounds in microseconds; the final bucket catches everything slower.
    static constexpr std::array<std::int64_t, 14> kBoundsUs = {
        5, 10, 20, 50, 100, 200, 500, 1'000, 2'000, 5'000, 10'000, 50'000, 100'000, 1'000'000};

    void record(std::int64_t latency_us);
    std::int64_t percentile_us(double p) const;  // p in [0, 100]
    std::uint64_t count() const;

private:
    std::array<std::atomic<std::uint64_t>, kBoundsUs.size() + 1> buckets_{};
};

struct StatsSnapshot {
    std::uint64_t requests = 0;
    std::uint64_t filled = 0;
    std::uint64_t cache_hits = 0;
    std::uint64_t no_relevant_ads = 0;
    std::uint64_t below_floor = 0;
    std::uint64_t capped_or_no_budget = 0;
    std::uint64_t clicks = 0;
    std::int64_t p50_us = 0;
    std::int64_t p99_us = 0;
    double fill_rate() const {
        return requests ? static_cast<double>(filled) / static_cast<double>(requests) : 0.0;
    }
    double cache_hit_rate() const {
        return requests ? static_cast<double>(cache_hits) / static_cast<double>(requests) : 0.0;
    }
};

class Metrics {
public:
    void record(const AdResponse& response);
    void record_click();
    StatsSnapshot snapshot() const;

private:
    std::atomic<std::uint64_t> requests_{0};
    std::atomic<std::uint64_t> filled_{0};
    std::atomic<std::uint64_t> cache_hits_{0};
    std::atomic<std::uint64_t> no_relevant_{0};
    std::atomic<std::uint64_t> below_floor_{0};
    std::atomic<std::uint64_t> capped_{0};
    std::atomic<std::uint64_t> clicks_{0};
    LatencyHistogram latency_;
};

}  // namespace adserve
