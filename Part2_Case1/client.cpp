#include "fault.h"
#include "protocol.h"
#include "metrics_logger.h"
#include "common_types.h"

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

/*
 * client.cpp - Member 3 (PR #4, Wave 2)
 *
 * Implements:
 *   1. parse_client_cli positional parser and directory preparer
 *   2. run_client_part1_and_case1_loop stateless retry engine with truncated exponential backoff
 *   3. Case mode dispatcher with weak hooks for Wave 3 modules
 */

// Forward declarations with weak linkage for Wave 3 modules
extern int run_client_case2_loop(const char* server_ip, uint16_t port,
                                const char* topic, const char* output_dir,
                                int max_retries, uint32_t ckpt_window) __attribute__((weak));

extern int run_enhanced_client(const char* server_ip, uint16_t port,
                              const char* topic, const char* output_dir,
                              int num_streams, int max_retries) __attribute__((weak));

static int make_dir_recursive(const std::string& path) {
    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
        char c = path[i];
        if (c == '/' || c == '\\') {
            if (!current.empty() && current != "." && current != "..") {
                mkdir(current.c_str(), 0755);
            }
        }
        current += c;
    }
    if (!current.empty()) {
        if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
            return -1;
        }
    }
    return 0;
}

int parse_client_cli(int argc, char* argv[], std::string* out_ip, uint16_t* out_port,
                     std::string* out_topic, std::string* out_dir, int* out_max_retries,
                     uint32_t* out_ckpt_win, int* out_streams) {
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <server_ip> <server_port> <topic> <output_dir> "
                  << "[--max-retries N] [--ckpt-window W] [--streams N]\n";
        return -1;
    }

    if (!out_ip || !out_port || !out_topic || !out_dir) {
        return -1;
    }

    *out_ip = argv[1];
    struct sockaddr_in sa;
    if (inet_pton(AF_INET, out_ip->c_str(), &(sa.sin_addr)) <= 0) {
        log_event("ERROR", "Invalid IPv4 address format: '%s'", out_ip->c_str());
        return -1;
    }

    int port_val = std::atoi(argv[2]);
    if (port_val <= 0 || port_val > 65535) {
        log_event("ERROR", "Invalid server port: '%s' (must be 1-65535)", argv[2]);
        return -1;
    }
    *out_port = static_cast<uint16_t>(port_val);

    *out_topic = argv[3];
    if (out_topic->empty()) {
        log_event("ERROR", "Topic argument cannot be empty");
        return -1;
    }

    *out_dir = argv[4];
    if (out_dir->empty()) {
        log_event("ERROR", "Output directory argument cannot be empty");
        return -1;
    }

    // Default configuration values
    if (out_max_retries) *out_max_retries = DEFAULT_MAX_RETRIES;
    if (out_ckpt_win) *out_ckpt_win = CHUNK_SIZE;
    if (out_streams) *out_streams = DEFAULT_PARALLEL_CONN;

    // Parse optional flags
    for (int i = 5; i < argc; ++i) {
        if (std::strcmp(argv[i], "--max-retries") == 0 && i + 1 < argc) {
            if (out_max_retries) *out_max_retries = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--ckpt-window") == 0 && i + 1 < argc) {
            if (out_ckpt_win) *out_ckpt_win = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--streams") == 0 && i + 1 < argc) {
            if (out_streams) *out_streams = std::atoi(argv[++i]);
        } else {
            log_event("ERROR", "Unrecognized or incomplete CLI option: '%s'", argv[i]);
            return -1;
        }
    }

    // Ensure output topic destination directory exists (<output_dir>/<topic>/)
    std::string topic_dest_dir = *out_dir + "/" + *out_topic;
    if (make_dir_recursive(topic_dest_dir) != 0 && errno != EEXIST) {
        log_event("ERROR", "Failed to create destination topic directory '%s': %s",
                  topic_dest_dir.c_str(), strerror(errno));
        return -1;
    }

    return 0;
}

