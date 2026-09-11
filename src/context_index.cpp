#include "adserve/context_index.h"

#include <algorithm>
#include <cmath>
#include <queue>

#include "adserve/tokenizer.h"

namespace adserve {

namespace {

// Sublinear term frequency: a word appearing 10 times is more relevant than once,
// but not 10x more. Keeps long, repetitive pages from dominating.
float tf_weight(int count) {
    return 1.0f + std::log(static_cast<float>(count));
}

void normalize(SparseVector& v) {
    double norm_sq = 0.0;
    for (const auto& [term, w] : v) norm_sq += static_cast<double>(w) * w;
    if (norm_sq == 0.0) return;
    const float inv = static_cast<float>(1.0 / std::sqrt(norm_sq));
    for (auto& entry : v) entry.second *= inv;
}

}  // namespace

ContextIndex::ContextIndex(const std::vector<Ad>& ads) {
    // Pass 1: tokenise each ad's keywords, assign term ids, count document frequency.
    std::vector<std::unordered_map<TermId, int>> ad_term_counts(ads.size());
    std::vector<int> doc_freq;

    for (std::size_t i = 0; i < ads.size(); ++i) {
        for (const auto& keyword : ads[i].keywords) {
            for (auto& token : tokenize(keyword)) {
                auto [it, inserted] = vocabulary_.emplace(std::move(token),
                                                          static_cast<TermId>(doc_freq.size()));
                if (inserted) doc_freq.push_back(0);
                int& count = ad_term_counts[i][it->second];
                if (count == 0) ++doc_freq[it->second];  // first time this ad uses the term
                ++count;
            }
        }
    }

    // Smoothed IDF: rare terms ("trekking") score higher than common ones ("sale").
    const auto n = static_cast<float>(ads.size());
    idf_.resize(doc_freq.size());
    for (std::size_t t = 0; t < doc_freq.size(); ++t) {
        idf_[t] = std::log((1.0f + n) / (1.0f + static_cast<float>(doc_freq[t]))) + 1.0f;
    }

    // Pass 2: build normalised ad vectors and the inverted index (term -> ads using it).
    postings_.resize(doc_freq.size());
    ad_vectors_.resize(ads.size());
    for (std::size_t i = 0; i < ads.size(); ++i) {
        SparseVector& vec = ad_vectors_[i];
        for (const auto& [term, count] : ad_term_counts[i]) {
            vec.emplace_back(term, tf_weight(count) * idf_[term]);
        }
        std::sort(vec.begin(), vec.end());
        normalize(vec);
        for (const auto& [term, w] : vec) {
            postings_[term].push_back({static_cast<std::uint32_t>(i), w});
        }
    }
}

SparseVector ContextIndex::vectorize(std::string_view page_text) const {
    std::unordered_map<TermId, int> counts;
    for (const auto& token : tokenize(page_text)) {
        auto it = vocabulary_.find(token);
        if (it != vocabulary_.end()) ++counts[it->second];
    }

    SparseVector vec;
    vec.reserve(counts.size());
    for (const auto& [term, count] : counts) {
        vec.emplace_back(term, tf_weight(count) * idf_[term]);
    }
    std::sort(vec.begin(), vec.end());
    normalize(vec);
    return vec;
}

float ContextIndex::dot(const SparseVector& a, const SparseVector& b) {
    // Linear merge of two term-sorted lists: O(|a| + |b|).
    float sum = 0.0f;
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].first < b[j].first) {
            ++i;
        } else if (a[i].first > b[j].first) {
            ++j;
        } else {
            sum += a[i].second * b[j].second;
            ++i;
            ++j;
        }
    }
    return sum;
}

std::vector<Candidate> ContextIndex::select_top_k(std::vector<Candidate> scored, std::size_t k) {
    // "Better" = higher relevance; ties broken by lower ad index so results are deterministic.
    auto better = [](const Candidate& x, const Candidate& y) {
        if (x.relevance != y.relevance) return x.relevance > y.relevance;
        return x.ad_index < y.ad_index;
    };

    // Min-heap of size k (top of heap = worst of the current best k): O(n log k),
    // cheaper than fully sorting all n scored ads when k << n.
    std::priority_queue<Candidate, std::vector<Candidate>, decltype(better)> heap(better);
    for (const auto& c : scored) {
        if (heap.size() < k) {
            heap.push(c);
        } else if (k > 0 && better(c, heap.top())) {
            heap.pop();
            heap.push(c);
        }
    }

    std::vector<Candidate> result;
    result.reserve(heap.size());
    while (!heap.empty()) {
        result.push_back(heap.top());
        heap.pop();
    }
    std::reverse(result.begin(), result.end());  // best first
    return result;
}

std::vector<Candidate> ContextIndex::top_k(const SparseVector& page, std::size_t k,
                                           float min_relevance) const {
    // Scratch buffers are thread_local so concurrent requests never share them and
    // we avoid allocating a fresh score array on every request.
    thread_local std::vector<float> scores;
    thread_local std::vector<std::uint32_t> touched;
    if (scores.size() < ad_vectors_.size()) scores.resize(ad_vectors_.size(), 0.0f);
    touched.clear();

    // Accumulate partial dot products, walking only the posting lists of the page's terms.
    for (const auto& [term, page_weight] : page) {
        for (const Posting& p : postings_[term]) {
            if (scores[p.ad_index] == 0.0f) touched.push_back(p.ad_index);
            scores[p.ad_index] += page_weight * p.weight;
        }
    }

    std::vector<Candidate> scored;
    scored.reserve(touched.size());
    for (std::uint32_t ad : touched) {
        if (scores[ad] >= min_relevance) scored.push_back({ad, scores[ad]});
        scores[ad] = 0.0f;  // reset for the next request on this thread
    }
    return select_top_k(std::move(scored), k);
}

std::vector<Candidate> ContextIndex::top_k_brute_force(const SparseVector& page, std::size_t k,
                                                       float min_relevance) const {
    std::vector<Candidate> scored;
    for (std::size_t i = 0; i < ad_vectors_.size(); ++i) {
        const float score = dot(page, ad_vectors_[i]);
        if (score > 0.0f && score >= min_relevance) scored.push_back({i, score});
    }
    return select_top_k(std::move(scored), k);
}

}  // namespace adserve
