#include "adserve/metrics.h"

#include <cmath>

namespace adserve {

void LatencyHistogram::record(std::int64_t latency_us) {
    std::size_t i = 0;
    while (i < kBoundsUs.size() && latency_us > kBoundsUs[i]) ++i;
    buckets_[i].fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t LatencyHistogram::count() const {
    std::uint64_t total = 0;
    for (const auto& b : buckets_) total += b.load(std::memory_order_relaxed);
    return total;
}

std::int64_t LatencyHistogram::percentile_us(double p) const {
    const std::uint64_t total = count();
    if (total == 0) return 0;
    // Rank of the sample we want (1-based), e.g. p99 of 1000 samples -> the 990th.
    const auto target = static_cast<std::uint64_t>(std::ceil(p / 100.0 * static_cast<double>(total)));
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < buckets_.size(); ++i) {
        seen += buckets_[i].load(std::memory_order_relaxed);
        if (seen >= target && seen > 0) {
            return i < kBoundsUs.size() ? kBoundsUs[i] : kBoundsUs.back();
        }
    }
    return kBoundsUs.back();
}

void Metrics::record(const AdResponse& r) {
    requests_.fetch_add(1, std::memory_order_relaxed);
    if (r.cache_hit) cache_hits_.fetch_add(1, std::memory_order_relaxed);
    switch (r.reason) {
        case NoFillReason::None: filled_.fetch_add(1, std::memory_order_relaxed); break;
        case NoFillReason::NoRelevantAds: no_relevant_.fetch_add(1, std::memory_order_relaxed); break;
        case NoFillReason::BelowFloor: below_floor_.fetch_add(1, std::memory_order_relaxed); break;
        case NoFillReason::CappedOrNoBudget: capped_.fetch_add(1, std::memory_order_relaxed); break;
    }
    latency_.record(r.latency_us);
}

void Metrics::record_click() { clicks_.fetch_add(1, std::memory_order_relaxed); }

StatsSnapshot Metrics::snapshot() const {
    StatsSnapshot s;
    s.requests = requests_.load(std::memory_order_relaxed);
    s.filled = filled_.load(std::memory_order_relaxed);
    s.cache_hits = cache_hits_.load(std::memory_order_relaxed);
    s.no_relevant_ads = no_relevant_.load(std::memory_order_relaxed);
    s.below_floor = below_floor_.load(std::memory_order_relaxed);
    s.capped_or_no_budget = capped_.load(std::memory_order_relaxed);
    s.clicks = clicks_.load(std::memory_order_relaxed);
    s.p50_us = latency_.percentile_us(50.0);
    s.p99_us = latency_.percentile_us(99.0);
    return s;
}

}  // namespace adserve
