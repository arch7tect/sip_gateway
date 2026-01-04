#include "sip_gateway/metrics.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace sip_gateway {

Metrics& Metrics::instance() {
    static Metrics metrics;
    return metrics;
}

Metrics::Metrics() {
    histogram_bounds_ = {0.005, 0.01, 0.025, 0.05, 0.075, 0.1, 0.25, 0.5,
                         0.75, 1.0, 2.5, 5.0, 7.5, 10.0};
}

void Metrics::increment_request() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++request_total_;
}

Metrics::HistogramSeries& Metrics::histogram_for(const std::string& method) {
    auto& series = response_histograms_[method];
    if (series.buckets.empty()) {
        series.buckets.assign(histogram_bounds_.size() + 1, 0);
    }
    return series;
}

Metrics::SummarySeries& Metrics::summary_for(const std::string& method) {
    return response_summaries_[method];
}

void Metrics::observe_response_time(const std::string& method, double seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& histogram = histogram_for(method);
    histogram.count += 1;
    histogram.sum += seconds;
    for (size_t i = 0; i < histogram_bounds_.size(); ++i) {
        if (seconds <= histogram_bounds_[i]) {
            histogram.buckets[i] += 1;
        }
    }
    histogram.buckets.back() += 1;
}

void Metrics::observe_response_summary(const std::string& method, double seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& summary = summary_for(method);
    summary.count += 1;
    summary.sum += seconds;
}

std::string Metrics::render_prometheus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out << std::setprecision(6);

    out << "# HELP client_requests_total Total number of client requests\n";
    out << "# TYPE client_requests_total counter\n";
    out << "client_requests_total " << request_total_ << "\n";

    out << "# HELP client_response_summary Time elapsed for response\n";
    out << "# TYPE client_response_summary summary\n";

    std::vector<std::string> methods;
    methods.reserve(response_summaries_.size());
    for (const auto& item : response_summaries_) {
        methods.push_back(item.first);
    }
    std::sort(methods.begin(), methods.end());
    for (const auto& method : methods) {
        const auto& series = response_summaries_.at(method);
        out << "client_response_summary_count{method=\"" << method << "\"} "
            << series.count << "\n";
        out << "client_response_summary_sum{method=\"" << method << "\"} "
            << series.sum << "\n";
    }

    out << "# HELP response_time_milliseconds Response time in milliseconds\n";
    out << "# TYPE response_time_milliseconds histogram\n";
    methods.clear();
    methods.reserve(response_histograms_.size());
    for (const auto& item : response_histograms_) {
        methods.push_back(item.first);
    }
    std::sort(methods.begin(), methods.end());
    for (const auto& method : methods) {
        const auto& series = response_histograms_.at(method);
        for (size_t i = 0; i < histogram_bounds_.size(); ++i) {
            out << "response_time_milliseconds_bucket{method=\"" << method
                << "\",le=\"" << histogram_bounds_[i] << "\"} "
                << series.buckets[i] << "\n";
        }
        out << "response_time_milliseconds_bucket{method=\"" << method
            << "\",le=\"+Inf\"} " << series.buckets.back() << "\n";
        out << "response_time_milliseconds_count{method=\"" << method << "\"} "
            << series.count << "\n";
        out << "response_time_milliseconds_sum{method=\"" << method << "\"} "
            << series.sum << "\n";
    }

    return out.str();
}

}
