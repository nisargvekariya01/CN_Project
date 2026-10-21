#include "concurrency.h"
#include "directory_mapper.h"
#include "protocol.h"
#include "metrics_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cstring>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>

volatile sig_atomic_t g_keep_running = 1;
static int g_listen_fd = -1;

// Weak hook for Member 3's stream_topic_to_client & init_thread_rng (Wave 2)
extern void init_thread_rng(uint32_t optional_seed) __attribute__((weak));
extern int stream_topic_to_client(int client_fd, const TopicEntry& entry,
                                  uint32_t start_file_idx, uint64_t start_offset,
                                  double pf, ServerSession* session_ptr,
                                  pthread_mutex_t* session_mutex) __attribute__((weak));

// Weak hooks for Member 4's stateful session management and validation (Wave 3)
extern int create_session(const char* topic, uint8_t out_session_id[16],
                          ServerSession** out_session_ptr) __attribute__((weak));
extern int validate_checkpoint(const CheckpointRecord* client_ckpt,
                               const std::unordered_map<std::string, TopicEntry>& topic_map,
                               uint16_t* out_err_code,
                               std::string* out_reason,
                               uint32_t* out_rem_files,
                               uint64_t* out_rem_bytes,
                               ServerSession** out_session_ptr) __attribute__((weak));
extern pthread_mutex_t* get_session_table_mutex() __attribute__((weak));
extern int process_enhanced_opcodes(int client_fd, const FrameHeader& hdr,
                                    const std::vector<uint8_t>& payload,
                                    const std::unordered_map<std::string, TopicEntry>& topic_map,
                                    double pf, uint64_t* wire_counter) __attribute__((weak));

int parse_server_cli(int argc, char* argv[], int part_num, uint16_t* out_port,
                     std::string* out_root_dir, double* out_pf, int* out_threaded_mode,
                     uint32_t* out_seed) {
    if (!out_port || !out_root_dir || !out_pf || !out_threaded_mode || !out_seed) {
        return -1;
    }

    *out_pf = 0.0;
    *out_threaded_mode = 1; // Default to multi-threaded
    *out_seed = 0;

    int min_args = (part_num == 1) ? 3 : 4;
    if (argc < min_args) {
        if (part_num == 1) {
            fprintf(stderr, "Usage: %s <port> <topics_root_dir> [--single-threaded] [--seed <val>]\n", argv[0]);
        } else {
            fprintf(stderr, "Usage: %s <port> <topics_root_dir> <failure_probability> [--single-threaded] [--seed <val>]\n", argv[0]);
        }
        return -1;
    }

    // Positional Arg 1: port [1024, 65535]
    char* endptr = nullptr;
    long port_val = strtol(argv[1], &endptr, 10);
    if (*endptr != '\0' || port_val < 1024 || port_val > 65535) {
        fprintf(stderr, "Error: Port must be an integer in [1024, 65535]. Got: %s\n", argv[1]);
        return -1;
    }
    *out_port = static_cast<uint16_t>(port_val);

    // Positional Arg 2: topics_root_dir must exist and be a directory
    struct stat st;
    if (stat(argv[2], &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: '%s' is not a valid directory\n", argv[2]);
        return -1;
    }
    *out_root_dir = argv[2];

    int opt_idx = 3;
    // Positional Arg 3: failure_probability for Part II in [0.0, 1.0]
    if (part_num == 2) {
        double pf_val = strtod(argv[3], &endptr);
        if (*endptr != '\0' || pf_val < 0.0 || pf_val > 1.0) {
            fprintf(stderr, "Error: Failure probability must be a float in [0.0, 1.0]. Got: %s\n", argv[3]);
            return -1;
        }
        *out_pf = pf_val;
        opt_idx = 4;
    }

    // Optional trailing flags
    for (int i = opt_idx; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--single-threaded") {
            *out_threaded_mode = 0;
        } else if (arg == "--seed" && i + 1 < argc) {
            *out_seed = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 10));
        } else if (arg.rfind("--seed=", 0) == 0) {
            *out_seed = static_cast<uint32_t>(strtoul(arg.substr(7).c_str(), nullptr, 10));
        } else {
            fprintf(stderr, "Warning: Unknown option '%s' ignored\n", arg.c_str());
        }
    }

    return 0;
}

static void handle_sigint_sigterm(int sig) {
    (void)sig;
    g_keep_running = 0;
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}

