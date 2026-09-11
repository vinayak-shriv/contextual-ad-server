#include "adserve/tokenizer.h"

#include <cctype>
#include <string>
#include <unordered_set>

namespace adserve {

namespace {

const std::unordered_set<std::string_view>& stopwords() {
    // Function-local static: built once, thread-safe initialisation (C++11 guarantee).
    static const std::unordered_set<std::string_view> kStopwords = {
        "a",     "about", "above", "after", "again", "all",   "am",    "an",    "and",
        "any",   "are",   "as",    "at",    "be",    "been",  "before", "being", "below",
        "but",   "by",    "can",   "did",   "do",    "does",  "doing", "down",  "during",
        "each",  "few",   "for",   "from",  "had",   "has",   "have",  "having", "he",
        "her",   "here",  "hers",  "him",   "his",   "how",   "if",    "in",    "into",
        "is",    "it",    "its",   "just",  "me",    "more",  "most",  "my",    "no",
        "nor",   "not",   "now",   "of",    "off",   "on",    "once",  "only",  "or",
        "other", "our",   "out",   "over",  "own",   "same",  "she",   "should", "so",
        "some",  "such",  "than",  "that",  "the",   "their", "them",  "then",  "there",
        "these", "they",  "this",  "those", "through", "to",  "too",   "under", "until",
        "up",    "very",  "was",   "we",    "were",  "what",  "when",  "where", "which",
        "while", "who",   "whom",  "why",   "will",  "with",  "you",   "your",  "yours",
    };
    return kStopwords;
}

}  // namespace

bool is_stopword(std::string_view token) {
    return stopwords().count(token) > 0;
}

std::vector<std::string> tokenize(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;

    auto flush = [&] {
        if (current.size() >= 2 && !is_stopword(current)) {
            tokens.push_back(current);
        }
        current.clear();
    };

    for (char ch : text) {
        // Cast to unsigned char first: passing a negative char to isalnum is UB.
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            current.push_back(static_cast<char>(std::tolower(c)));
        } else {
            flush();
        }
    }
    flush();
    return tokens;
}

}  // namespace adserve
