#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace adserve {

// Online click-through-rate estimate per ad, learned from impression/click feedback.
//
// Raw clicks / impressions is useless for new ads (0/0, or 1/1 = 100% after one lucky
// click). We use a Beta prior instead ("Bayesian smoothing"):
//
//   pCTR = (clicks + alpha) / (impressions + alpha + beta)
//   alpha = prior_ctr * prior_strength,  beta = (1 - prior_ctr) * prior_strength
//
// With no data pCTR equals the prior; as real impressions accumulate the estimate moves
// toward the observed rate. prior_strength is how many "virtual impressions" the prior
// is worth.
//
// Counters are relaxed atomics: each is independent and a slightly stale read only
// shifts the estimate by one event, which is harmless for ranking.
class CtrEstimator {
public:
    CtrEstimator(std::size_t num_ads, double prior_ctr = 0.01, double prior_strength = 200.0);

    void record_impression(std::size_t ad_index);
    void record_click(std::size_t ad_index);

    double predicted_ctr(std::size_t ad_index) const;

    // pCTR divided by the prior, so a brand-new ad has factor 1.0 and an ad that
    // performs twice as well as average has factor ~2.0. Used in the quality score.
    double quality_factor(std::size_t ad_index) const;

    std::uint64_t impressions(std::size_t ad_index) const;
    std::uint64_t clicks(std::size_t ad_index) const;

private:
    std::size_t num_ads_;
    double prior_ctr_;
    double alpha_;
    double beta_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> impressions_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> clicks_;
};

}  // namespace adserve
