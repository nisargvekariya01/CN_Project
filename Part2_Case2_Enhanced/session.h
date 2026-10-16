#ifndef SESSION_H
#define SESSION_H

#include "common_types.h"
#include <unordered_map>
#include <string>
#include <pthread.h>
#include <time.h>

/*
 * session.h - Member 4 (PR #5, Wave 3)
 *
 * Implements 128-bit server session state table, thread-safe session creation,
 * 600s idle TTL lazy sweep, and the strict 7-step boundary-aware checkpoint
 * validation required by Section 4.3 of the Design Document.
 */

// Global session table mutex accessor
pthread_mutex_t* get_session_table_mutex();

// Creates a new 128-bit server session using /dev/urandom and registers it in the table.
int create_session(const char* topic, uint8_t out_session_id[16], ServerSession** out_session_ptr);

// Strict 7-step validation of a client's checkpoint upon OP_RESUME
// Returns 0 on OP_RESUME_OK, -1 on OP_RESTART (with out_err_code and out_reason populated)
int validate_checkpoint(const CheckpointRecord* client_ckpt,
                        const std::unordered_map<std::string, TopicEntry>& topic_map,
                        uint16_t* out_err_code,
                        std::string* out_reason,
                        uint32_t* out_rem_files,
                        uint64_t* out_rem_bytes,
                        ServerSession** out_session_ptr);

// Sweeps sessions that have been idle for longer than SESSION_TTL_SEC (600s)
void sweep_expired_sessions(time_t now = 0);

// Look up an active session by its 16-byte binary ID or 32-char hex string
ServerSession* find_session(const uint8_t session_id[16]);
ServerSession* find_session_hex(const char* hex32);

// Test helper: clears all registered sessions
void clear_all_sessions();

// Test helper: returns number of currently active sessions in table
size_t get_active_session_count();

#endif // SESSION_H
