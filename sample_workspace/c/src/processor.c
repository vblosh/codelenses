#include "common.h"
#include "math_utils.h"

#include <stdio.h>
#include <string.h>

int g_total_records_processed = 0;

MetricRecord init_metric(int id, const char* name, double value) {
    MetricRecord record;
    record.id = id;
    record.value = CLAMP_VALUE(value, -1000.0, 1000.0);
    record.status = METRIC_STATUS_OK;

    if (name != NULL) {
        strncpy(record.name, name, MAX_METRIC_NAME - 1);
        record.name[MAX_METRIC_NAME - 1] = '\0';
    } else {
        record.name[0] = '\0';
    }

    g_total_records_processed++;
    return record;
}

MetricStatus evaluate_metric(const MetricRecord* record, double threshold) {
    if (record == NULL) {
        return METRIC_STATUS_ERROR;
    }
    if (record->value > threshold * 1.5) {
        return METRIC_STATUS_ERROR;
    } else if (record->value > threshold) {
        return METRIC_STATUS_WARN;
    }
    return METRIC_STATUS_OK;
}

void print_metric_summary(const MetricRecord* record) {
    if (record == NULL) {
        return;
    }
    printf("Metric [%d] %s: %.2f (status: %d)\n", record->id, record->name, record->value,
           (int)record->status);
}
