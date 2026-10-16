#include "enhanced_parallel_client.h"
#include "protocol.h"
#include "fault.h"
#include "metrics_logger.h"

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
#include <unistd.h>

/*
 * enhanced_parallel_client.cpp - Member 5 (PR #6, Wave 3)
 *
 * Implements:
 *   1. Parallel TCP sub-stream pool (N = 4 streams default, configurable via CLI)
 *   2. Target file pre-allocation via posix_fallocate() / ftruncate()
 *   3. Lock-free concurrent client file assembly via positional pwrite()
 *   4. Sub-stream fault isolation and dynamic bitmask resynchronization
 *   5. Entry point run_enhanced_client() invoked by client.cpp
 */

static void ensure_parent_dirs(const std::string& filepath) {
    size_t last_slash = filepath.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        std::string dir = filepath.substr(0, last_slash);
        std::string current;
        for (size_t i = 0; i < dir.size(); ++i) {
            char c = dir[i];
            if (c == '/' || c == '\\') {
                if (!current.empty() && current != "." && current != "..") {
                    mkdir(current.c_str(), 0755);
                }
            }
            current += c;
        }
        if (!current.empty()) {
            mkdir(current.c_str(), 0755);
        }
    }
}

int preallocate_target_file(const std::string& path, uint64_t file_size) {
    ensure_parent_dirs(path);
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        log_event("ERROR", "open failed for '%s': %s", path.c_str(), strerror(errno));
        return -1;
    }

    if (file_size > 0) {
        int prc = posix_fallocate(fd, 0, static_cast<off_t>(file_size));
        if (prc != 0) {
            if (ftruncate(fd, static_cast<off_t>(file_size)) != 0) {
                log_event("WARNING", "ftruncate fallback failed for '%s': %s", path.c_str(), strerror(errno));
            }
        }
    } else {
        if (ftruncate(fd, 0) != 0) {
            log_event("WARNING", "ftruncate(0) failed for '%s': %s", path.c_str(), strerror(errno));
        }
    }

    return fd;
}

bool mark_chunk_committed(SharedBitmaskState* state, uint32_t unit_id, uint32_t chunk_len) {
    if (!state) return false;
    pthread_mutex_lock(&state->bmap_mutex);
    bool is_new = false;
    if (unit_id < state->total_units) {
        size_t byte_idx = unit_id / 8;
        uint8_t bit_mask = static_cast<uint8_t>(1 << (unit_id % 8));
        if ((state->chunk_bitmap[byte_idx] & bit_mask) == 0) {
            state->chunk_bitmap[byte_idx] |= bit_mask;
            state->committed_units++;
            state->useful_bytes += chunk_len;
            is_new = true;
        } else {
            state->redundant_bytes += chunk_len;
        }
    }
    pthread_mutex_unlock(&state->bmap_mutex);
    return is_new;
}

bool is_transfer_complete(SharedBitmaskState* state) {
    if (!state) return false;
    pthread_mutex_lock(&state->bmap_mutex);
    bool complete = (state->committed_units >= state->total_units);
    pthread_mutex_unlock(&state->bmap_mutex);
    return complete;
}

