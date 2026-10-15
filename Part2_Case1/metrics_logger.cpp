#include "metrics_logger.h"
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <sys/time.h>

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void log_event(const char* tag, const char* fmt, ...) {
    pthread_mutex_lock(&log_mutex);
    
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    
    // Print to stdout to serve as background logging without blocking interactive prompts
    printf("[%02d:%02d:%02d] [%s] ", t->tm_hour, t->tm_min, t->tm_sec, tag);
    
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    
    // Ensure the auto-grader doesn't buffer and hang
    fflush(stdout); 
    
    pthread_mutex_unlock(&log_mutex);
}

void record_transfer_metrics(const char* csv_filepath, const char* case_name, 
                             int num_clients, double pf, 
                             uint64_t useful_bytes, uint64_t total_bytes, uint64_t redundant_bytes, 
                             struct timespec t_start, struct timespec t_end, int retries) {
    
    pthread_mutex_lock(&log_mutex);

    // Calculate elapsed wall-clock duration in seconds
    double duration_sec = (t_end.tv_sec - t_start.tv_sec) + (t_end.tv_nsec - t_start.tv_nsec) * 1e-9;
    
    // Compute performance metrics (10^6 bytes = 1 MB)
    double goodput_mbps = (duration_sec > 0) ? (useful_bytes / (duration_sec * 1000000.0)) : 0;
    double throughput_mbps = (duration_sec > 0) ? (total_bytes / (duration_sec * 1000000.0)) : 0;
    double overhead_pct = (useful_bytes > 0) ? ((double)(total_bytes - useful_bytes) / useful_bytes) * 100.0 : 0;

    FILE* fp = fopen(csv_filepath, "a");
    if (fp) {
        // Appends: case_name, num_clients, pf, t_start, t_end, duration, useful, total, redundant, goodput, throughput, overhead, retries
        fprintf(fp, "%s,%d,%.2f,%ld,%ld,%.6f,%lu,%lu,%lu,%.6f,%.6f,%.2f,%d\n",
                case_name, num_clients, pf,
                t_start.tv_sec, t_end.tv_sec, duration_sec,
                useful_bytes, total_bytes, redundant_bytes,
                goodput_mbps, throughput_mbps, overhead_pct, retries);
        fclose(fp);
    }

    pthread_mutex_unlock(&log_mutex);
}