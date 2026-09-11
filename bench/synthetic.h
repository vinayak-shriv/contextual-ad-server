#pragma once

// Deterministic synthetic workload: ads grouped into topics, pages that mostly talk about
// one topic, and a request stream where page popularity follows a Zipf distribution
// (a few pages get most of the traffic, as on real sites). Same seed -> same workload.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "adserve/types.h"

namespace synthetic {

struct Config {
    std::size_t num_ads = 10'000;
    std::size_t num_campaigns = 500;
    std::size_t num_topics = 200;
    std::size_t words_per_topic = 40;
    std::size_t general_vocab = 5'000;
    std::size_t num_pages = 20'000;
    std::size_t words_per_page = 300;
    std::size_t num_users = 100'000;
    double zipf_s = 1.0;
    double campaign_budget = 1e9;  // effectively unlimited unless a test lowers it
    std::uint64_t seed = 42;
};

struct Page {
    std::string url;
    std::string text;
};

struct Workload {
    std::vector<adserve::Ad> ads;
    std::vector<adserve::Campaign> campaigns;
    std::vector<Page> pages;
};

inline std::string topic_word(std::size_t topic, std::size_t i) {
    return "t" + std::to_string(topic) + "w" + std::to_string(i);
}

inline std::string general_word(std::size_t i) { return "g" + std::to_string(i); }

inline Workload generate(const Config& cfg) {
    std::mt19937_64 rng(cfg.seed);
    auto uniform = [&](std::size_t n) {
        return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
    };

    Workload w;
    for (std::size_t c = 0; c < cfg.num_campaigns; ++c) {
        w.campaigns.push_back({static_cast<adserve::CampaignId>(c + 1),
                               static_cast<adserve::Micros>(cfg.campaign_budget *
                                                            adserve::kMicrosPerUnit)});
    }

    std::uniform_real_distribution<double> bid_dist(1.0, 10.0);
    std::uniform_int_distribution<int> kw_count(4, 8);
    for (std::size_t i = 0; i < cfg.num_ads; ++i) {
        adserve::Ad ad;
        ad.id = static_cast<adserve::AdId>(i + 1);
        ad.campaign = static_cast<adserve::CampaignId>(i % cfg.num_campaigns + 1);
        ad.bid_cpm = std::round(bid_dist(rng) * 100.0) / 100.0;
        const std::size_t topic = uniform(cfg.num_topics);
        const int n = kw_count(rng);
        for (int k = 0; k < n; ++k) ad.keywords.push_back(topic_word(topic, uniform(cfg.words_per_topic)));
        if (uniform(2) == 0) ad.keywords.push_back(general_word(uniform(cfg.general_vocab)));
        ad.creative = "synthetic ad " + std::to_string(ad.id);
        w.ads.push_back(std::move(ad));
    }

    static const char* kFiller[] = {"the", "and", "of", "to", "in", "is", "for", "with", "on", "this"};
    for (std::size_t p = 0; p < cfg.num_pages; ++p) {
        const std::size_t topic = uniform(cfg.num_topics);
        std::string text;
        text.reserve(cfg.words_per_page * 7);
        for (std::size_t k = 0; k < cfg.words_per_page; ++k) {
            const std::size_t roll = uniform(10);
            if (roll < 2) text += topic_word(topic, uniform(cfg.words_per_topic));  // 20% on-topic
            else if (roll < 5) text += kFiller[uniform(10)];                         // 30% stopwords
            else text += general_word(uniform(cfg.general_vocab));                  // 50% noise
            text += ' ';
        }
        w.pages.push_back({"https://site" + std::to_string(p % 50) + ".example/page/" +
                               std::to_string(p),
                           std::move(text)});
    }
    return w;
}

// Samples page indices with P(rank r) proportional to 1 / r^s.
class ZipfSampler {
public:
    ZipfSampler(std::size_t n, double s, std::uint64_t seed) : rng_(seed) {
        cdf_.resize(n);
        double sum = 0.0;
        for (std::size_t r = 0; r < n; ++r) {
            sum += 1.0 / std::pow(static_cast<double>(r + 1), s);
            cdf_[r] = sum;
        }
        for (auto& v : cdf_) v /= sum;
    }

    std::size_t next() {
        const double u = std::uniform_real_distribution<double>(0.0, 1.0)(rng_);
        const auto it = std::lower_bound(cdf_.begin(), cdf_.end(), u);
        return static_cast<std::size_t>(std::min<std::ptrdiff_t>(it - cdf_.begin(),
                                                                 static_cast<std::ptrdiff_t>(cdf_.size()) - 1));
    }

private:
    std::mt19937_64 rng_;
    std::vector<double> cdf_;
};

}  // namespace synthetic
