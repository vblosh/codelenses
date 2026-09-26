#pragma once

#include "buffer.hpp"
#include "filter.hpp"

#include <memory>
#include <string>
#include <vector>

namespace sample::engine {

struct PipelineStats {
    std::size_t total_inputs{0};
    std::size_t passed_count{0};
    double sum_passed{0.0};
};

class MetricPipeline {
public:
    explicit MetricPipeline(std::unique_ptr<BaseFilter> filter, std::size_t buffer_capacity = 256);

    void feed(double value);
    void flush();

    [[nodiscard]] const PipelineStats& stats() const noexcept;
    [[nodiscard]] std::vector<double> drain_buffer();

private:
    std::unique_ptr<BaseFilter> filter_;
    CircularBuffer<double> buffer_;
    PipelineStats stats_;
};

} // namespace sample::engine
