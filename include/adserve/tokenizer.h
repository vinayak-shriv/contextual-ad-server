#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace adserve {

// Splits text into lowercase alphanumeric tokens.
// Drops tokens shorter than 2 characters and common English stopwords,
// since words like "the" or "and" carry no contextual signal.
std::vector<std::string> tokenize(std::string_view text);

bool is_stopword(std::string_view token);

}  // namespace adserve
