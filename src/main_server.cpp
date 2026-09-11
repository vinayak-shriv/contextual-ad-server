// HTTP front-end for the ad engine.
//
//   POST /ad?user_id=u42&url=https://site/page&floor=1.5    body: page text (text/plain)
//   POST /click?ad_id=101
//   GET  /stats
//   GET  /health

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "adserve/ad_server.h"
#include "adserve/inventory_loader.h"
#include "httplib.h"

namespace {

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(ch));
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void send_error(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    res.set_content("{\"error\":\"" + json_escape(message) + "\"}", "application/json");
}

struct Options {
    std::string ads_path = "data/ads.csv";
    std::string campaigns_path = "data/campaigns.csv";
    std::string host = "0.0.0.0";
    int port = 8080;
    unsigned threads = std::max(2u, std::thread::hardware_concurrency());
};

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << arg << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--ads") o.ads_path = next();
        else if (arg == "--campaigns") o.campaigns_path = next();
        else if (arg == "--host") o.host = next();
        else if (arg == "--port") o.port = std::stoi(next());
        else if (arg == "--threads") o.threads = static_cast<unsigned>(std::stoul(next()));
        else {
            std::cerr << "usage: adserve_server [--ads PATH] [--campaigns PATH] [--host H] "
                         "[--port N] [--threads N]\n";
            std::exit(arg == "--help" ? 0 : 2);
        }
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    const Options opt = parse_args(argc, argv);

    std::vector<adserve::Ad> ads;
    std::vector<adserve::Campaign> campaigns;
    try {
        campaigns = adserve::load_campaigns(opt.campaigns_path);
        ads = adserve::load_ads(opt.ads_path);
    } catch (const std::exception& e) {
        std::cerr << "failed to load inventory: " << e.what() << "\n";
        return 1;
    }

    const std::size_t num_ads = ads.size();
    adserve::AdServer engine(std::move(ads), campaigns, adserve::ServerConfig{});

    httplib::Server svr;
    const unsigned threads = opt.threads;
    svr.new_task_queue = [threads] { return new httplib::ThreadPool(threads); };
    svr.set_payload_max_length(1 << 20);  // 1 MiB of page text is plenty

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    svr.Post("/ad", [&engine](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("user_id")) return send_error(res, 400, "user_id is required");

        adserve::AdRequest ad_req;
        ad_req.user_id = req.get_param_value("user_id");
        ad_req.page_url = req.get_param_value("url");
        ad_req.page_text = req.body;
        ad_req.now_ms = now_ms();
        if (req.has_param("floor")) {
            try {
                ad_req.floor_cpm = std::stod(req.get_param_value("floor"));
            } catch (const std::exception&) {
                return send_error(res, 400, "floor must be a number");
            }
            if (ad_req.floor_cpm < 0) return send_error(res, 400, "floor must be >= 0");
        }

        const adserve::AdResponse r = engine.serve(ad_req);

        std::ostringstream out;
        out << "{\"filled\":" << (r.filled ? "true" : "false");
        if (r.filled) {
            const adserve::Ad* ad = engine.find_ad(r.ad_id);
            out << ",\"ad_id\":" << r.ad_id << ",\"campaign_id\":" << r.campaign
                << ",\"creative\":\"" << json_escape(ad ? ad->creative : "") << "\""
                << ",\"price_cpm\":" << r.price_cpm << ",\"relevance\":" << r.relevance;
        } else {
            out << ",\"reason\":\"" << adserve::to_string(r.reason) << "\"";
        }
        out << ",\"cache_hit\":" << (r.cache_hit ? "true" : "false")
            << ",\"latency_us\":" << r.latency_us << "}";
        res.set_content(out.str(), "application/json");
    });

    svr.Post("/click", [&engine](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("ad_id")) return send_error(res, 400, "ad_id is required");
        adserve::AdId id = 0;
        try {
            id = static_cast<adserve::AdId>(std::stoul(req.get_param_value("ad_id")));
        } catch (const std::exception&) {
            return send_error(res, 400, "ad_id must be an integer");
        }
        if (!engine.record_click(id)) return send_error(res, 404, "unknown ad_id");
        res.set_content("{\"recorded\":true}", "application/json");
    });

    svr.Get("/stats", [&engine](const httplib::Request&, httplib::Response& res) {
        const adserve::StatsSnapshot s = engine.stats();
        std::ostringstream out;
        out << "{\"requests\":" << s.requests << ",\"filled\":" << s.filled
            << ",\"fill_rate\":" << s.fill_rate() << ",\"cache_hit_rate\":" << s.cache_hit_rate()
            << ",\"no_fill\":{\"no_relevant_ads\":" << s.no_relevant_ads
            << ",\"below_floor\":" << s.below_floor
            << ",\"capped_or_no_budget\":" << s.capped_or_no_budget << "}"
            << ",\"clicks\":" << s.clicks << ",\"latency_us\":{\"p50\":" << s.p50_us
            << ",\"p99\":" << s.p99_us << "}}";
        res.set_content(out.str(), "application/json");
    });

    std::cout << "loaded " << num_ads << " ads / " << campaigns.size() << " campaigns\n"
              << "listening on " << opt.host << ":" << opt.port << " with " << threads
              << " worker threads" << std::endl;
    if (!svr.listen(opt.host, opt.port)) {
        std::cerr << "failed to bind " << opt.host << ":" << opt.port << "\n";
        return 1;
    }
    return 0;
}
