#include "math_utils.h"

double compute_average(const double* numbers, size_t count) {
    if (numbers == NULL || count == 0) {
        return 0.0;
    }
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        sum += numbers[i];
    }
    return sum / (double)count;
}

double find_peak_value(const double* numbers, size_t count) {
    if (numbers == NULL || count == 0) {
        return 0.0;
    }
    double max_val = numbers[0];
    for (size_t i = 1; i < count; ++i) {
        if (numbers[i] > max_val) {
            max_val = numbers[i];
        }
    }
    return max_val;
}
