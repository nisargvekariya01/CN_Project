#ifndef CONCURRENCY_H
#define CONCURRENCY_H

#include "common_types.h"
#include <signal.h>
#include <unordered_map>
#include <string>

/*
 * concurrency.h - Member 1 (PR #3, Wave 1)
 *
 * Provides CLI validation, TCP listening socket initialization, POSIX signal
 * handling (SIGPIPE ignore, clean SIGINT/SIGTERM shutdown), and execution loops
 * for both sequential (--single-threaded) and concurrent (pthread) serving.
 */

// Global flag modified asynchronously by signal handlers
extern volatile sig_atomic_t g_keep_running;

// Strictly validates auto-grader positional CLI:
// Part I:  ./server <port> <topics_root_dir> [--single-threaded] [--seed <val>]
// Part II: ./server <port> <topics_root_dir> <failure_probability> [--single-threaded] [--seed <val>]
int parse_server_cli(int argc, char* argv[], int part_num, uint16_t* out_port,
                     std::string* out_root_dir, double* out_pf, int* out_threaded_mode,
                     uint32_t* out_seed);

// Creates a TCP socket, binds to INADDR_ANY:port, sets SO_REUSEADDR, and begins listening.
int create_listening_socket(uint16_t port);

// Configures signal handlers: ignores SIGPIPE, sets g_keep_running = 0 on SIGINT/SIGTERM.
void register_signal_handlers();

// Single-threaded event loop: handles clients sequentially on main thread.
void run_sequential_loop(int listen_fd, const std::unordered_map<std::string, TopicEntry>& topic_map,
                         double pf = 0.0, int case_mode = 0, uint32_t seed = 0);

// Multi-threaded event loop: accepts connections and spawns detached pthread workers.
void run_concurrent_loop(int listen_fd, const std::unordered_map<std::string, TopicEntry>& topic_map,
                         double pf, int case_mode, uint32_t seed);

// Worker thread routine executed by pthread_create() for each client connection.
void* worker_thread(void* arg);

// Core client request dispatcher: handles OP_GET / OP_RESUME / OP_GET_ENHANCED.
int process_client(int client_fd, const std::unordered_map<std::string, TopicEntry>& topic_map,
                   double pf, int case_mode, uint32_t seed);

#endif // CONCURRENCY_H
