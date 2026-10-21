#ifndef METRICS_LOGGER_H
#define METRICS_LOGGER_H

#include <stdint.h>
#include <time.h>

// Formats a timestamped log line protected by a mutex to avoid interleaved output.
// Writes to it305_runtime.log and stdout without blocking interactive stdin prompts.
void log_event(const char* tag, const char* fmt, ...);

// Sets the active case name for logging and CSV routing (e.g. "Part1_Single", "Part2_Case1", etc.)
void set_metrics_case_name(const char* case_name);

// Computes throughput/overhead and appends a thread-safe CSV row for analytics.
void record_transfer_metrics(const char* csv_filepath, const char* case_name, 
                             int num_clients, double pf, 
                             uint64_t useful_bytes, uint64_t total_bytes, uint64_t redundant_bytes, 
                             struct timespec t_start, struct timespec t_end, int retries);

#endif // METRICS_LOGGER_H
