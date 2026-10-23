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

    auto write_csv_entry = [](const char* target_path, const char* c_name, int n_clients, double p_fail,
                              struct timespec ts, struct timespec te, double d_sec,
                              uint64_t u_bytes, uint64_t tot_bytes, uint64_t red_bytes,
                              double gp_mbps, double tp_mbps, double ov_pct, int r_tries) {
        if (!target_path || target_path[0] == '\0') return;

        bool is_khush_p1 = false;
        bool is_khush_p2 = false;
        bool need_header = true;

        FILE* rfp = fopen(target_path, "r");
        if (rfp) {
            char first_line[512] = "";
            if (fgets(first_line, sizeof(first_line), rfp)) {
                if (strlen(first_line) > 5) {
                    need_header = false;
                    if (strstr(first_line, "Server_Type") || strstr(first_line, "Concurrent_Clients")) {
                        is_khush_p1 = true;
                    } else if (strstr(first_line, "Failure_Probability") || strstr(first_line, "Total_Connection_Attempts") || strstr(first_line, "Wasted_Bytes")) {
                        is_khush_p2 = true;
                    }
                }
            }
            fclose(rfp);
        }

        if (need_header) {
            if (strstr(target_path, "Part1") || strstr(c_name, "Part1")) {
                is_khush_p1 = true;
            } else if (strstr(target_path, "Part2") || strstr(c_name, "Part2")) {
                is_khush_p2 = true;
            }
        }

        FILE* fp = fopen(target_path, "a");
        if (!fp) return;

        static unsigned int run_seq = 1;

        if (is_khush_p1) {
            if (need_header) {
                fputs("Test_Run_ID,Server_Type,Concurrent_Clients,Payload_Bytes,Total_Time_Seconds,Throughput_MBps,Goodput_MBps,Total_Bytes_Sent,Retries\n", fp);
            }
            const char* server_type = (strstr(c_name, "Single") != nullptr) ? "Single" : "Multi";
            fprintf(fp, "P1_%s_N%d_C%u,%s,%d,%llu,%.6f,%.6f,%.6f,%llu,%d\n",
                    server_type, n_clients, run_seq++,
                    server_type, n_clients,
                    (unsigned long long)u_bytes,
                    d_sec, tp_mbps, gp_mbps,
                    (unsigned long long)tot_bytes, r_tries);
        } else if (is_khush_p2) {
            if (need_header) {
                fputs("Test_Run_ID,Case_Name,Failure_Probability,Total_Connection_Attempts,Wasted_Bytes,Total_Time_Seconds,Payload_Bytes,Throughput_MBps,Goodput_MBps,Session_Resumes_Count,Resumed_Offset_Bytes,Total_Bytes_Sent,Active_Streams,State_Lookup_Latency_ms,Metadata_Overhead_Bytes,Checksum_Validation_Time_ms\n", fp);
            }
            const char* cn = "Part2_Case1";
            if (strstr(c_name, "Enhanced") || strstr(c_name, "Case2_Enhanced")) {
                cn = "Part2_Case2_Enhanced";
            } else if (strstr(c_name, "Case2")) {
                cn = "Part2_Case2";
            }
            int streams = (strstr(cn, "Enhanced") != nullptr) ? 4 : 1;
            int resumes = (strstr(cn, "Case1") != nullptr) ? 0 : r_tries;
            uint64_t resumed_offset = (resumes > 0) ? (u_bytes / (resumes + 1)) : 0;
            int attempts = r_tries + 1;

            fprintf(fp, "P2_%s_Pf%.2f_C%u,%s,%.2f,%d,%llu,%.6f,%llu,%.6f,%.6f,%d,%llu,%llu,%d,0.0,%llu,0.0\n",
                    cn, p_fail, run_seq++,
                    cn, p_fail, attempts,
                    (unsigned long long)red_bytes,
                    d_sec,
                    (unsigned long long)u_bytes,
                    tp_mbps, gp_mbps,
                    resumes, (unsigned long long)resumed_offset,
                    (unsigned long long)tot_bytes,
                    streams, (unsigned long long)(tot_bytes > u_bytes ? tot_bytes - u_bytes : 0));
        } else {
            if (need_header) {
                fputs("case_name,num_clients,pf,t_start_sec,t_end_sec,duration_sec,useful_bytes,total_bytes,redundant_bytes,goodput_mbps,throughput_mbps,overhead_pct,retries\n", fp);
            }
            fprintf(fp, "%s,%d,%.2f,%lld,%lld,%.6f,%llu,%llu,%llu,%.6f,%.6f,%.2f,%d\n",
                    c_name, n_clients, p_fail,
                    (long long)ts.tv_sec, (long long)te.tv_sec, d_sec,
                    (unsigned long long)u_bytes,
                    (unsigned long long)tot_bytes,
                    (unsigned long long)red_bytes,
                    gp_mbps, tp_mbps, ov_pct, r_tries);
        }

        fflush(fp);
        fclose(fp);
    };

    // 1. Append to primary csv_filepath
    if (csv_filepath && csv_filepath[0] != '\0') {
        write_csv_entry(csv_filepath, case_name, num_clients, pf,
                        t_start, t_end, duration_sec,
                        useful_bytes, total_bytes, redundant_bytes,
                        goodput_mbps, throughput_mbps, overhead_pct, retries);
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

        if (specific_csv[0] != '\0' && (!csv_filepath || strcmp(csv_filepath, specific_csv) != 0)) {
            write_csv_entry(specific_csv, case_name, num_clients, pf,
                            t_start, t_end, duration_sec,
                            useful_bytes, total_bytes, redundant_bytes,
                            goodput_mbps, throughput_mbps, overhead_pct, retries);
        }
    }

    pthread_mutex_unlock(&log_mutex);
}
