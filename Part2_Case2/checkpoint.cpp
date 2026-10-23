#include "checkpoint.h"
#include "protocol.h"
#include "metrics_logger.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <algorithm>
#include <ctime>
#include <sys/ioctl.h>

/*
 * checkpoint.cpp - Member 4 (PR #5, Wave 3)
 *
 * Implements:
 *   1. Strict 3-step durability checkpoint save (fsync payload -> fsync tmp -> atomic rename -> optional ACK)
 *   2. Checkpoint load and parser from disk ASCII record
 *   3. Checkpoint cleanup on transfer completion or restart
 *   4. Safe file opening with O_WRONLY | O_CREAT (never O_TRUNC or O_APPEND) and seek
 *   5. Case 2 stateful recovery client loop (run_client_case2_loop)
 */

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

int save_checkpoint(const char* output_dir, const CheckpointRecord* ckpt,
                    int active_file_fd, int sockfd) {
    if (!output_dir || !ckpt) {
        return -1;
    }

    // Step (a): Call fsync on active target file descriptor so payload up to byte_offset is durable
    if (active_file_fd >= 0) {
        if (fsync(active_file_fd) < 0 && errno != EBADF && errno != EINVAL) {
            log_event("ERROR", "fsync failed on active file fd=%d: %s", active_file_fd, strerror(errno));
            return -1;
        }
    }

    // Step (b): Write single-line ASCII record to .it305_<topic>.ckpt.tmp, fsync, and atomic rename
    std::string tmp_path = std::string(output_dir) + "/.it305_" + ckpt->topic + ".ckpt.tmp";
    std::string final_path = std::string(output_dir) + "/.it305_" + ckpt->topic + ".ckpt";

    int tmp_fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (tmp_fd < 0) {
        log_event("ERROR", "Failed to open tmp ckpt file '%s': %s", tmp_path.c_str(), strerror(errno));
        return -1;
    }

    char hex32[33];
    session_id_to_hex(ckpt->session_id, hex32);

    const char* fn = (ckpt->file_name[0] != '\0') ? ckpt->file_name : "EOF";

    char line_buf[1024];
    int len = snprintf(line_buf, sizeof(line_buf), "%s %s %u %llu %u %s\n",
                       hex32, ckpt->topic, ckpt->file_index,
                       static_cast<unsigned long long>(ckpt->byte_offset),
                       ckpt->files_done, fn);

    if (len <= 0 || len >= static_cast<int>(sizeof(line_buf))) {
        close(tmp_fd);
        return -1;
    }

    ssize_t w = write(tmp_fd, line_buf, len);
    if (w != len) {
        log_event("ERROR", "Short write to '%s': %s", tmp_path.c_str(), strerror(errno));
        close(tmp_fd);
        unlink(tmp_path.c_str());
        return -1;
    }

    if (fsync(tmp_fd) < 0) {
        log_event("ERROR", "fsync failed on '%s': %s", tmp_path.c_str(), strerror(errno));
        close(tmp_fd);
        unlink(tmp_path.c_str());
        return -1;
    }
    close(tmp_fd);

    if (rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        log_event("ERROR", "rename '%s' to '%s' failed: %s",
                  tmp_path.c_str(), final_path.c_str(), strerror(errno));
        unlink(tmp_path.c_str());
        return -1;
    }

    // Step (c): Optionally send non-blocking CHECKPOINT_ACK (0x08)
    if (sockfd >= 0) {
        send_ckpt_ack(sockfd, ckpt->session_id, ckpt->file_index, ckpt->byte_offset, nullptr);
    }

    return 0;
}

