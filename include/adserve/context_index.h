#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "adserve/types.h"

namespace adserve {

using TermId = std::uint32_t;

// A sparse, L2-normalised TF-IDF vector: (term id, weight) pairs sorted by term id.
// Sorted order lets two vectors be dot-multiplied with a linear merge.
using SparseVector = std::vector<std::pair<TermId, float>>;

struct Candidate {
    std::size_t ad_index = 0;  // position of the ad in the inventory vector
    float relevance = 0.0f;    // cosine similarity in [0, 1]
};

// Contextual matcher: represents every ad and every page as a TF-IDF vector
// and scores them by cosine similarity.
//
// Two retrieval strategies are provided so they can be benchmarked against each other:
//   * top_k()             -- inverted index: only ads sharing at least one term with the
//                            page are ever touched. Cost ~ sum of posting-list lengths.
//   * top_k_brute_force() -- scores every ad. Cost ~ number of ads. Kept as a baseline
//                            and as a test oracle for the indexed path.
//
// The index is immutable after construction, so concurrent reads need no locking.
class ContextIndex {
public:
    explicit ContextIndex(const std::vector<Ad>& ads);

    // Turns page text into a normalised TF-IDF vector. Terms that no ad uses are
    // dropped: they can never contribute to a match.
    SparseVector vectorize(std::string_view page_text) const;

    std::vector<Candidate> top_k(const SparseVector& page, std::size_t k, float min_relevance) const;
    std::vector<Candidate> top_k_brute_force(const SparseVector& page, std::size_t k,
                                             float min_relevance) const;

    std::size_t num_ads() const { return ad_vectors_.size(); }
    std::size_t vocabulary_size() const { return idf_.size(); }

private:
    struct Posting {
        std::uint32_t ad_index;
        float weight;
    };

    static float dot(const SparseVector& a, const SparseVector& b);
    static std::vector<Candidate> select_top_k(std::vector<Candidate> scored, std::size_t k);

    std::unordered_map<std::string, TermId> vocabulary_;
    std::vector<float> idf_;                     // indexed by TermId
    std::vector<std::vector<Posting>> postings_; // indexed by TermId
    std::vector<SparseVector> ad_vectors_;       // indexed by ad position
};

}  // namespace adserve
