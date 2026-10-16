#ifndef ENHANCED_PARALLEL_CLIENT_H
#define ENHANCED_PARALLEL_CLIENT_H

#include "common_types.h"
#include <string>
#include <vector>
#include <pthread.h>
#include <stdint.h>

/*
 * enhanced_parallel_client.h - Member 5 (PR #6, Wave 3)
 *
 * Implements:
 *   1. Parallel TCP sub-stream pool (N = 4 streams default, configurable via CLI)
 *   2. Target file pre-allocation via posix_fallocate() / ftruncate()
 *   3. Lock-free concurrent client file assembly via positional pwrite()
 *   4. Sub-stream fault isolation and dynamic bitmask resynchronization
 *   5. Entry point run_enhanced_client() invoked by client.cpp
 */

struct EnhancedClientWorkerArgs {
    std::string server_ip;
    uint16_t port;
    uint32_t conn_id;
    uint8_t session_id[16];
    int max_retries;
    SharedBitmaskState* shared_state;
    std::vector<uint64_t> file_chunk_bases;
    std::vector<uint64_t> file_sizes;
};

// Main entry point for Part 2 Case 2 Enhanced parallel client
int run_enhanced_client(const char* server_ip, uint16_t port,
                        const char* topic, const char* output_dir,
                        int num_streams, int max_retries);

// Worker thread routine for each parallel sub-stream
void* client_parallel_worker(void* arg);

// Pre-allocates a target file on disk using posix_fallocate (or ftruncate fallback)
int preallocate_target_file(const std::string& path, uint64_t file_size);

// Updates the shared bitmask atomically when a chunk is written to disk
bool mark_chunk_committed(SharedBitmaskState* state, uint32_t unit_id, uint32_t chunk_len);

// Checks if all 64 KiB work units across the topic have been committed
bool is_transfer_complete(SharedBitmaskState* state);

#endif // ENHANCED_PARALLEL_CLIENT_H