static void render_case1_progress_bar(int trial_num, int max_trials,
                                     uint32_t current_files, uint32_t total_files,
                                     uint64_t current_bytes, uint64_t total_bytes,
                                     uint64_t redundant_bytes,
                                     const struct timespec& start_time,
                                     bool force_complete = false) {
    static const char spinner_chars[] = "|/-\\";
    static int spinner_idx = 0;
    static struct timespec last_update{0, 0};

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // Throttle updates to ~25 FPS (40ms) unless force_complete
    if (!force_complete && last_update.tv_sec != 0) {
        long elapsed_ms = (now.tv_sec - last_update.tv_sec) * 1000 +
                          (now.tv_nsec - last_update.tv_nsec) / 1000000;
        if (elapsed_ms < 40) {
            return;
        }
    }
    last_update = now;

    bool is_terminal = (isatty(STDOUT_FILENO) != 0);
    if (!is_terminal) {
        if (force_complete || (total_files > 0 && current_files > 0 && (current_files % 100 == 0 || current_files == total_files))) {
            double pct = total_bytes > 0 ? (static_cast<double>(current_bytes) * 100.0) / static_cast<double>(total_bytes)
                                         : (total_files > 0 ? (static_cast<double>(current_files) * 100.0) / static_cast<double>(total_files) : 0.0);
            double mb = static_cast<double>(current_bytes) / (1024.0 * 1024.0);
            double waste_mb = static_cast<double>(redundant_bytes) / (1024.0 * 1024.0);
            log_event("INFO", "[Trial %d/%d] Progress: %u/%u files (%.1f%%) - %.2f MB (Wasted: %.2f MB)",
                      trial_num, max_trials, current_files, total_files, pct, mb, waste_mb);
        }
        return;
    }

    int cols = 80;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20) {
        cols = ws.ws_col;
    }

    double pct = 0.0;
    if (total_bytes > 0) {
        pct = (static_cast<double>(current_bytes) / static_cast<double>(total_bytes)) * 100.0;
    } else if (total_files > 0) {
        pct = (static_cast<double>(current_files) / static_cast<double>(total_files)) * 100.0;
    }
    if (pct > 100.0) pct = 100.0;
    if (force_complete) pct = 100.0;

    double elapsed_sec = (double)(now.tv_sec - start_time.tv_sec) +
                         (double)(now.tv_nsec - start_time.tv_nsec) * 1e-9;
    double speed_mbps = (elapsed_sec > 0.05) ? (static_cast<double>(current_bytes) / (elapsed_sec * 1024.0 * 1024.0)) : 0.0;

    char spinner = spinner_chars[spinner_idx % 4];
    spinner_idx++;

    double cur_mb = static_cast<double>(current_bytes) / (1024.0 * 1024.0);
    double tot_mb = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
    double waste_mb = static_cast<double>(redundant_bytes) / (1024.0 * 1024.0);

    char trial_buf[32];
    snprintf(trial_buf, sizeof(trial_buf), "[Trial %d/%d]", trial_num, max_trials);
    int trial_len = static_cast<int>(strlen(trial_buf));

    char files_buf[32];
    snprintf(files_buf, sizeof(files_buf), "%u/%u", current_files, total_files);
    int files_len = static_cast<int>(strlen(files_buf));

    char mb_buf[40];
    if (cols >= 100) {
        snprintf(mb_buf, sizeof(mb_buf), "%.1f/%.1f MB", cur_mb, tot_mb);
    } else {
        snprintf(mb_buf, sizeof(mb_buf), "%.1f/%.1fM", cur_mb, tot_mb);
    }
    int mb_len = static_cast<int>(strlen(mb_buf));

    char speed_buf[32];
    if (cols >= 90) {
        snprintf(speed_buf, sizeof(speed_buf), "%.2f MB/s", speed_mbps);
    } else {
        snprintf(speed_buf, sizeof(speed_buf), "%.1fMB/s", speed_mbps);
    }
    int speed_len = static_cast<int>(strlen(speed_buf));

    char waste_display[48] = "";
    int waste_vis_len = 0;
    if (redundant_bytes > 0) {
        if (cols >= 100) {
            snprintf(waste_display, sizeof(waste_display), " | \033[1;31mWasted: %.1fMB\033[0m", waste_mb);
            char tmp[32];
            snprintf(tmp, sizeof(tmp), " | Wasted: %.1fMB", waste_mb);
            waste_vis_len = static_cast<int>(strlen(tmp));
        } else {
            snprintf(waste_display, sizeof(waste_display), " | \033[1;31mW:%.1fM\033[0m", waste_mb);
            char tmp[32];
            snprintf(tmp, sizeof(tmp), " | W:%.1fM", waste_mb);
            waste_vis_len = static_cast<int>(strlen(tmp));
        }
    }

    // Visible fixed characters excluding bar:
    // trial_len + " [" + bar_width + "] " + " 100.0%" (7) + " | " (3) + files_len + " | " (3) + mb_len + waste_vis_len + " | " (3) + speed_len + " [" + spinner (1) + "]" (2)
    int non_bar_len = trial_len + 1 + 2 + 1 + 7 + 3 + files_len + 3 + mb_len + waste_vis_len + 3 + speed_len + 1 + 3;

    int max_vis = cols - 2;
    int bar_width = max_vis - non_bar_len;
    if (bar_width < 5) bar_width = 5;
    if (bar_width > 22) bar_width = 22;

    int filled = static_cast<int>((pct / 100.0) * bar_width);
    if (filled > bar_width) filled = bar_width;

    char bar_str[bar_width + 1];
    for (int i = 0; i < bar_width; ++i) {
        if (i < filled) {
            bar_str[i] = '=';
        } else if (i == filled && filled < bar_width) {
            bar_str[i] = '>';
        } else {
            bar_str[i] = ' ';
        }
    }
    bar_str[bar_width] = '\0';

    if (force_complete) {
        printf("\r\033[2K\033[1;35m%s\033[0m \033[1;32m[%s]\033[0m \033[1;32m100.0%%\033[0m | \033[1;36m%s\033[0m | \033[1;36m%s\033[0m%s | \033[1;33m%s\033[0m | \033[1;32mDone! [OK]\033[0m\n",
               trial_buf, bar_str, files_buf, mb_buf, waste_display, speed_buf);
    } else {
        printf("\r\033[2K\033[1;35m%s\033[0m \033[1;32m[%s]\033[0m \033[1;32m%5.1f%%\033[0m | \033[1;36m%s\033[0m | \033[1;36m%s\033[0m%s | \033[1;33m%s\033[0m [%c]",
               trial_buf, bar_str, pct, files_buf, mb_buf, waste_display, speed_buf, spinner);
    }
    fflush(stdout);
}

