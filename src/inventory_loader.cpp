#include "adserve/inventory_loader.h"

#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace adserve {

namespace {

std::string trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(first, last - first + 1));
}

// Splits on `sep` into at most `max_fields` pieces; the last piece keeps any remaining
// separators (so a creative like "Shoes, 20% off" survives).
std::vector<std::string> split(const std::string& line, char sep, std::size_t max_fields) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (fields.size() + 1 < max_fields) {
        const auto pos = line.find(sep, start);
        if (pos == std::string::npos) break;
        fields.push_back(trim(std::string_view(line).substr(start, pos - start)));
        start = pos + 1;
    }
    fields.push_back(trim(std::string_view(line).substr(start)));
    return fields;
}

[[noreturn]] void fail(const std::string& path, int line_no, const std::string& msg) {
    throw std::runtime_error(path + ":" + std::to_string(line_no) + ": " + msg);
}

template <typename Fn>
void for_each_data_line(const std::string& path, const char* header_prefix, Fn&& fn) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t.rfind(header_prefix, 0) == 0) continue;
        fn(t, line_no);
    }
}

std::uint32_t parse_id(const std::string& s, const std::string& path, int line_no) {
    try {
        std::size_t used = 0;
        const unsigned long v = std::stoul(s, &used);
        if (used != s.size()) fail(path, line_no, "invalid id '" + s + "'");
        return static_cast<std::uint32_t>(v);
    } catch (const std::logic_error&) {
        fail(path, line_no, "invalid id '" + s + "'");
    }
}

double parse_non_negative(const std::string& s, const std::string& path, int line_no) {
    try {
        std::size_t used = 0;
        const double v = std::stod(s, &used);
        if (used != s.size() || v < 0.0 || !std::isfinite(v)) {
            fail(path, line_no, "invalid amount '" + s + "'");
        }
        return v;
    } catch (const std::logic_error&) {
        fail(path, line_no, "invalid amount '" + s + "'");
    }
}

}  // namespace

std::vector<Campaign> load_campaigns(const std::string& path) {
    std::vector<Campaign> campaigns;
    std::unordered_set<CampaignId> seen;
    for_each_data_line(path, "campaign_id", [&](const std::string& line, int line_no) {
        const auto f = split(line, ',', 2);
        if (f.size() != 2) fail(path, line_no, "expected 2 fields");
        Campaign c;
        c.id = parse_id(f[0], path, line_no);
        c.daily_budget =
            static_cast<Micros>(std::llround(parse_non_negative(f[1], path, line_no) * kMicrosPerUnit));
        if (!seen.insert(c.id).second) fail(path, line_no, "duplicate campaign id");
        campaigns.push_back(c);
    });
    return campaigns;
}

std::vector<Ad> load_ads(const std::string& path) {
    std::vector<Ad> ads;
    std::unordered_set<AdId> seen;
    for_each_data_line(path, "ad_id", [&](const std::string& line, int line_no) {
        const auto f = split(line, ',', 5);
        if (f.size() != 5) fail(path, line_no, "expected 5 fields");
        Ad ad;
        ad.id = parse_id(f[0], path, line_no);
        ad.campaign = parse_id(f[1], path, line_no);
        ad.bid_cpm = parse_non_negative(f[2], path, line_no);
        for (auto& kw : split(f[3], ';', static_cast<std::size_t>(-1))) {
            if (!kw.empty()) ad.keywords.push_back(std::move(kw));
        }
        if (ad.keywords.empty()) fail(path, line_no, "ad has no keywords");
        ad.creative = f[4];
        if (!seen.insert(ad.id).second) fail(path, line_no, "duplicate ad id");
        ads.push_back(std::move(ad));
    });
    return ads;
}

}  // namespace adserve