bool load_checkpoint(const char* output_dir, const char* topic, CheckpointRecord* out_ckpt) {
    if (!output_dir || !topic || !out_ckpt) {
        return false;
    }

    std::string final_path = std::string(output_dir) + "/.it305_" + topic + ".ckpt";
    FILE* fp = fopen(final_path.c_str(), "r");
    if (!fp) {
        return false;
    }

    char line[1024];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return false;
    }
    fclose(fp);

    char hex32[128];
    char top[128];
    unsigned int f_idx = 0;
    unsigned long long b_off = 0;
    unsigned int f_done = 0;
    char fname[512];

    int matched = sscanf(line, "%127s %127s %u %llu %u %511s",
                         hex32, top, &f_idx, &b_off, &f_done, fname);
    if (matched != 6) {
        return false;
    }

    if (!hex_to_session_id(hex32, out_ckpt->session_id)) {
        return false;
    }

    std::strncpy(out_ckpt->session_id_hex, hex32, 33);
    out_ckpt->session_id_hex[32] = '\0';

    std::strncpy(out_ckpt->topic, top, sizeof(out_ckpt->topic) - 1);
    out_ckpt->topic[sizeof(out_ckpt->topic) - 1] = '\0';

    out_ckpt->file_index = f_idx;
    out_ckpt->byte_offset = b_off;
    out_ckpt->files_done = f_done;

    std::strncpy(out_ckpt->file_name, fname, sizeof(out_ckpt->file_name) - 1);
    out_ckpt->file_name[sizeof(out_ckpt->file_name) - 1] = '\0';

    return true;
}

void remove_checkpoint(const char* output_dir, const char* topic) {
    if (!output_dir || !topic) {
        return;
    }
    std::string final_path = std::string(output_dir) + "/.it305_" + topic + ".ckpt";
    std::string tmp_path = std::string(output_dir) + "/.it305_" + topic + ".ckpt.tmp";
    unlink(final_path.c_str());
    unlink(tmp_path.c_str());
}

int open_and_seek_resume_file(const char* filepath, uint64_t accepted_offset) {
    if (!filepath) {
        return -1;
    }

    // Must NEVER use O_TRUNC or O_APPEND
    int fd = open(filepath, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        log_event("ERROR", "open_and_seek_resume_file: open failed on '%s': %s", filepath, strerror(errno));
        return -1;
    }

    if (accepted_offset > 0) {
        if (lseek(fd, static_cast<off_t>(accepted_offset), SEEK_SET) == (off_t)-1) {
            log_event("ERROR", "open_and_seek_resume_file: lseek to %llu failed on '%s': %s",
                      static_cast<unsigned long long>(accepted_offset), filepath, strerror(errno));
            close(fd);
            return -1;
        }
    }

    return fd;
}