void register_signal_handlers() {
    // Ignore SIGPIPE so disconnects produce EPIPE / ECONNRESET instead of crashing
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint_sigterm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // Do not use SA_RESTART so blocking accept() unwinds immediately on EINTR

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

int create_listening_socket(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_event("ERROR", "Failed to create TCP socket: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
    // Pre-tune listen socket send/recv buffers (256 KiB each)
    int buf_size = 256 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        log_event("ERROR", "Failed to bind to port %u: %s", port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 128) < 0) {
        log_event("ERROR", "Failed to listen on socket: %s", strerror(errno));
        close(fd);
        return -1;
    }

    g_listen_fd = fd;
    log_event("INFO", "Listening socket created on port %u (fd=%d)", port, fd);
    return fd;
}

int process_client(int client_fd, const std::unordered_map<std::string, TopicEntry>& topic_map,
                   double pf, int case_mode, uint32_t seed) {
    if (init_thread_rng) {
        init_thread_rng(seed);
    }
    uint64_t wire_counter = 0;
    FrameHeader hdr;
    std::vector<uint8_t> payload;

    int frc = recv_frame(client_fd, &hdr, payload, &wire_counter);
    if (frc <= 0) {
        if (frc == FRAME_EOF) {
            log_event("INFO", "Client fd=%d disconnected before request", client_fd);
        } else {
            log_event("ERROR", "recv_frame error from client fd=%d (rc=%d)", client_fd, frc);
        }
        return -1;
    }

    if (hdr.type == OP_GET) {
        MsgGet req;
        if (!parse_get(payload.data(), static_cast<uint32_t>(payload.size()), &req)) {
            log_event("ERROR", "Malformed OP_GET received on fd=%d", client_fd);
            send_error(client_fd, ERR_BAD_REQUEST, "Malformed OP_GET payload", &wire_counter);
            return -1;
        }

        log_event("INFO", "Client requested topic: '%s'", req.topic.c_str());
        const TopicEntry* entry = get_topic_path(topic_map, req.topic);
        if (!entry) {
            log_event("ERROR", "Topic '%s' not found", req.topic.c_str());
            send_error(client_fd, ERR_TOPIC_NOT_FOUND, "Topic not found: " + req.topic, &wire_counter);
            return -1;
        }

        uint8_t sess_id[16];
        ServerSession* session_ptr = nullptr;
        pthread_mutex_t* session_mutex = nullptr;

        if (case_mode >= 2 && create_session && get_session_table_mutex) {
            create_session(entry->topic_name.c_str(), sess_id, &session_ptr);
            session_mutex = get_session_table_mutex();
        } else {
            zero_session_id(sess_id);
        }

        send_get_ok(client_fd, sess_id, static_cast<uint32_t>(entry->files.size()), entry->total_bytes, &wire_counter);
        log_event("INFO", "Sent OP_GET_OK for topic '%s' (%zu files, %llu bytes)",
                  entry->topic_name.c_str(), entry->files.size(),
                  static_cast<unsigned long long>(entry->total_bytes));

        if (stream_topic_to_client) {
            return stream_topic_to_client(client_fd, *entry, 0, 0, pf, session_ptr, session_mutex);
        } else {
            // Standalone Member 1 loop test stub
            send_transfer_end(client_fd, static_cast<uint32_t>(entry->files.size()), entry->total_bytes, &wire_counter);
            return 0;
        }
    } else if (hdr.type == OP_RESUME) {
        if (case_mode < 2) {
            send_error(client_fd, ERR_BAD_REQUEST, "OP_RESUME not supported in this mode", &wire_counter);
            return -1;
        }

        MsgResume req;
        if (!parse_resume(payload.data(), static_cast<uint32_t>(payload.size()), &req)) {
            log_event("ERROR", "Malformed OP_RESUME received on fd=%d", client_fd);
            send_restart(client_fd, ERR_BAD_REQUEST, "Malformed OP_RESUME payload", &wire_counter);
            return -1;
        }

        if (!validate_checkpoint) {
            send_restart(client_fd, ERR_SESSION_EXPIRED, "Session validation unavailable", &wire_counter);
            return 0;
        }

        CheckpointRecord client_ckpt;
        memset(&client_ckpt, 0, sizeof(client_ckpt));
        memcpy(client_ckpt.session_id, req.session_id, 16);
        session_id_to_hex(client_ckpt.session_id, client_ckpt.session_id_hex);
        strncpy(client_ckpt.topic, req.topic.c_str(), sizeof(client_ckpt.topic) - 1);
        client_ckpt.topic[sizeof(client_ckpt.topic) - 1] = '\0';
        client_ckpt.file_index = req.file_index;
        client_ckpt.byte_offset = req.byte_offset;
        client_ckpt.files_done = req.file_index;
        strncpy(client_ckpt.file_name, req.filename.c_str(), sizeof(client_ckpt.file_name) - 1);
        client_ckpt.file_name[sizeof(client_ckpt.file_name) - 1] = '\0';

        uint16_t err_code = 0;
        std::string reason;
        uint32_t rem_files = 0;
        uint64_t rem_bytes = 0;
        ServerSession* session_ptr = nullptr;

        int vrc = validate_checkpoint(&client_ckpt, topic_map, &err_code, &reason,
                                      &rem_files, &rem_bytes, &session_ptr);
        if (vrc != 0) {
            log_event("INFO", "Resume rejected (code=%u, reason='%s')", err_code, reason.c_str());
            send_restart(client_fd, err_code, reason, &wire_counter);
            return 0;
        }

        send_resume_ok(client_fd, client_ckpt.file_index, client_ckpt.byte_offset,
                       rem_files, rem_bytes, &wire_counter);
        log_event("INFO", "Resume accepted: file_index=%u, offset=%llu, rem_files=%u, rem_bytes=%llu",
                  client_ckpt.file_index, static_cast<unsigned long long>(client_ckpt.byte_offset),
                  rem_files, static_cast<unsigned long long>(rem_bytes));

        const TopicEntry* entry = get_topic_path(topic_map, client_ckpt.topic);
        if (!entry) {
            send_error(client_fd, ERR_TOPIC_NOT_FOUND, "Topic not found", &wire_counter);
            return -1;
        }

        if (rem_files == 0 && rem_bytes == 0) {
            // Terminal EOF resume: topic already finished!
            send_transfer_end(client_fd, static_cast<uint32_t>(entry->files.size()), entry->total_bytes, &wire_counter);
            return 0;
        }

        pthread_mutex_t* session_mutex = get_session_table_mutex ? get_session_table_mutex() : nullptr;
        if (stream_topic_to_client) {
            return stream_topic_to_client(client_fd, *entry, client_ckpt.file_index,
                                          client_ckpt.byte_offset, pf, session_ptr, session_mutex);
        } else {
            send_transfer_end(client_fd, static_cast<uint32_t>(entry->files.size()), entry->total_bytes, &wire_counter);
            return 0;
        }
    } else if (hdr.type == OP_CHECKPOINT_ACK) {
        MsgCkptAck ack;
        if (parse_ckpt_ack(payload.data(), static_cast<uint32_t>(payload.size()), &ack)) {
            log_event("INFO", "Received OP_CHECKPOINT_ACK for file_index %u offset %llu",
                      ack.file_index, static_cast<unsigned long long>(ack.ack_offset));
        }
        return 0;
    } else if (hdr.type == OP_GET_ENHANCED || hdr.type == OP_JOIN_SESSION) {
        if (process_enhanced_opcodes) {
            return process_enhanced_opcodes(client_fd, hdr, payload, topic_map, pf, &wire_counter);
        } else {
            send_error(client_fd, ERR_BAD_REQUEST, "Enhanced opcodes not supported", &wire_counter);
            return -1;
        }
    } else {
        log_event("ERROR", "Unexpected opcode %u received on fd=%d", hdr.type, client_fd);
        send_error(client_fd, ERR_BAD_REQUEST, "Unexpected opcode", &wire_counter);
        return -1;
    }
}

void run_sequential_loop(int listen_fd, const std::unordered_map<std::string, TopicEntry>& topic_map,
                         double pf, int case_mode, uint32_t seed) {
    log_event("INFO", "Entering single-threaded sequential accept loop");
    while (g_keep_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (!g_keep_running) {
                break;
            }
            log_event("ERROR", "accept() failed: %s", strerror(errno));
            break;
        }

        char ip_buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), ip_buf, INET_ADDRSTRLEN);
        log_event("INFO", "Accepted client from %s:%u (fd=%d)", ip_buf, ntohs(client_addr.sin_port), client_fd);

        // Disable Nagle's algorithm — flush frames immediately without 40ms delay
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        int buf_sz = 256 * 1024;
        setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &buf_sz, sizeof(buf_sz));
        setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &buf_sz, sizeof(buf_sz));

        process_client(client_fd, topic_map, pf, case_mode, seed);
        close(client_fd);
    }
    log_event("INFO", "Exited sequential accept loop");
}

