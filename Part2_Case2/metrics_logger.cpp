#include "metrics_logger.h"

#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <sys/time.h>
#include <inttypes.h>

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void log_event(const char* tag, const char* fmt, ...) {
    pthread_mutex_lock(&log_mutex);
    
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    
    // 1. Output to stdout for real-time visibility without blocking stdin
    if (t) {
        printf("[%02d:%02d:%02d] [%s] ", t->tm_hour, t->tm_min, t->tm_sec, tag ? tag : "INFO");
    } else {
        printf("[00:00:00] [%s] ", tag ? tag : "INFO");
    }
    
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout); 

    // 2. Persist to it305_runtime.log as mandated by Project Plan Section 4 (Page 19)
    FILE* log_fp = fopen("it305_runtime.log", "a");
    if (log_fp) {
        if (t) {
            fprintf(log_fp, "[%02d:%02d:%02d] [%s] ", t->tm_hour, t->tm_min, t->tm_sec, tag ? tag : "INFO");
        } else {
            fprintf(log_fp, "[00:00:00] [%s] ", tag ? tag : "INFO");
        }
        va_list fargs;
        va_start(fargs, fmt);
        vfprintf(log_fp, fmt, fargs);
        va_end(fargs);
        fprintf(log_fp, "\n");
        fflush(log_fp);
        fclose(log_fp);
    }
    
    pthread_mutex_unlock(&log_mutex);
}

void record_transfer_metrics(const char* csv_filepath, const char* case_name, 
                             int num_clients, double pf, 
                             uint64_t useful_bytes, uint64_t total_bytes, uint64_t redundant_bytes, 
                             struct timespec t_start, struct timespec t_end, int retries) {
    if (!csv_filepath || !case_name) {
        return;
    }

    pthread_mutex_lock(&log_mutex);

    // Calculate elapsed duration in seconds: T_complete = (t_end - t_start)
    double duration_sec = (double)(t_end.tv_sec - t_start.tv_sec) + 
                          (double)(t_end.tv_nsec - t_start.tv_nsec) * 1e-9;
    if (duration_sec < 0.0) {
        duration_sec = 0.0;
    }
    
    // Compute the three core metrics defined in Section 9.1:
    // Goodput (MB/s) = B_useful / (T_complete * 10^6)
    double goodput_mbps = (duration_sec > 0.0) ? (useful_bytes / (duration_sec * 1000000.0)) : 0.0;
    
    // Wire Throughput (MB/s) = B_total / (T_complete * 10^6)
    double throughput_mbps = (duration_sec > 0.0) ? (total_bytes / (duration_sec * 1000000.0)) : 0.0;
    
    // Overhead (%) = ((B_total - B_useful) / B_useful) * 100
    double overhead_pct = (useful_bytes > 0) ? 
        (((double)total_bytes - (double)useful_bytes) / (double)useful_bytes) * 100.0 : 0.0;

    FILE* fp = fopen(csv_filepath, "a");
    if (fp) {
        // Appends: case_name,num_clients,pf,t_start_sec,t_end_sec,duration_sec,useful_bytes,total_bytes,redundant_bytes,goodput_mbps,throughput_mbps,overhead_pct,retries
        fprintf(fp, "%s,%d,%.2f,%lld,%lld,%.6f,%llu,%llu,%llu,%.6f,%.6f,%.2f,%d\n",
                case_name, num_clients, pf,
                (long long)t_start.tv_sec, (long long)t_end.tv_sec, duration_sec,
                (unsigned long long)useful_bytes, 
                (unsigned long long)total_bytes, 
                (unsigned long long)redundant_bytes,
                goodput_mbps, throughput_mbps, overhead_pct, retries);
        fflush(fp);
        fclose(fp);
    }

    pthread_mutex_unlock(&log_mutex);
}