static void render_case2_progress_bar(int trial_num, int max_trials, bool has_ckpt,
                                     uint32_t current_files, uint32_t total_files,
                                     uint64_t current_bytes, uint64_t total_bytes,
                                     uint64_t redundant_bytes,
                                     const struct timespec& trial_start_time,
                                     uint64_t trial_wire_bytes,
                                     bool force_complete = false) {
    static const char spinner_chars[] = "|/-\\";
    static int spinner_idx = 0;
    static struct timespec last_update{0, 0};

    // If running in benchmark mode with concurrent clients, avoid stdout TTY mutex contention
    const char* env_nc = getenv("NUM_CLIENTS");
    if (env_nc && atoi(env_nc) > 1 && !force_complete) {
        return;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // Throttle updates to ~10 FPS (100ms) unless force_complete
    if (!force_complete && last_update.tv_sec != 0) {
        long elapsed_ms = (now.tv_sec - last_update.tv_sec) * 1000 +
                          (now.tv_nsec - last_update.tv_nsec) / 1000000;
        if (elapsed_ms < 100) {
            return;
        }
    }
    last_update = now;

    static bool is_terminal = (isatty(STDOUT_FILENO) != 0);
    if (!is_terminal) {
        if (force_complete || (total_files > 0 && current_files > 0 && (current_files % 100 == 0 || current_files == total_files))) {
            double pct = total_bytes > 0 ? (static_cast<double>(current_bytes) * 100.0) / static_cast<double>(total_bytes)
                                         : (total_files > 0 ? (static_cast<double>(current_files) * 100.0) / static_cast<double>(total_files) : 0.0);
            double mb = static_cast<double>(current_bytes) / (1024.0 * 1024.0);
            log_event("INFO", "[Trial %d/%d%s] Progress: %u/%u files (%.1f%%) - %.2f MB",
                      trial_num, max_trials, has_ckpt ? " (CKPT)" : "",
                      current_files, total_files, pct, mb);
        }
        return;
    }

    // Cache terminal width to avoid frequent ioctl syscalls
    static int cached_cols = 80;
    static time_t last_cols_query = 0;
    if (now.tv_sec - last_cols_query >= 2) {
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20) {
            cached_cols = ws.ws_col;
        }
        last_cols_query = now.tv_sec;
    }
    int cols = cached_cols;

    double pct = 0.0;
    if (total_bytes > 0) {
        pct = (static_cast<double>(current_bytes) / static_cast<double>(total_bytes)) * 100.0;
    } else if (total_files > 0) {
        pct = (static_cast<double>(current_files) / static_cast<double>(total_files)) * 100.0;
    }
    if (pct > 100.0) pct = 100.0;
    if (force_complete) pct = 100.0;

    double elapsed_sec = (double)(now.tv_sec - trial_start_time.tv_sec) +
                         (double)(now.tv_nsec - trial_start_time.tv_nsec) * 1e-9;
    double speed_mbps = (elapsed_sec > 0.05) ? (static_cast<double>(trial_wire_bytes) / (elapsed_sec * 1024.0 * 1024.0)) : 0.0;

    char spinner = spinner_chars[spinner_idx % 4];
    spinner_idx++;

    double cur_mb = static_cast<double>(current_bytes) / (1024.0 * 1024.0);
    double tot_mb = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
    double waste_mb = static_cast<double>(redundant_bytes) / (1024.0 * 1024.0);

    char trial_buf[40];
    if (has_ckpt) {
        if (cols >= 86) {
            snprintf(trial_buf, sizeof(trial_buf), "[Trial %d/%d] [CKPT]", trial_num, max_trials);
        } else {
            snprintf(trial_buf, sizeof(trial_buf), "[T%d/%d-CKPT]", trial_num, max_trials);
        }
    } else {
        snprintf(trial_buf, sizeof(trial_buf), "[Trial %d/%d]", trial_num, max_trials);
    }
    int trial_len = static_cast<int>(strlen(trial_buf));

    char files_buf[32];
    snprintf(files_buf, sizeof(files_buf), "%u/%u", current_files, total_files);
    int files_len = static_cast<int>(strlen(files_buf));

    char mb_buf[40];
    if (cols >= 95) {
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
        } else if (cols >= 86) {
            snprintf(waste_display, sizeof(waste_display), " | \033[1;31mW:%.1fM\033[0m", waste_mb);
            char tmp[32];
            snprintf(tmp, sizeof(tmp), " | W:%.1fM", waste_mb);
            waste_vis_len = static_cast<int>(strlen(tmp));
        }
    }

    int tail_len = force_complete ? 13 : 4;
    int non_bar_len = trial_len + 1 + 2 + 1 + 7 + 3 + files_len + 3 + mb_len + waste_vis_len + 3 + speed_len + tail_len;

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

