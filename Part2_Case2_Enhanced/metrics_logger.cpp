#include "metrics_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <inttypes.h>

#ifndef PART_NUM
#define PART_NUM 1
#endif

#ifndef CASE_MODE
#define CASE_MODE 0
#endif

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_active_case_name[64] = "";

void set_metrics_case_name(const char* case_name) {
    if (case_name && case_name[0] != '\0') {
        pthread_mutex_lock(&log_mutex);
        snprintf(g_active_case_name, sizeof(g_active_case_name), "%s", case_name);
        pthread_mutex_unlock(&log_mutex);
    }
}

static const char* get_results_dir() {
    if (access("Results", F_OK) == 0) {
        return "Results";
    }
    if (access("../Results", F_OK) == 0) {
        return "../Results";
    }
    return nullptr;
}

static const char* get_current_case_name() {
    if (g_active_case_name[0] != '\0') {
        return g_active_case_name;
    }
    const char* env_case = getenv("CASE_NAME");
    if (env_case && env_case[0] != '\0') {
        return env_case;
    }
#if PART_NUM == 1
    return "Part1_Multi";
#elif PART_NUM == 2
  #if CASE_MODE == 1
    return "Part2_Case1";
  #elif CASE_MODE == 2
    return "Part2_Case2";
  #elif CASE_MODE == 3
    return "Part2_Case2_Enhanced";
  #else
    return "Part2";
  #endif
#else
    return "Part1";
#endif
}

static void append_to_file(const char* filepath, const char* content) {
    if (!filepath || !content) return;
    FILE* fp = fopen(filepath, "a");
    if (fp) {
        fputs(content, fp);
        fflush(fp);
        fclose(fp);
    }
}

void log_event(const char* tag, const char* fmt, ...) {
    pthread_mutex_lock(&log_mutex);
    
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    
    char time_buf[64];
    if (t) {
        snprintf(time_buf, sizeof(time_buf), "[%02d:%02d:%02d] [%s] ", t->tm_hour, t->tm_min, t->tm_sec, tag ? tag : "INFO");
    } else {
        snprintf(time_buf, sizeof(time_buf), "[00:00:00] [%s] ", tag ? tag : "INFO");
    }

    char msg_buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    // 1. Output to stdout for real-time visibility without blocking stdin
    printf("%s%s\n", time_buf, msg_buf);
    fflush(stdout); 

    // Build the full formatted line
    char full_log_line[2120];
    snprintf(full_log_line, sizeof(full_log_line), "%s%s\n", time_buf, msg_buf);

    // 2. Persist to local it305_runtime.log
    append_to_file("it305_runtime.log", full_log_line);

    // 3. Persist to Results/ directory log files if Results folder is accessible
    const char* res_dir = get_results_dir();
    if (res_dir) {
        // Central unified log in Results/
        char res_unified_log[256];
        snprintf(res_unified_log, sizeof(res_unified_log), "%s/it305_runtime.log", res_dir);
        if (strcmp(res_dir, "Results") != 0) {
            append_to_file(res_unified_log, full_log_line);
        }

        const char* cname = get_current_case_name();

        // Part-level unified log: Results/Part1_runtime.log or Results/Part2_runtime.log
        char part_log[256];
        if (strncmp(cname, "Part1", 5) == 0) {
            snprintf(part_log, sizeof(part_log), "%s/Part1_runtime.log", res_dir);
            append_to_file(part_log, full_log_line);
        } else {
            snprintf(part_log, sizeof(part_log), "%s/Part2_runtime.log", res_dir);
            append_to_file(part_log, full_log_line);
        }

        // Dedicated per-case log
        char case_log[256] = "";
        if (strstr(cname, "Part1_Single")) {
            snprintf(case_log, sizeof(case_log), "%s/Part1_Single_runtime.log", res_dir);
        } else if (strstr(cname, "Part1_Multi") || strncmp(cname, "Part1", 5) == 0) {
            snprintf(case_log, sizeof(case_log), "%s/Part1_Multi_runtime.log", res_dir);
        } else if (strstr(cname, "Part2_Case1")) {
            snprintf(case_log, sizeof(case_log), "%s/Part2_Case1_runtime.log", res_dir);
        } else if (strstr(cname, "Part2_Case2_Enhanced")) {
            snprintf(case_log, sizeof(case_log), "%s/Part2_Case2_Enhanced_runtime.log", res_dir);
        } else if (strstr(cname, "Part2_Case2")) {
            snprintf(case_log, sizeof(case_log), "%s/Part2_Case2_runtime.log", res_dir);
        }

        if (case_log[0] != '\0') {
            append_to_file(case_log, full_log_line);
        }
    }
    
    pthread_mutex_unlock(&log_mutex);
}

