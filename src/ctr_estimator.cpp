#include "adserve/ctr_estimator.h"

namespace adserve {

CtrEstimator::CtrEstimator(std::size_t num_ads, double prior_ctr, double prior_strength)
    : num_ads_(num_ads),
      prior_ctr_(prior_ctr),
      alpha_(prior_ctr * prior_strength),
      beta_((1.0 - prior_ctr) * prior_strength),
      impressions_(new std::atomic<std::uint64_t>[num_ads]),
      clicks_(new std::atomic<std::uint64_t>[num_ads]) {
    for (std::size_t i = 0; i < num_ads; ++i) {
        impressions_[i].store(0, std::memory_order_relaxed);
        clicks_[i].store(0, std::memory_order_relaxed);
    }
}

void CtrEstimator::record_impression(std::size_t ad_index) {
    if (ad_index < num_ads_) impressions_[ad_index].fetch_add(1, std::memory_order_relaxed);
}

void CtrEstimator::record_click(std::size_t ad_index) {
    if (ad_index < num_ads_) clicks_[ad_index].fetch_add(1, std::memory_order_relaxed);
}

double CtrEstimator::predicted_ctr(std::size_t ad_index) const {
    if (ad_index >= num_ads_) return prior_ctr_;
    const auto imps = static_cast<double>(impressions_[ad_index].load(std::memory_order_relaxed));
    const auto clk = static_cast<double>(clicks_[ad_index].load(std::memory_order_relaxed));
    return (clk + alpha_) / (imps + alpha_ + beta_);
}

double CtrEstimator::quality_factor(std::size_t ad_index) const {
    return predicted_ctr(ad_index) / prior_ctr_;
}

std::uint64_t CtrEstimator::impressions(std::size_t ad_index) const {
    return ad_index < num_ads_ ? impressions_[ad_index].load(std::memory_order_relaxed) : 0;
}

std::uint64_t CtrEstimator::clicks(std::size_t ad_index) const {
    return ad_index < num_ads_ ? clicks_[ad_index].load(std::memory_order_relaxed) : 0;
}

}  // namespace adserve