int run_client_part1_and_case1_loop(const char* server_ip, uint16_t port,
                                    const char* topic, const char* output_dir,
                                    int max_retries, bool is_part1) {
    if (!server_ip || !topic || !output_dir) {
        return -1;
    }

    std::string topic_dest_dir = std::string(output_dir) + "/" + topic;
    if (make_dir_recursive(topic_dest_dir) != 0 && errno != EEXIST) {
        log_event("ERROR", "Failed to create destination topic directory '%s': %s",
                  topic_dest_dir.c_str(), strerror(errno));
        return -1;
    }

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    uint64_t total_wire_bytes = 0;
    uint64_t redundant_bytes = 0;
    uint64_t useful_bytes = 0;
    int retries_count = 0;
    bool transfer_completed = false;
    uint32_t files_received_count = 0;
    uint32_t expected_file_count = 0;
    uint64_t expected_total_bytes = 0;

    log_event("INFO", "Connecting to %s:%u...", server_ip, port);

    for (int attempt = 0; attempt < max_retries; ++attempt) {
        if (attempt > 0) {
            retries_count++;
            int shift = (attempt < 20) ? attempt : 20;
            int delay_ms = std::min(1000, 50 * (1 << shift));
            log_event("INFO", "Retry attempt %d/%d after backoff %d ms...", attempt, max_retries, delay_ms);
            struct timespec sleep_ts;
            sleep_ts.tv_sec = delay_ms / 1000;
            sleep_ts.tv_nsec = (delay_ms % 1000) * 1000000L;
            nanosleep(&sleep_ts, nullptr);
        }

        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            log_event("ERROR", "socket() failed: %s", strerror(errno));
            continue;
        }

        struct sockaddr_in saddr;
        std::memset(&saddr, 0, sizeof(saddr));
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons(port);
        inet_pton(AF_INET, server_ip, &saddr.sin_addr);

        if (connect(sockfd, reinterpret_cast<struct sockaddr*>(&saddr), sizeof(saddr)) < 0) {
            log_event("ERROR", "connect() to %s:%u failed: %s", server_ip, port, strerror(errno));
            close(sockfd);
            continue;
        }

        struct timespec t_trial_start;
        clock_gettime(CLOCK_MONOTONIC, &t_trial_start);

        log_event("INFO", "Connected to server. Requesting topic '%s' (Trial %d/%d)...",
                  topic, attempt + 1, max_retries);

        if (send_get(sockfd, topic, &total_wire_bytes) < 0) {
            log_event("ERROR", "Failed to send OP_GET request for topic '%s'", topic);
            close(sockfd);
            continue;
        }

        uint64_t attempt_file_bytes = 0;
        files_received_count = 0;
        int current_file_fd = -1;
        std::string current_filename = "";
        uint64_t current_file_bytes_written = 0;
        bool connection_failed = false;

        while (true) {
            FrameHeader hdr;
            std::vector<uint8_t> payload;
            int rc = recv_frame(sockfd, &hdr, payload, &total_wire_bytes);

            if (rc <= 0) {
                const char* fault_desc = (rc == FRAME_PROTO_ERR) ? "Corrupted frame header / protocol error" :
                                         (rc == 0) ? "Connection closed prematurely by server" :
                                                     "Connection dropped / fault injected";
                if (isatty(STDOUT_FILENO)) {
                    double wasted_attempt_mb = static_cast<double>(attempt_file_bytes) / (1024.0 * 1024.0);
                    printf("\n\033[1;31m[FAULT / CORRUPTION DETECTED]\033[0m %s at %u/%u files (%.2f MB). Stateless reset: restarting from File 0 for Trial %d/%d...\n",
                           fault_desc, files_received_count, expected_file_count, wasted_attempt_mb, attempt + 2, max_retries);
                    fflush(stdout);
                }
                log_event("INFO", "Server dropped connection (%s, rc=%d) mid-transfer at attempt %d", fault_desc, rc, attempt);
                if (current_file_fd >= 0) {
                    close(current_file_fd);
                    current_file_fd = -1;
                }
                redundant_bytes += attempt_file_bytes;
                close(sockfd);
                sockfd = -1;
                connection_failed = true;
                break;
            }

            if (hdr.type == OP_GET_OK) {
                MsgGetOk get_ok;
                if (parse_get_ok(payload.data(), static_cast<uint32_t>(payload.size()), &get_ok)) {
                    expected_file_count = get_ok.file_count;
                    expected_total_bytes = get_ok.total_bytes;
                    double total_mb = static_cast<double>(get_ok.total_bytes) / (1024.0 * 1024.0);
                    log_event("INFO", "Received OP_GET_OK: %u files, %llu total bytes (%.2f MB)",
                              get_ok.file_count, static_cast<unsigned long long>(get_ok.total_bytes), total_mb);
                }
            } else if (hdr.type == OP_ERROR) {
                MsgStatus st;
                if (parse_status_msg(payload.data(), static_cast<uint32_t>(payload.size()), &st)) {
                    log_event("ERROR", "Server returned OP_ERROR code %u: %s", st.code, st.reason.c_str());
                    if (current_file_fd >= 0) {
                        close(current_file_fd);
                        current_file_fd = -1;
                    }
                    redundant_bytes += attempt_file_bytes;
                    close(sockfd);
                    if (st.code == ERR_TOPIC_NOT_FOUND || st.code == ERR_BAD_REQUEST) {
                        return -1;
                    }
                    if (isatty(STDOUT_FILENO)) {
                        double wasted_attempt_mb = static_cast<double>(attempt_file_bytes) / (1024.0 * 1024.0);
                        printf("\n\033[1;31m[SERVER ERROR DETECTED]\033[0m Error code %u (%s) at %u/%u files (%.2f MB). Stateless reset: restarting from File 0 for Trial %d/%d...\n",
                               st.code, st.reason.c_str(), files_received_count, expected_file_count, wasted_attempt_mb, attempt + 2, max_retries);
                        fflush(stdout);
                    }
                    connection_failed = true;
                    break;
                }
            } else if (hdr.type == OP_FILE_BEGIN) {
                MsgFileBegin fb;
                if (!parse_file_begin(payload.data(), static_cast<uint32_t>(payload.size()), &fb)) {
                    if (isatty(STDOUT_FILENO)) {
                        double wasted_attempt_mb = static_cast<double>(attempt_file_bytes) / (1024.0 * 1024.0);
                        printf("\n\033[1;31m[CORRUPTED DATA DETECTED]\033[0m Malformed FILE_BEGIN payload at %u/%u files (%.2f MB). Stateless reset: restarting from File 0 for Trial %d/%d...\n",
                               files_received_count, expected_file_count, wasted_attempt_mb, attempt + 2, max_retries);
                        fflush(stdout);
                    }
                    log_event("ERROR", "Corrupted FILE_BEGIN payload received at attempt %d", attempt);
                    if (current_file_fd >= 0) {
                        close(current_file_fd);
                        current_file_fd = -1;
                    }
                    redundant_bytes += attempt_file_bytes;
                    close(sockfd);
                    sockfd = -1;
                    connection_failed = true;
                    break;
                }
                if (current_file_fd >= 0) {
                    close(current_file_fd);
                    current_file_fd = -1;
                }
                current_filename = fb.filename;
                current_file_bytes_written = 0;
                if (fb.file_index == 0) {
                    log_event("INFO", "Receiving file [1/%u]: '%s' (%llu bytes)",
                              expected_file_count, fb.filename.c_str(),
                              static_cast<unsigned long long>(fb.bytes_following));
                }
                std::string dest_path = std::string(output_dir) + "/" + topic + "/" + fb.filename;
                // In Part 1 & Case 1: Open with O_WRONLY | O_CREAT | O_TRUNC (overwriting from Byte 0)
                current_file_fd = open(dest_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (current_file_fd < 0) {
                    log_event("ERROR", "Failed to open destination file '%s': %s", dest_path.c_str(), strerror(errno));
                    close(sockfd);
                    return -1;
                }
            } else if (hdr.type == OP_FILE_DATA) {
                MsgFileData fd_msg;
                if (!parse_file_data(payload.data(), static_cast<uint32_t>(payload.size()), &fd_msg)) {
                    if (isatty(STDOUT_FILENO)) {
                        double wasted_attempt_mb = static_cast<double>(attempt_file_bytes) / (1024.0 * 1024.0);
                        printf("\n\033[1;31m[CORRUPTED DATA DETECTED]\033[0m Malformed FILE_DATA payload at %u/%u files (%.2f MB). Stateless reset: restarting from File 0 for Trial %d/%d...\n",
                               files_received_count, expected_file_count, wasted_attempt_mb, attempt + 2, max_retries);
                        fflush(stdout);
                    }
                    log_event("ERROR", "Corrupted FILE_DATA payload received mid-transfer at attempt %d", attempt);
                    if (current_file_fd >= 0) {
                        close(current_file_fd);
                        current_file_fd = -1;
                    }
                    redundant_bytes += attempt_file_bytes;
                    close(sockfd);
                    sockfd = -1;
                    connection_failed = true;
                    break;
                }
                if (current_file_fd >= 0) {
                    ssize_t written = 0;
                    size_t to_write = fd_msg.data.size();
                    while (written < static_cast<ssize_t>(to_write)) {
                        ssize_t w = write(current_file_fd, fd_msg.data.data() + written, to_write - written);
                        if (w <= 0) {
                            log_event("ERROR", "Write failed to '%s': %s", current_filename.c_str(), strerror(errno));
                            break;
                        }
                        written += w;
                    }
                    current_file_bytes_written += written;
                    attempt_file_bytes += written;
                    render_case1_progress_bar(attempt + 1, max_retries,
                                             files_received_count, expected_file_count,
                                             attempt_file_bytes, expected_total_bytes,
                                             redundant_bytes, t_trial_start);
                }
            } else if (hdr.type == OP_FILE_END) {
                MsgFileEnd fe;
                if (!parse_file_end(payload.data(), static_cast<uint32_t>(payload.size()), &fe)) {
                    if (isatty(STDOUT_FILENO)) {
                        double wasted_attempt_mb = static_cast<double>(attempt_file_bytes) / (1024.0 * 1024.0);
                        printf("\n\033[1;31m[CORRUPTED DATA DETECTED]\033[0m Malformed FILE_END payload at %u/%u files (%.2f MB). Stateless reset: restarting from File 0 for Trial %d/%d...\n",
                               files_received_count, expected_file_count, wasted_attempt_mb, attempt + 2, max_retries);
                        fflush(stdout);
                    }
                    log_event("ERROR", "Corrupted FILE_END payload received at attempt %d", attempt);
                    if (current_file_fd >= 0) {
                        close(current_file_fd);
                        current_file_fd = -1;
                    }
                    redundant_bytes += attempt_file_bytes;
                    close(sockfd);
                    sockfd = -1;
                    connection_failed = true;
                    break;
                }
                if (current_file_fd >= 0) {
                    close(current_file_fd);
                    current_file_fd = -1;
                }
                files_received_count++;
                render_case1_progress_bar(attempt + 1, max_retries,
                                         files_received_count, expected_file_count,
                                         attempt_file_bytes, expected_total_bytes,
                                         redundant_bytes, t_trial_start);
            } else if (hdr.type == OP_TRANSFER_END) {
                MsgTransferEnd te;
                if (parse_transfer_end(payload.data(), static_cast<uint32_t>(payload.size()), &te)) {
                    if (current_file_fd >= 0) {
                        close(current_file_fd);
                        current_file_fd = -1;
                    }
                    close(sockfd);
                    sockfd = -1;

                    // Strictly verify completion invariants
                    if (te.files_sent != files_received_count) {
                        log_event("ERROR", "File count mismatch: server sent %u, client received %u",
                                  te.files_sent, files_received_count);
                        return -1;
                    }
                    if (te.useful_bytes != attempt_file_bytes) {
                        log_event("ERROR", "Payload bytes mismatch: server=%llu, client wrote=%llu",
                                  static_cast<unsigned long long>(te.useful_bytes),
                                  static_cast<unsigned long long>(attempt_file_bytes));
                        return -1;
                    }

                    useful_bytes = attempt_file_bytes;
                    transfer_completed = true;
                    render_case1_progress_bar(attempt + 1, max_retries,
                                             files_received_count, expected_file_count,
                                             attempt_file_bytes, expected_total_bytes,
                                             redundant_bytes, t_trial_start, true);
                    log_event("INFO", "Received OP_TRANSFER_END from server");
                    break;
                }
            }
        }

        if (transfer_completed) {
            break;
        }

        if (!connection_failed && sockfd >= 0) {
            close(sockfd);
        }
    }

    if (!transfer_completed) {
        log_event("ERROR", "Transfer failed after %d retries", max_retries);
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);

    std::string csv_path = is_part1 ? "Results/Part1_metrics.csv" : "Results/Part2_metrics.csv";
    if (access("Results", F_OK) != 0 && access("../Results", F_OK) == 0) {
        csv_path = is_part1 ? "../Results/Part1_metrics.csv" : "../Results/Part2_metrics.csv";
    }

    const char* case_name = is_part1 ? "Part1_Reliable" : "Part2_Case1_Stateless";
    record_transfer_metrics(csv_path.c_str(), case_name, 1, 0.0,
                            useful_bytes, total_wire_bytes, redundant_bytes,
                            t_start, t_end, retries_count);

    double duration_sec = (double)(t_end.tv_sec - t_start.tv_sec) + 
                          (double)(t_end.tv_nsec - t_start.tv_nsec) * 1e-9;
    if (duration_sec < 0.0) duration_sec = 0.0;
    double goodput_mbps = (duration_sec > 0.0) ? (useful_bytes / (duration_sec * 1000000.0)) : 0.0;
    double throughput_mbps = (duration_sec > 0.0) ? (total_wire_bytes / (duration_sec * 1000000.0)) : 0.0;
    double overhead_pct = (useful_bytes > 0) ? 
        (((double)total_wire_bytes - (double)useful_bytes) / (double)useful_bytes) * 100.0 : 0.0;

    log_event("INFO", "Transfer SUCCESS: %u files (%llu bytes) written to '%s/%s' in %d retries",
              files_received_count, static_cast<unsigned long long>(useful_bytes),
              output_dir, topic, retries_count);
    log_event("INFO", "Performance: Duration = %.2fs | Goodput = %.2f MB/s | Wire Throughput = %.2f MB/s | Overhead = %.2f%%",
              duration_sec, goodput_mbps, throughput_mbps, overhead_pct);
    return 0;
}

