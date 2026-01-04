#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sip_gateway {

class Metrics {
public:
    static Metrics& instance();

    void increment_request();
    void observe_response_time(const std::string& method, double seconds);
    void observe_response_summary(const std::string& method, double seconds);
    std::string render_prometheus() const;

private:
    struct SummarySeries {
        uint64_t count = 0;
        double sum = 0.0;
    };

    struct HistogramSeries {
        uint64_t count = 0;
        double sum = 0.0;
        std::vector<uint64_t> buckets;
    };

    Metrics();

    HistogramSeries& histogram_for(const std::string& method);
    SummarySeries& summary_for(const std::string& method);

    mutable std::mutex mutex_;
    uint64_t request_total_ = 0;
    std::unordered_map<std::string, SummarySeries> response_summaries_;
    std::unordered_map<std::string, HistogramSeries> response_histograms_;
    std::vector<double> histogram_bounds_;
};

}