int run_client_case2_loop(const char* server_ip, uint16_t port,
                          const char* topic, const char* output_dir,
                          int max_retries, uint32_t ckpt_window) {
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
    uint32_t total_files_in_topic = 0;
    uint64_t expected_total_bytes = 0;
    uint64_t cumulative_useful_bytes = 0;

    log_event("INFO", "Connecting to %s:%u...", server_ip, port);

    for (int attempt = 0; attempt < max_retries; ++attempt) {
        struct timespec t_trial_start;
        clock_gettime(CLOCK_MONOTONIC, &t_trial_start);
        uint64_t trial_wire_bytes = 0;

        if (attempt > 0) {
            retries_count++;
            int shift = (attempt < 20) ? attempt : 20;
            int delay_ms = std::min(1000, 50 * (1 << shift));
            log_event("INFO", "Case 2 retry attempt %d/%d after backoff %d ms...", attempt, max_retries, delay_ms);
            struct timespec sleep_ts;
            sleep_ts.tv_sec = delay_ms / 1000;
            sleep_ts.tv_nsec = (delay_ms % 1000) * 1000000L;
            nanosleep(&sleep_ts, nullptr);
        }

        CheckpointRecord ckpt;
        std::memset(&ckpt, 0, sizeof(ckpt));
        bool has_ckpt = load_checkpoint(output_dir, topic, &ckpt);

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

        // Disable Nagle's algorithm + tune socket buffers to 256 KiB
        {
            int nd = 1, bsz = 256 * 1024;
            setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
            setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &bsz, sizeof(bsz));
            setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &bsz, sizeof(bsz));
        }

        if (has_ckpt) {
            log_event("INFO", "Loaded checkpoint for '%s': session='%s' file_index=%u offset=%llu filename='%s'",
                      topic, ckpt.session_id_hex, ckpt.file_index,
                      static_cast<unsigned long long>(ckpt.byte_offset), ckpt.file_name);
            log_event("INFO", "Connected to server. Resuming topic '%s' with checkpoint (Trial %d/%d)...",
                      topic, attempt + 1, max_retries);

            if (send_resume(sockfd, ckpt.session_id, ckpt.file_index, ckpt.byte_offset,
                            topic, ckpt.file_name, &total_wire_bytes) < 0) {
                log_event("ERROR", "Failed to send OP_RESUME");
                close(sockfd);
                continue;
            }

            FrameHeader resp_hdr;
            std::vector<uint8_t> resp_payload;
            int rc = recv_frame(sockfd, &resp_hdr, resp_payload, &total_wire_bytes);
            if (rc <= 0) {
                log_event("INFO", "Server dropped connection after OP_RESUME");
                close(sockfd);
                continue;
            }

            if (resp_hdr.type == OP_RESTART) {
                MsgStatus st;
                if (parse_status_msg(resp_payload.data(), static_cast<uint32_t>(resp_payload.size()), &st)) {
                    log_event("INFO", "Server sent OP_RESTART (code=%u, reason='%s'). Clearing checkpoint and restarting fresh.",
                              st.code, st.reason.c_str());
                }
                remove_checkpoint(output_dir, topic);
                has_ckpt = false;
                cumulative_useful_bytes = 0;
                files_received_count = 0;
                if (send_get(sockfd, topic, &total_wire_bytes) < 0) {
                    close(sockfd);
                    continue;
                }
            } else if (resp_hdr.type == OP_RESUME_OK) {
                MsgResumeOk rok;
                if (parse_resume_ok(resp_payload.data(), static_cast<uint32_t>(resp_payload.size()), &rok)) {
                    if (expected_total_bytes == 0) {
                        expected_total_bytes = ckpt.byte_offset + rok.rem_bytes;
                        total_files_in_topic = ckpt.files_done + rok.rem_files;
                    }
                    cumulative_useful_bytes = (expected_total_bytes > rok.rem_bytes) ? (expected_total_bytes - rok.rem_bytes) : 0;
                    files_received_count = (total_files_in_topic > rok.rem_files) ? (total_files_in_topic - rok.rem_files) : 0;

                    log_event("INFO", "Server accepted RESUME: file_index=%u accepted_offset=%llu rem_files=%u rem_bytes=%llu",
                              rok.file_index, static_cast<unsigned long long>(rok.accepted_offset),
                              rok.rem_files, static_cast<unsigned long long>(rok.rem_bytes));

                    double preserved_mb = static_cast<double>(cumulative_useful_bytes) / (1024.0 * 1024.0);
                    double tot_mb = static_cast<double>(expected_total_bytes) / (1024.0 * 1024.0);
                    double resume_pct = expected_total_bytes > 0 ? (static_cast<double>(cumulative_useful_bytes) * 100.0) / static_cast<double>(expected_total_bytes) : 0.0;
                    if (isatty(STDOUT_FILENO)) {
                        printf("\033[1;36m[STATEFUL RESUMPTION]\033[0m Resuming at File %u/%u (offset %llu B) | Progress: %.1f%% (%.2f/%.2f MB preserved)\n",
                               rok.file_index, total_files_in_topic, static_cast<unsigned long long>(rok.accepted_offset),
                               resume_pct, preserved_mb, tot_mb);
                        fflush(stdout);
                    }
                }
            } else if (resp_hdr.type == OP_ERROR) {
                MsgStatus st;
                if (parse_status_msg(resp_payload.data(), static_cast<uint32_t>(resp_payload.size()), &st)) {
                    log_event("ERROR", "Server sent OP_ERROR (code=%u, reason='%s')", st.code, st.reason.c_str());
                }
                close(sockfd);
                return -1;
            } else {
                log_event("ERROR", "Unexpected response opcode %u after OP_RESUME", resp_hdr.type);
                close(sockfd);
                continue;
            }
        } else {
            log_event("INFO", "Connected to server. Requesting topic '%s' (Trial %d/%d)...",
                      topic, attempt + 1, max_retries);
            if (send_get(sockfd, topic, &total_wire_bytes) < 0) {
                log_event("ERROR", "Failed to send OP_GET");
                close(sockfd);
                continue;
            }
        }

        int current_file_fd = -1;
        uint64_t current_file_bytes_written = 0;
        uint64_t bytes_since_last_ckpt = 0;
        bool connection_failed = false;

        while (true) {
            FrameHeader hdr;
            std::vector<uint8_t> payload;
            int rc = recv_frame(sockfd, &hdr, payload, &total_wire_bytes);

            if (rc <= 0) {
                const char* fault_desc = (rc == FRAME_PROTO_ERR) ? "Corrupted frame header / protocol error" :
                                         (rc == 0) ? "Connection closed prematurely by server" :
                                                     "Connection dropped / fault injected";
                if (current_file_fd >= 0) {
                    if (current_file_bytes_written > ckpt.byte_offset) {
                        ckpt.byte_offset = current_file_bytes_written;
                        save_checkpoint(output_dir, &ckpt, current_file_fd);
                    }
                    close(current_file_fd);
                    current_file_fd = -1;
                }
                if (isatty(STDOUT_FILENO)) {
                    double preserved_mb = static_cast<double>(cumulative_useful_bytes) / (1024.0 * 1024.0);
                    printf("\n\033[1;33m[FAULT DETECTED / CHECKPOINT SAVED]\033[0m %s at File %u/%u (offset %llu B). Stateful recovery: saved checkpoint, resuming on Trial %d/%d (%.2f MB preserved)...\n",
                           fault_desc, ckpt.file_index, total_files_in_topic,
                           static_cast<unsigned long long>(ckpt.byte_offset),
                           attempt + 2, max_retries, preserved_mb);
                    fflush(stdout);
                }
                log_event("INFO", "Server dropped connection (%s, rc=%d) mid-transfer at attempt %d", fault_desc, rc, attempt);
                close(sockfd);
                sockfd = -1;
                connection_failed = true;
                break;
            }

            if (hdr.type == OP_GET_OK) {
                MsgGetOk get_ok;
                if (parse_get_ok(payload.data(), static_cast<uint32_t>(payload.size()), &get_ok)) {
                    std::memcpy(ckpt.session_id, get_ok.session_id, 16);
                    session_id_to_hex(ckpt.session_id, ckpt.session_id_hex);
                    std::strncpy(ckpt.topic, topic, sizeof(ckpt.topic) - 1);
                    ckpt.topic[sizeof(ckpt.topic) - 1] = '\0';
                    ckpt.file_index = 0;
                    ckpt.byte_offset = 0;
                    ckpt.files_done = 0;
                    ckpt.file_name[0] = '\0';
                    total_files_in_topic = get_ok.file_count;
                    expected_total_bytes = get_ok.total_bytes;
                    cumulative_useful_bytes = 0;
                    files_received_count = 0;
                    double total_mb = static_cast<double>(get_ok.total_bytes) / (1024.0 * 1024.0);
                    log_event("INFO", "Received OP_GET_OK: %u files, %llu total bytes (%.2f MB)",
                              get_ok.file_count,
                              static_cast<unsigned long long>(get_ok.total_bytes), total_mb);
                }
            } else if (hdr.type == OP_ERROR) {
                MsgStatus st;
                if (parse_status_msg(payload.data(), static_cast<uint32_t>(payload.size()), &st)) {
                    log_event("ERROR", "Server returned OP_ERROR code %u: %s", st.code, st.reason.c_str());
                    if (current_file_fd >= 0) {
                        close(current_file_fd);
                        current_file_fd = -1;
                    }
                    close(sockfd);
                    if (st.code == ERR_TOPIC_NOT_FOUND || st.code == ERR_BAD_REQUEST) {
                        return -1;
                    }
                    connection_failed = true;
                    break;
                }
            } else if (hdr.type == OP_FILE_BEGIN) {
                MsgFileBegin fb;
                if (parse_file_begin(payload.data(), static_cast<uint32_t>(payload.size()), &fb)) {
                    if (current_file_fd >= 0) {
                        close(current_file_fd);
                        current_file_fd = -1;
                    }

                    std::string dest_path = topic_dest_dir + "/" + fb.filename;
                    if (fb.byte_offset > 0) {
                        current_file_fd = open_and_seek_resume_file(dest_path.c_str(), fb.byte_offset);
                    } else {
                        current_file_fd = open(dest_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    }

                    if (current_file_fd < 0) {
                        log_event("ERROR", "Failed to open destination file '%s': %s", dest_path.c_str(), strerror(errno));
                        close(sockfd);
                        return -1;
                    }

                    // Pre-allocate disk space to avoid fragmentation and metadata stalls
                    if (fb.bytes_following > 0) {
                        posix_fallocate(current_file_fd, 0, static_cast<off_t>(fb.bytes_following + fb.byte_offset));
                    }

                    current_file_bytes_written = fb.byte_offset;
                    bytes_since_last_ckpt = 0;

                    ckpt.file_index = fb.file_index;
                    ckpt.byte_offset = fb.byte_offset;
                    std::strncpy(ckpt.file_name, fb.filename.c_str(), sizeof(ckpt.file_name) - 1);
                    ckpt.file_name[sizeof(ckpt.file_name) - 1] = '\0';

                    save_checkpoint(output_dir, &ckpt, current_file_fd, sockfd);

                    render_case2_progress_bar(attempt + 1, max_retries, has_ckpt,
                                              files_received_count, total_files_in_topic,
                                              cumulative_useful_bytes, expected_total_bytes,
                                              redundant_bytes, t_trial_start, trial_wire_bytes);
                }
            } else if (hdr.type == OP_FILE_DATA) {
                MsgFileData fd_msg;
                if (parse_file_data(payload.data(), static_cast<uint32_t>(payload.size()), &fd_msg)) {
                    if (current_file_fd >= 0 && !fd_msg.data.empty()) {
                        ssize_t written = write(current_file_fd, fd_msg.data.data(), fd_msg.data.size());
                        if (written < 0 || static_cast<size_t>(written) != fd_msg.data.size()) {
                            log_event("ERROR", "Write error on destination file fd=%d: %s",
                                      current_file_fd, strerror(errno));
                            close(current_file_fd);
                            close(sockfd);
                            return -1;
                        }
                        current_file_bytes_written += fd_msg.data.size();
                        cumulative_useful_bytes += fd_msg.data.size();
                        trial_wire_bytes += fd_msg.data.size();
                        bytes_since_last_ckpt += fd_msg.data.size();

                        if (bytes_since_last_ckpt >= ckpt_window) {
                            ckpt.byte_offset = current_file_bytes_written;
                            save_checkpoint(output_dir, &ckpt, current_file_fd, sockfd);
                            bytes_since_last_ckpt = 0;
                        }

                        static uint64_t last_rendered_bytes = 0;
                        if (cumulative_useful_bytes - last_rendered_bytes >= 524288) {
                            render_case2_progress_bar(attempt + 1, max_retries, has_ckpt,
                                                      files_received_count, total_files_in_topic,
                                                      cumulative_useful_bytes, expected_total_bytes,
                                                      redundant_bytes, t_trial_start, trial_wire_bytes);
                            last_rendered_bytes = cumulative_useful_bytes;
                        }
                    }
                }
            } else if (hdr.type == OP_FILE_END) {
                MsgFileEnd fe;
                if (parse_file_end(payload.data(), static_cast<uint32_t>(payload.size()), &fe)) {
                    if (current_file_fd >= 0) {
                        fsync(current_file_fd);
                        close(current_file_fd);
                        current_file_fd = -1;
                    }

                    files_received_count++;
                    ckpt.files_done = fe.file_index + 1;
                    ckpt.file_index = fe.file_index + 1;
                    ckpt.byte_offset = 0;
                    bytes_since_last_ckpt = 0;

                    if (total_files_in_topic > 0 && ckpt.file_index == total_files_in_topic) {
                        std::strncpy(ckpt.file_name, "EOF", sizeof(ckpt.file_name) - 1);
                    } else {
                        ckpt.file_name[0] = '\0';
                    }

                    save_checkpoint(output_dir, &ckpt, -1, sockfd);

                    render_case2_progress_bar(attempt + 1, max_retries, has_ckpt,
                                              files_received_count, total_files_in_topic,
                                              cumulative_useful_bytes, expected_total_bytes,
                                              redundant_bytes, t_trial_start, trial_wire_bytes);
                }
            } else if (hdr.type == OP_TRANSFER_END) {
                MsgTransferEnd te;
                if (parse_transfer_end(payload.data(), static_cast<uint32_t>(payload.size()), &te)) {
                    useful_bytes = te.useful_bytes;
                    transfer_completed = true;
                    remove_checkpoint(output_dir, topic);
                    render_case2_progress_bar(attempt + 1, max_retries, has_ckpt,
                                              total_files_in_topic, total_files_in_topic,
                                              useful_bytes, useful_bytes,
                                              redundant_bytes, t_trial_start, trial_wire_bytes,
                                              true /* force_complete */);
                    log_event("INFO", "Transfer completed successfully (%u files, %llu bytes)",
                              te.files_sent, static_cast<unsigned long long>(te.useful_bytes));
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
        log_event("ERROR", "Case 2 transfer failed after %d retries", max_retries);
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);

    std::string csv_path = "Results/Part2_metrics.csv";
    if (access("Results", F_OK) != 0 && access("../Results", F_OK) == 0) {
        csv_path = "../Results/Part2_metrics.csv";
    }

    redundant_bytes = (total_wire_bytes > useful_bytes) ? (total_wire_bytes - useful_bytes) : 0;
    record_transfer_metrics(csv_path.c_str(), "Part2_Case2_Stateful", 1, 0.0,
                            useful_bytes, total_wire_bytes, redundant_bytes,
                            t_start, t_end, retries_count);

    double duration_sec = (double)(t_end.tv_sec - t_start.tv_sec) + 
                          (double)(t_end.tv_nsec - t_start.tv_nsec) * 1e-9;
    if (duration_sec < 0.0) duration_sec = 0.0;
    double goodput_mbps = (duration_sec > 0.0) ? (useful_bytes / (duration_sec * 1000000.0)) : 0.0;
    double throughput_mbps = (duration_sec > 0.0) ? (total_wire_bytes / (duration_sec * 1000000.0)) : 0.0;
    double overhead_pct = (useful_bytes > 0) ? 
        (((double)total_wire_bytes - (double)useful_bytes) / (double)useful_bytes) * 100.0 : 0.0;

    log_event("INFO", "Transfer SUCCESS: %u files (%llu bytes) written to '%s/%s' in %d retries (wire: %llu, redundant: %llu)",
              files_received_count, static_cast<unsigned long long>(useful_bytes),
              output_dir, topic, retries_count,
              static_cast<unsigned long long>(total_wire_bytes),
              static_cast<unsigned long long>(redundant_bytes));
    log_event("INFO", "Performance: Duration = %.2fs | Goodput = %.2f MB/s | Wire Throughput = %.2f MB/s | Overhead = %.2f%%",
              duration_sec, goodput_mbps, throughput_mbps, overhead_pct);
    return 0;
}