void record_transfer_metrics(const char* csv_filepath, const char* case_name, 
                             int num_clients, double pf, 
                             uint64_t useful_bytes, uint64_t total_bytes, uint64_t redundant_bytes, 
                             struct timespec t_start, struct timespec t_end, int retries) {
    if (!case_name) {
        return;
    }

    // Check environment overrides (for automated benchmark runner)
    const char* env_csv = getenv("CSV_FILE");
    if (env_csv && env_csv[0] != '\0') {
        csv_filepath = env_csv;
    }
    const char* env_case = getenv("CASE_NAME");
    if (env_case && env_case[0] != '\0') {
        case_name = env_case;
    }
    const char* env_num = getenv("NUM_CLIENTS");
    if (env_num && env_num[0] != '\0') {
        num_clients = atoi(env_num);
    }
    const char* env_pf = getenv("FAULT_PROB");
    if (env_pf && env_pf[0] != '\0') {
        pf = atof(env_pf);
    }

    pthread_mutex_lock(&log_mutex);

    // Calculate elapsed duration in seconds: T_complete = (t_end - t_start)
    double duration_sec = (double)(t_end.tv_sec - t_start.tv_sec) + 
                          (double)(t_end.tv_nsec - t_start.tv_nsec) * 1e-9;
    if (duration_sec < 0.0) {
        duration_sec = 0.0;
    }
    
    // Compute the three core metrics defined in Section 9.1:
    double goodput_mbps = (duration_sec > 0.0) ? (useful_bytes / (duration_sec * 1000000.0)) : 0.0;
    double throughput_mbps = (duration_sec > 0.0) ? (total_bytes / (duration_sec * 1000000.0)) : 0.0;
    double overhead_pct = (useful_bytes > 0) ? 
        (((double)total_bytes - (double)useful_bytes) / (double)useful_bytes) * 100.0 : 0.0;

    const char* csv_header = "case_name,num_clients,pf,t_start_sec,t_end_sec,duration_sec,useful_bytes,total_bytes,redundant_bytes,goodput_mbps,throughput_mbps,overhead_pct,retries\n";

    char csv_line[256];
    snprintf(csv_line, sizeof(csv_line), "%s,%d,%.2f,%lld,%lld,%.6f,%llu,%llu,%llu,%.6f,%.6f,%.2f,%d\n",
             case_name, num_clients, pf,
             (long long)t_start.tv_sec, (long long)t_end.tv_sec, duration_sec,
             (unsigned long long)useful_bytes, 
             (unsigned long long)total_bytes, 
             (unsigned long long)redundant_bytes,
             goodput_mbps, throughput_mbps, overhead_pct, retries);

    // 1. Append to primary csv_filepath (e.g. Results/Part1_metrics.csv or Results/Part2_metrics.csv)
    if (csv_filepath && csv_filepath[0] != '\0') {
        struct stat st;
        bool write_hdr = (stat(csv_filepath, &st) != 0 || st.st_size == 0);
        FILE* fp = fopen(csv_filepath, "a");
        if (fp) {
            if (write_hdr) {
                fputs(csv_header, fp);
            }
            fputs(csv_line, fp);
            fflush(fp);
            fclose(fp);
        }
    }

    // 2. Also automatically append to dedicated per-case CSV in Results/
    const char* res_dir = get_results_dir();
    if (res_dir) {
        char specific_csv[256] = "";
        if (strstr(case_name, "Part1_Single")) {
            snprintf(specific_csv, sizeof(specific_csv), "%s/Part1_Single_metrics.csv", res_dir);
        } else if (strstr(case_name, "Part1_Multi") || strstr(case_name, "Part1_Reliable") || strncmp(case_name, "Part1", 5) == 0) {
            snprintf(specific_csv, sizeof(specific_csv), "%s/Part1_Multi_metrics.csv", res_dir);
        } else if (strstr(case_name, "Part2_Case1")) {
            snprintf(specific_csv, sizeof(specific_csv), "%s/Part2_Case1_metrics.csv", res_dir);
        } else if (strstr(case_name, "Part2_Case2_Enhanced")) {
            snprintf(specific_csv, sizeof(specific_csv), "%s/Part2_Case2_Enhanced_metrics.csv", res_dir);
        } else if (strstr(case_name, "Part2_Case2")) {
            snprintf(specific_csv, sizeof(specific_csv), "%s/Part2_Case2_metrics.csv", res_dir);
        }

        // Avoid writing duplicate row if specific_csv is already the primary csv_filepath
        if (specific_csv[0] != '\0' && (!csv_filepath || strcmp(csv_filepath, specific_csv) != 0)) {
            struct stat st_spec;
            bool write_spec_hdr = (stat(specific_csv, &st_spec) != 0 || st_spec.st_size == 0);
            FILE* sfp = fopen(specific_csv, "a");
            if (sfp) {
                if (write_spec_hdr) {
                    fputs(csv_header, sfp);
                }
                fputs(csv_line, sfp);
                fflush(sfp);
                fclose(sfp);
            }
        }
    }

    pthread_mutex_unlock(&log_mutex);
}
