#ifndef ENHANCED_EPOLL_SERVER_H
#define ENHANCED_EPOLL_SERVER_H

#include "common_types.h"
#include <unordered_map>
#include <string>
#include <vector>
#include <pthread.h>
#include <sys/types.h>

/*
 * enhanced_epoll_server.h - Member 5 (PR #6, Wave 3)
 *
 * Implements:
 *   1. Non-blocking socket configuration and Linux epoll instance initialization
 *   2. Zero-copy sendfile() chunk transmission with build_file_data_header()
 *   3. Dynamic 64 KiB chunk work-queue with 3-state lifecycle (UNASSIGNED -> IN_FLIGHT -> COMMITTED)
 *   4. Sub-stream fault isolation and atomic chunk requeueing
 *   5. Full non-blocking epoll event loop for Case 2 Enhanced
 */

// Sets socket descriptor to non-blocking mode (O_NONBLOCK)
int set_socket_nonblocking(int sockfd);

// Initializes Linux epoll instance and registers listening socket
int init_epoll_instance(int listen_fd);

// Zero-copy chunk transmission directly from page cache to NIC DMA ring via sendfile()
ssize_t send_chunk_zerocopy(int client_fd, int file_fd, const ChunkWorkUnit& unit,
                            uint64_t* wire_counter = nullptr);

// Thread-safe chunk work unit popping from EnhancedSessionState
bool pop_next_chunk_unit(EnhancedSessionState* sess, uint32_t conn_id,
                         ChunkWorkUnit* out_unit);

// Atomically requeues in-flight chunks assigned to a disconnected sub-stream
void requeue_stream_chunks(EnhancedSessionState* sess, int32_t conn_id);

// Marks chunks committed on disk according to client bitmap from OP_JOIN_SESSION
void apply_client_bitmap(EnhancedSessionState* sess, const uint8_t* bitmap, uint32_t bmap_len);

// Creates an enhanced session state with all 64 KiB work units for a topic
EnhancedSessionState* create_enhanced_session(const TopicEntry& entry, uint16_t num_streams,
                                             const uint8_t session_id[16]);

// Looks up an existing enhanced session state by 16-byte session ID
EnhancedSessionState* find_enhanced_session(const uint8_t session_id[16]);

// Cleans up an enhanced session
void free_enhanced_session(const uint8_t session_id[16]);

// Hook called by concurrency.cpp process_client for Enhanced opcodes 0x0C and 0x0D
int process_enhanced_opcodes(int client_fd, const FrameHeader& hdr,
                             const std::vector<uint8_t>& payload,
                             const std::unordered_map<std::string, TopicEntry>& topic_map,
                             double pf, uint64_t* wire_counter);

// Main epoll-driven concurrent server loop for Part 2 Case 2 Enhanced
int run_enhanced_epoll_server_loop(int listen_fd,
                                   const std::unordered_map<std::string, TopicEntry>& topic_map,
                                   double pf, uint32_t seed);

#endif // ENHANCED_EPOLL_SERVER_H
