#ifndef SAMPLE_COMMON_H
#define SAMPLE_COMMON_H

#include <stddef.h>

#define BUFFER_SIZE 1024
#define MAX_METRIC_NAME 64
#define CLAMP_VALUE(val, min_v, max_v) (((val) < (min_v)) ? (min_v) : (((val) > (max_v)) ? (max_v) : (val)))

typedef enum {
    METRIC_STATUS_OK = 0,
    METRIC_STATUS_WARN = 1,
    METRIC_STATUS_ERROR = 2
} MetricStatus;

typedef struct {
    int id;
    char name[MAX_METRIC_NAME];
    double value;
    MetricStatus status;
} MetricRecord;

typedef struct {
    MetricRecord records[BUFFER_SIZE];
    size_t count;
    double threshold;
} MetricCollection;

MetricRecord init_metric(int id, const char* name, double value);
MetricStatus evaluate_metric(const MetricRecord* record, double threshold);
void print_metric_summary(const MetricRecord* record);

#endif // SAMPLE_COMMON_H