#ifndef UNIT_TEST
int main(int argc, char* argv[]) {
    std::string server_ip;
    uint16_t port = 0;
    std::string topic;
    std::string output_dir;
    int max_retries = DEFAULT_MAX_RETRIES;
    uint32_t ckpt_window = CHUNK_SIZE;
    int streams = DEFAULT_PARALLEL_CONN;

    if (parse_client_cli(argc, argv, &server_ip, &port, &topic, &output_dir,
                         &max_retries, &ckpt_window, &streams) != 0) {
        return 1;
    }

#if CASE_MODE == 0
    return run_client_part1_and_case1_loop(server_ip.c_str(), port, topic.c_str(), output_dir.c_str(), max_retries, true);
#elif CASE_MODE == 1
    return run_client_part1_and_case1_loop(server_ip.c_str(), port, topic.c_str(), output_dir.c_str(), max_retries, false);
#elif CASE_MODE == 2
    if (run_client_case2_loop) {
        return run_client_case2_loop(server_ip.c_str(), port, topic.c_str(), output_dir.c_str(), max_retries, ckpt_window);
    } else {
        log_event("INFO", "Case 2 stateful recovery loop will be activated in Wave 3 (Member 4). Falling back to stateless loop.");
        return run_client_part1_and_case1_loop(server_ip.c_str(), port, topic.c_str(), output_dir.c_str(), max_retries, false);
    }
#elif CASE_MODE == 3
    if (run_enhanced_client) {
        return run_enhanced_client(server_ip.c_str(), port, topic.c_str(), output_dir.c_str(), streams, max_retries);
    } else {
        log_event("INFO", "Enhanced parallel client will be activated in Wave 3 (Member 5). Falling back to stateless loop.");
        return run_client_part1_and_case1_loop(server_ip.c_str(), port, topic.c_str(), output_dir.c_str(), max_retries, false);
    }
#else
    return run_client_part1_and_case1_loop(server_ip.c_str(), port, topic.c_str(), output_dir.c_str(), max_retries, false);
#endif
}
#endif