void* client_parallel_worker(void* arg) {
    EnhancedClientWorkerArgs* args = static_cast<EnhancedClientWorkerArgs*>(arg);
    SharedBitmaskState* state = args->shared_state;
    uint32_t conn_id = args->conn_id;

    for (int attempt = 0; attempt < args->max_retries; ++attempt) {
        if (is_transfer_complete(state)) {
            break;
        }

        if (attempt > 0) {
            pthread_mutex_lock(&state->bmap_mutex);
            state->retries++;
            pthread_mutex_unlock(&state->bmap_mutex);

            int shift = (attempt < 10) ? attempt : 10;
            int delay_ms = std::min(1000, 25 * (1 << shift));
            log_event("INFO", "Sub-stream %u retrying attempt %d after %d ms...",
                      conn_id, attempt, delay_ms);
            struct timespec ts;
            ts.tv_sec = delay_ms / 1000;
            ts.tv_nsec = (delay_ms % 1000) * 1000000L;
            nanosleep(&ts, nullptr);
        }

        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            continue;
        }

        struct sockaddr_in saddr;
        std::memset(&saddr, 0, sizeof(saddr));
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons(args->port);
        inet_pton(AF_INET, args->server_ip.c_str(), &saddr.sin_addr);

        if (connect(sockfd, reinterpret_cast<struct sockaddr*>(&saddr), sizeof(saddr)) < 0) {
            close(sockfd);
            continue;
        }

        // Take thread-safe snapshot of bitmask for resynchronization
        std::vector<uint8_t> bmap_snapshot;
        pthread_mutex_lock(&state->bmap_mutex);
        bmap_snapshot = state->chunk_bitmap;
        pthread_mutex_unlock(&state->bmap_mutex);

        uint64_t stream_wire = 0;
        int jrc = send_join_session(sockfd, args->session_id, conn_id,
                                    bmap_snapshot.data(),
                                    static_cast<uint32_t>(bmap_snapshot.size()),
                                    &stream_wire);
        if (jrc < 0) {
            close(sockfd);
            continue;
        }

        bool stream_faulted = false;
        while (!is_transfer_complete(state)) {
            FrameHeader hdr;
            std::vector<uint8_t> payload;
            int rrc = recv_frame(sockfd, &hdr, payload, &stream_wire);
            if (rrc <= 0) {
                // Connection dropped due to fault injection or network event
                log_event("INFO", "Sub-stream %u disconnected (rc=%d). Surviving streams continue uninterrupted.",
                          conn_id, rrc);
                stream_faulted = true;
                break;
            }

            if (hdr.type == OP_FILE_DATA) {
                MsgFileData fd_msg;
                if (!parse_file_data(payload.data(), static_cast<uint32_t>(payload.size()), &fd_msg)) {
                    log_event("ERROR", "Sub-stream %u received malformed OP_FILE_DATA", conn_id);
                    stream_faulted = true;
                    break;
                }

                if (fd_msg.file_index < state->open_fds.size() && state->open_fds[fd_msg.file_index] >= 0) {
                    int target_fd = state->open_fds[fd_msg.file_index];
                    size_t to_write = fd_msg.data.size();
                    size_t written = 0;

                    // LOCK-FREE POSITIONAL PWRITE DIRECTLY INTO PRE-ALLOCATED FILE
                    while (written < to_write) {
                        ssize_t w = pwrite(target_fd, fd_msg.data.data() + written,
                                           to_write - written,
                                           static_cast<off_t>(fd_msg.offset + written));
                        if (w <= 0) {
                            log_event("ERROR", "pwrite failed on fd=%d offset=%llu: %s",
                                      target_fd, static_cast<unsigned long long>(fd_msg.offset),
                                      strerror(errno));
                            break;
                        }
                        written += static_cast<size_t>(w);
                    }

                    // Update bitmask and accounting
                    if (fd_msg.file_index < args->file_chunk_bases.size()) {
                        uint32_t unit_id = static_cast<uint32_t>(
                            args->file_chunk_bases[fd_msg.file_index] + (fd_msg.offset / CHUNK_SIZE));
                        mark_chunk_committed(state, unit_id, static_cast<uint32_t>(fd_msg.data.size()));
                    }
                }
            } else if (hdr.type == OP_TRANSFER_END) {
                log_event("INFO", "Sub-stream %u received OP_TRANSFER_END", conn_id);
                break;
            } else if (hdr.type == OP_ERROR || hdr.type == OP_RESTART) {
                log_event("INFO", "Sub-stream %u received opcode %u", conn_id, hdr.type);
                break;
            }
        }

        // Add local stream wire counter to total
        pthread_mutex_lock(&state->bmap_mutex);
        state->total_wire_bytes += stream_wire;
        pthread_mutex_unlock(&state->bmap_mutex);

        close(sockfd);

        if (!stream_faulted && is_transfer_complete(state)) {
            break;
        }
    }

    delete args;
    return nullptr;
}

