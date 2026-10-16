#include "fault.h"
#include "protocol.h"
#include "metrics_logger.h"

#include <random>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <algorithm>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * fault.cpp - Member 3 (PR #4, Wave 2)
 *
 * Implements:
 *   1. Thread-local MT19937 random generator initialization
 *   2. Chunk-Boundary Fault Injection Model
 *   3. stream_topic_to_client streaming pipeline
 */

static thread_local std::mt19937 g_rng_engine;
static thread_local bool g_rng_initialized = false;

void init_thread_rng(uint32_t optional_seed) {
    if (g_rng_initialized) {
        return;
    }
    if (optional_seed != 0) {
        uint32_t tid = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pthread_self()));
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        g_rng_engine.seed(optional_seed ^ (tid * 2654435761U + 1) ^ static_cast<uint32_t>(ts.tv_nsec));
    } else {
        std::random_device rd;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint32_t tid = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pthread_self()));
        uint32_t s = rd() ^ (tid * 2654435761U) ^ static_cast<uint32_t>(ts.tv_nsec);
        g_rng_engine.seed(s);
    }
    g_rng_initialized = true;
}

bool should_inject_fault(double pf, const char* topic, uint32_t file_idx, uint64_t bytes_sent) {
    if (pf <= 0.0) {
        return false;
    }
    if (pf >= 1.0) {
        log_event("FAULT", "Dropped connection on topic '%s' file_index %u at %llu bytes (pf=%.3f, r=1.0000)",
                  topic ? topic : "unknown", file_idx, static_cast<unsigned long long>(bytes_sent), pf);
        return true;
    }

    if (!g_rng_initialized) {
        init_thread_rng(0);
    }

    std::uniform_real_distribution<double> dist(0.0, 1.0);
    double r = dist(g_rng_engine);

    if (r < pf) {
        log_event("FAULT", "Dropped connection on topic '%s' file_index %u at %llu bytes (pf=%.3f, r=%.4f)",
                  topic ? topic : "unknown", file_idx, static_cast<unsigned long long>(bytes_sent), pf, r);
        return true;
    }
    return false;
}

int stream_topic_to_client(int client_fd, const TopicEntry& entry,
                           uint32_t start_file_idx, uint64_t start_offset,
                           double pf, ServerSession* session_ptr,
                           pthread_mutex_t* session_mutex) {
    uint64_t wire_counter = 0;

    if (!g_rng_initialized) {
        init_thread_rng(0);
    }

    for (size_t i = start_file_idx; i < entry.files.size(); ++i) {
        const FileMeta& file = entry.files[i];
        FILE* fp = fopen(file.full_path.c_str(), "rb");
        if (!fp) {
            log_event("ERROR", "Failed to open file '%s': %s", file.full_path.c_str(), strerror(errno));
            send_error(client_fd, ERR_IO_FAILURE, "Failed to open file on server", &wire_counter);
            return -1;
        }

        uint64_t curr_offset = (i == start_file_idx) ? start_offset : 0;
        if (curr_offset > 0) {
            if (fseeko(fp, static_cast<off_t>(curr_offset), SEEK_SET) != 0) {
                log_event("ERROR", "fseeko to %llu failed on file '%s': %s",
                          static_cast<unsigned long long>(curr_offset), file.full_path.c_str(), strerror(errno));
                fclose(fp);
                send_error(client_fd, ERR_IO_FAILURE, "Failed to seek file on server", &wire_counter);
                return -1;
            }
        }

        uint64_t bytes_following = (file.file_size >= curr_offset) ? (file.file_size - curr_offset) : 0;

        // 1. Send OP_FILE_BEGIN
        if (send_file_begin(client_fd, static_cast<uint32_t>(i), curr_offset,
                            bytes_following, file.filename, &wire_counter) != 0) {
            log_event("ERROR", "Failed to send OP_FILE_BEGIN for file '%s' (fd=%d)",
                      file.filename.c_str(), client_fd);
            fclose(fp);
            return -1;
        }

        uint8_t chunk_buf[CHUNK_SIZE];
        uint64_t file_offset = curr_offset;

        while (file_offset < file.file_size) {
            size_t to_read = static_cast<size_t>(std::min<uint64_t>(CHUNK_SIZE, file.file_size - file_offset));
            size_t nread = fread(chunk_buf, 1, to_read, fp);
            if (nread == 0) {
                if (ferror(fp)) {
                    log_event("ERROR", "fread error on file '%s': %s", file.full_path.c_str(), strerror(errno));
                    fclose(fp);
                    send_error(client_fd, ERR_IO_FAILURE, "Read error on server", &wire_counter);
                    return -1;
                }
                break; // EOF
            }

            // 2. Send OP_FILE_DATA
            if (send_file_data(client_fd, static_cast<uint32_t>(i), file_offset,
                               chunk_buf, static_cast<uint32_t>(nread), &wire_counter) != 0) {
                log_event("ERROR", "Failed to send OP_FILE_DATA on file '%s' at offset %llu (fd=%d)",
                          file.filename.c_str(), static_cast<unsigned long long>(file_offset), client_fd);
                fclose(fp);
                return -1;
            }

            file_offset += nread;

            // Session tracking if provided (Member 4 integration hook)
            if (session_ptr != nullptr) {
                if (session_mutex) pthread_mutex_lock(session_mutex);
                session_ptr->file_index = static_cast<uint32_t>(i);
                session_ptr->bytes_sent_highwater += nread;
                session_ptr->last_active = time(nullptr);
                if (session_mutex) pthread_mutex_unlock(session_mutex);
            }

            // 3. Strict Chunk-Boundary Fault Injection Check
            if (should_inject_fault(pf, entry.topic_name.c_str(), static_cast<uint32_t>(i), file_offset)) {
                fclose(fp);
                return -1;
            }
        }

        fclose(fp);

        // 4. Send OP_FILE_END
        if (send_file_end(client_fd, static_cast<uint32_t>(i), &wire_counter) != 0) {
            log_event("ERROR", "Failed to send OP_FILE_END for file '%s' (fd=%d)",
                      file.filename.c_str(), client_fd);
            return -1;
        }
    }

    if (session_ptr != nullptr) {
        if (session_mutex) pthread_mutex_lock(session_mutex);
        session_ptr->all_files_sent = true;
        session_ptr->last_active = time(nullptr);
        if (session_mutex) pthread_mutex_unlock(session_mutex);
    }

    // 5. Conclude topic with OP_TRANSFER_END
    if (send_transfer_end(client_fd, static_cast<uint32_t>(entry.files.size()),
                          entry.total_bytes, &wire_counter) != 0) {
        log_event("ERROR", "Failed to send OP_TRANSFER_END (fd=%d)", client_fd);
        return -1;
    }

    log_event("INFO", "Completed streaming topic '%s' (%zu files, %llu bytes)",
              entry.topic_name.c_str(), entry.files.size(),
              static_cast<unsigned long long>(entry.total_bytes));
    return 0;
}
