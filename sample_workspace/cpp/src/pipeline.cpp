#include "pipeline.hpp"

#include <format>
#include <sstream>

namespace sample::engine {

ThresholdFilter::ThresholdFilter(double min_threshold, double max_threshold)
    : min_threshold_(min_threshold), max_threshold_(max_threshold) {}

bool ThresholdFilter::accept(double value) const {
    return value >= min_threshold_ && value <= max_threshold_;
}

FilterKind ThresholdFilter::kind() const noexcept {
    return FilterKind::Threshold;
}

std::string ThresholdFilter::description() const {
    std::ostringstream ss;
    ss << "ThresholdFilter[" << min_threshold_ << " .. " << max_threshold_ << "]";
    return ss.str();
}

MetricPipeline::MetricPipeline(std::unique_ptr<BaseFilter> filter, std::size_t buffer_capacity)
    : filter_(std::move(filter)), buffer_(buffer_capacity) {}

void MetricPipeline::feed(double value) {
    stats_.total_inputs++;
    if (!filter_ || filter_->accept(value)) {
        buffer_.push(value);
        stats_.passed_count++;
        stats_.sum_passed += value;
    }
}

void MetricPipeline::flush() {
    // Pipeline flush logic
}

const PipelineStats& MetricPipeline::stats() const noexcept {
    return stats_;
}

std::vector<double> MetricPipeline::drain_buffer() {
    std::vector<double> items;
    while (!buffer_.empty()) {
        auto val = buffer_.pop();
        if (val) {
            items.push_back(*val);
        }
    }
    return items;
}

} // namespace sample::engine