void* worker_thread(void* arg) {
    WorkerArgs* args = static_cast<WorkerArgs*>(arg);
    if (!args) {
        return nullptr;
    }

    process_client(args->client_fd, *(args->topic_map), args->pf, args->case_mode, args->rng_seed);
    close(args->client_fd);
    delete args;
    return nullptr;
}

void run_concurrent_loop(int listen_fd, const std::unordered_map<std::string, TopicEntry>& topic_map,
                         double pf, int case_mode, uint32_t seed) {
    log_event("INFO", "Entering multi-threaded concurrent accept loop");
    while (g_keep_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (!g_keep_running) {
                break;
            }
            log_event("ERROR", "accept() failed: %s", strerror(errno));
            break;
        }

        char ip_buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), ip_buf, INET_ADDRSTRLEN);
        log_event("INFO", "Accepted client from %s:%u (fd=%d)", ip_buf, ntohs(client_addr.sin_port), client_fd);

        // Disable Nagle's algorithm — flush frames immediately without 40ms delay
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        int buf_sz = 256 * 1024;
        setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &buf_sz, sizeof(buf_sz));
        setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &buf_sz, sizeof(buf_sz));

        WorkerArgs* args = new WorkerArgs();
        args->client_fd = client_fd;
        args->topic_map = &topic_map;
        args->pf = pf;
        args->case_mode = case_mode;
        args->rng_seed = seed;

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        int rc = pthread_create(&tid, &attr, worker_thread, args);
        pthread_attr_destroy(&attr);

        if (rc != 0) {
            log_event("ERROR", "Failed to create worker thread: %s", strerror(rc));
            close(client_fd);
            delete args;
        }
    }
    log_event("INFO", "Exited concurrent accept loop");
}
