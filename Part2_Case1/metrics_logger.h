#ifndef METRICS_LOGGER_H
#define METRICS_LOGGER_H

#include <stdint.h>
#include <time.h>

// Formats a timestamped log line protected by a mutex to avoid interleaved output.
void log_event(const char* tag, const char* fmt, ...);

// Computes throughput/overhead and appends a thread-safe CSV row for analytics.
void record_transfer_metrics(const char* csv_filepath, const char* case_name, 
                             int num_clients, double pf, 
                             uint64_t useful_bytes, uint64_t total_bytes, uint64_t redundant_bytes, 
                             struct timespec t_start, struct timespec t_end, int retries);

#endif // METRICS_LOGGER_H