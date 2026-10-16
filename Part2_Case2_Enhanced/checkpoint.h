#ifndef CHECKPOINT_H
#define CHECKPOINT_H

#include "common_types.h"
#include <string>

/*
 * checkpoint.h - Member 4 (PR #5, Wave 3)
 *
 * Implements client persistent checkpoint management:
 *   1. Atomic checkpoint save with strict 3-step durability order (fsync payload -> fsync tmp -> rename)
 *   2. Checkpoint load and parser from disk ASCII record
 *   3. Checkpoint cleanup on transfer completion or restart
 *   4. Safe file opening with O_WRONLY | O_CREAT (never O_TRUNC or O_APPEND) and seek
 *   5. Case 2 stateful recovery client loop (run_client_case2_loop)
 */

// Saves checkpoint record to <output_dir>/.it305_<topic>.ckpt via atomic tmp rename
// active_file_fd: if >= 0, fsync() is called before the checkpoint file is updated
// sockfd: if >= 0, non-blocking OP_CHECKPOINT_ACK is sent to the server
int save_checkpoint(const char* output_dir, const CheckpointRecord* ckpt,
                    int active_file_fd = -1, int sockfd = -1);

// Loads checkpoint record from <output_dir>/.it305_<topic>.ckpt
// Returns true if a valid checkpoint file exists and was parsed successfully; false otherwise
bool load_checkpoint(const char* output_dir, const char* topic, CheckpointRecord* out_ckpt);

// Removes the checkpoint file <output_dir>/.it305_<topic>.ckpt and any tmp file
void remove_checkpoint(const char* output_dir, const char* topic);

// Opens file with O_WRONLY | O_CREAT, 0644 (never O_TRUNC or O_APPEND) and seeks to accepted_offset
int open_and_seek_resume_file(const char* filepath, uint64_t accepted_offset);

// Client recovery loop for Part 2 Case 2 (Stateful Session & Checkpoint/Resume)
int run_client_case2_loop(const char* server_ip, uint16_t port,
                          const char* topic, const char* output_dir,
                          int max_retries, uint32_t ckpt_window);

#endif // CHECKPOINT_H
