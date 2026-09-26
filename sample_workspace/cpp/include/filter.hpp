#pragma once

#include <string>

namespace sample::engine {

enum class FilterKind {
    PassThrough,
    Threshold,
    BandPass
};

class BaseFilter {
public:
    virtual ~BaseFilter() = default;
    [[nodiscard]] virtual bool accept(double value) const = 0;
    [[nodiscard]] virtual FilterKind kind() const noexcept = 0;
    [[nodiscard]] virtual std::string description() const = 0;
};

class ThresholdFilter : public BaseFilter {
public:
    explicit ThresholdFilter(double min_threshold, double max_threshold);

    [[nodiscard]] bool accept(double value) const override;
    [[nodiscard]] FilterKind kind() const noexcept override;
    [[nodiscard]] std::string description() const override;

    [[nodiscard]] double min_threshold() const noexcept {
        return min_threshold_;
    }

    [[nodiscard]] double max_threshold() const noexcept {
        return max_threshold_;
    }

private:
    double min_threshold_;
    double max_threshold_;
};

} // namespace sample::engine
