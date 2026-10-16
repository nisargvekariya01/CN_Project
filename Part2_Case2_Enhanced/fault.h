#ifndef FAULT_H
#define FAULT_H

#include "common_types.h"
#include <stdint.h>
#include <pthread.h>
#include <string>

/*
 * fault.h - Member 3 (PR #4, Wave 2)
 *
 * Implements the Chunk-Boundary Fault Injection Model and topic streaming
 * engine for server worker threads. Evaluates fault drops strictly at the
 * boundary immediately after an OP_FILE_DATA chunk is pushed onto the wire.
 *
 * Also declares client CLI parsing and Part 1 / Case 1 stateless recovery loops.
 */

// Initializes the thread-local MT19937 RNG engine combining random_device, pthread_self, and monotonic time.
void init_thread_rng(uint32_t optional_seed = 0);

// Evaluates whether the connection must be dropped after transmitting an OP_FILE_DATA chunk.
// Returns true if r ~ Uniform(0, 1) < pf; false otherwise.
bool should_inject_fault(double pf, const char* topic, uint32_t file_idx, uint64_t bytes_sent);

// Streams all files for a topic to the client starting from (start_file_idx, start_offset).
// Handles OP_FILE_BEGIN, 64 KiB OP_FILE_DATA chunks, OP_FILE_END, and OP_TRANSFER_END.
// Returns 0 on complete transmission, or -1 if dropped by fault injection or client disconnection.
int stream_topic_to_client(int client_fd, const TopicEntry& entry,
                           uint32_t start_file_idx = 0, uint64_t start_offset = 0,
                           double pf = 0.0, ServerSession* session_ptr = nullptr,
                           pthread_mutex_t* session_mutex = nullptr);

// Client CLI parser and recovery loop functions (Member 3)
int parse_client_cli(int argc, char* argv[], std::string* out_ip, uint16_t* out_port,
                     std::string* out_topic, std::string* out_dir, int* out_max_retries,
                     uint32_t* out_ckpt_win, int* out_streams);

int run_client_part1_and_case1_loop(const char* server_ip, uint16_t port,
                                    const char* topic, const char* output_dir,
                                    int max_retries = DEFAULT_MAX_RETRIES,
                                    bool is_part1 = true);

#endif // FAULT_H