int run_enhanced_client(const char* server_ip, uint16_t port,
                        const char* topic, const char* output_dir,
                        int num_streams, int max_retries) {
    if (!server_ip || !topic || !output_dir) {
        return -1;
    }

    if (num_streams <= 0) {
        num_streams = DEFAULT_PARALLEL_CONN;
    }
    if (max_retries <= 0) {
        max_retries = DEFAULT_MAX_RETRIES;
    }

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    uint64_t total_wire_bytes = 0;

    // Phase 1: Connect via control socket to initiate session
    int control_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (control_fd < 0) {
        log_event("ERROR", "Failed to create control socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in saddr;
    std::memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(port);
    inet_pton(AF_INET, server_ip, &saddr.sin_addr);

    if (connect(control_fd, reinterpret_cast<struct sockaddr*>(&saddr), sizeof(saddr)) < 0) {
        log_event("ERROR", "Control socket failed to connect to %s:%u: %s", server_ip, port, strerror(errno));
        close(control_fd);
        return -1;
    }

    if (send_get_enhanced(control_fd, static_cast<uint16_t>(num_streams), topic, &total_wire_bytes) < 0) {
        log_event("ERROR", "Failed to send OP_GET_ENHANCED for topic '%s'", topic);
        close(control_fd);
        return -1;
    }

    // Receive OP_GET_OK
    FrameHeader hdr;
    std::vector<uint8_t> payload;
    int rrc = recv_frame(control_fd, &hdr, payload, &total_wire_bytes);
    if (rrc <= 0 || hdr.type != OP_GET_OK) {
        log_event("ERROR", "Failed to receive OP_GET_OK from server (rrc=%d, type=%u)", rrc, hdr.type);
        close(control_fd);
        return -1;
    }

    MsgGetOk get_ok;
    if (!parse_get_ok(payload.data(), static_cast<uint32_t>(payload.size()), &get_ok)) {
        log_event("ERROR", "Malformed OP_GET_OK received");
        close(control_fd);
        return -1;
    }

    uint8_t session_id[16];
    std::memcpy(session_id, get_ok.session_id, 16);
    char hex32[33];
    session_id_to_hex(session_id, hex32);
    log_event("INFO", "Session established: id='%s', files=%u, total_bytes=%llu",
              hex32, get_ok.file_count, static_cast<unsigned long long>(get_ok.total_bytes));

    // Receive OP_FILE_BEGIN for each file in the topic
    std::vector<FileMeta> files(get_ok.file_count);
    for (uint32_t i = 0; i < get_ok.file_count; ++i) {
        rrc = recv_frame(control_fd, &hdr, payload, &total_wire_bytes);
        if (rrc <= 0 || hdr.type != OP_FILE_BEGIN) {
            log_event("ERROR", "Expected OP_FILE_BEGIN for file %u, got rc=%d type=%u", i, rrc, hdr.type);
            close(control_fd);
            return -1;
        }

        MsgFileBegin fb;
        if (!parse_file_begin(payload.data(), static_cast<uint32_t>(payload.size()), &fb)) {
            log_event("ERROR", "Malformed OP_FILE_BEGIN received for file %u", i);
            close(control_fd);
            return -1;
        }

        if (fb.file_index < files.size()) {
            files[fb.file_index].filename = fb.filename;
            files[fb.file_index].file_size = fb.bytes_following;
            files[fb.file_index].full_path = fb.filename;
        }
    }

    close(control_fd);

    // Phase 2: Pre-allocate target files on disk
    std::string topic_dir = std::string(output_dir) + "/" + topic;
    std::vector<int> open_fds(files.size(), -1);
    for (size_t f = 0; f < files.size(); ++f) {
        std::string full_dest = topic_dir + "/" + files[f].filename;
        int fd = preallocate_target_file(full_dest, files[f].file_size);
        if (fd < 0) {
            log_event("ERROR", "Failed to pre-allocate file '%s'", full_dest.c_str());
            for (int ofd : open_fds) { if (ofd >= 0) close(ofd); }
            return -1;
        }
        open_fds[f] = fd;
    }

    // Phase 3: Setup work units mapping and bitmask
    uint32_t total_units = 0;
    std::vector<uint64_t> file_chunk_bases(files.size(), 0);
    for (size_t f = 0; f < files.size(); ++f) {
        file_chunk_bases[f] = total_units;
        uint64_t fsize = files[f].file_size;
        if (fsize == 0) {
            total_units++;
        } else {
            total_units += static_cast<uint32_t>((fsize + CHUNK_SIZE - 1) / CHUNK_SIZE);
        }
    }

    SharedBitmaskState shared_state;
    shared_state.topic = topic;
    shared_state.output_dir = output_dir;
    shared_state.topic_meta = nullptr;
    shared_state.open_fds = open_fds;
    shared_state.total_units = total_units;
    shared_state.committed_units = 0;
    shared_state.chunk_bitmap.assign((total_units + 7) / 8, 0);
    shared_state.useful_bytes = 0;
    shared_state.total_wire_bytes = total_wire_bytes;
    shared_state.redundant_bytes = 0;
    shared_state.retries = 0;
    pthread_mutex_init(&shared_state.bmap_mutex, nullptr);

    // Mark 0-byte file units as committed immediately
    for (size_t f = 0; f < files.size(); ++f) {
        if (files[f].file_size == 0) {
            uint32_t u_id = static_cast<uint32_t>(file_chunk_bases[f]);
            shared_state.chunk_bitmap[u_id / 8] |= static_cast<uint8_t>(1 << (u_id % 8));
            shared_state.committed_units++;
        }
    }

    log_event("INFO", "Initialized parallel client: %u streams, %u 64 KiB chunks across %zu files",
              num_streams, total_units, files.size());

    // Phase 4: Launch parallel sub-stream worker pool
    std::vector<pthread_t> threads(num_streams);
    for (int i = 0; i < num_streams; ++i) {
        EnhancedClientWorkerArgs* args = new EnhancedClientWorkerArgs();
        args->server_ip = server_ip;
        args->port = port;
        args->conn_id = static_cast<uint32_t>(i);
        std::memcpy(args->session_id, session_id, 16);
        args->max_retries = max_retries;
        args->shared_state = &shared_state;
        args->file_chunk_bases = file_chunk_bases;
        for (const auto& fm : files) {
            args->file_sizes.push_back(fm.file_size);
        }
        pthread_create(&threads[i], nullptr, client_parallel_worker, args);
    }

    // Wait for all sub-streams to complete
    for (int i = 0; i < num_streams; ++i) {
        pthread_join(threads[i], nullptr);
    }

    // Phase 5: Flush and close all pre-allocated files
    for (int ofd : open_fds) {
        if (ofd >= 0) {
            fsync(ofd);
            close(ofd);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);

    bool success = (shared_state.committed_units >= shared_state.total_units);
    if (!success) {
        log_event("ERROR", "Enhanced parallel transfer failed: committed %u/%u units after %d retries",
                  shared_state.committed_units, shared_state.total_units, shared_state.retries);
        pthread_mutex_destroy(&shared_state.bmap_mutex);
        return -1;
    }

    std::string csv_path = "Results/Part2_metrics.csv";
    if (access("Results", F_OK) != 0 && access("../Results", F_OK) == 0) {
        csv_path = "../Results/Part2_metrics.csv";
    }

    record_transfer_metrics(csv_path.c_str(), "Part2_Case2_Enhanced",
                            num_streams, 0.0,
                            shared_state.useful_bytes,
                            shared_state.total_wire_bytes,
                            shared_state.redundant_bytes,
                            t_start, t_end, shared_state.retries);

    log_event("INFO", "Enhanced parallel transfer SUCCESS: %zu files (%llu bytes) written to '%s' in %d retries",
              files.size(), static_cast<unsigned long long>(shared_state.useful_bytes),
              topic_dir.c_str(), shared_state.retries);

    pthread_mutex_destroy(&shared_state.bmap_mutex);
    return 0;
}
