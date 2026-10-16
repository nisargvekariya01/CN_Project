#include "session.h"
#include "directory_mapper.h"
#include "protocol.h"
#include "metrics_logger.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <strings.h>
#include <random>

// Weak hook for directory_mapper's get_topic_path so client binary links cleanly
extern const TopicEntry* get_topic_path(const std::unordered_map<std::string, TopicEntry>& topic_map,
                                        const std::string& topic) __attribute__((weak));

static const TopicEntry* resolve_topic(const std::unordered_map<std::string, TopicEntry>& topic_map,
                                       const std::string& topic) {
    if (get_topic_path) {
        return get_topic_path(topic_map, topic);
    }
    auto it = topic_map.find(topic);
    if (it != topic_map.end()) {
        return &(it->second);
    }
    for (const auto& pair : topic_map) {
        if (strcasecmp(pair.first.c_str(), topic.c_str()) == 0) {
            return &(pair.second);
        }
    }
    return nullptr;
}

/*
 * session.cpp - Member 4 (PR #5, Wave 3)
 *
 * Implements:
 *   1. 128-bit cryptographically secure session ID generation and table management
 *   2. 600s idle TTL sweep
 *   3. Strict 7-step boundary-aware checkpoint validation (Section 4.3 of Design Doc)
 */

static std::unordered_map<std::string, ServerSession> g_session_table;
static pthread_mutex_t g_session_table_mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t* get_session_table_mutex() {
    return &g_session_table_mutex;
}

static void sweep_expired_sessions_locked(time_t now) {
    for (auto it = g_session_table.begin(); it != g_session_table.end(); ) {
        if (difftime(now, it->second.last_active) > SESSION_TTL_SEC) {
            log_event("INFO", "Swept expired session '%s' (idle > %ds)",
                      it->first.c_str(), SESSION_TTL_SEC);
            it = g_session_table.erase(it);
        } else {
            ++it;
        }
    }
}

void sweep_expired_sessions(time_t now) {
    if (now == 0) {
        now = time(nullptr);
    }
    pthread_mutex_lock(&g_session_table_mutex);
    sweep_expired_sessions_locked(now);
    pthread_mutex_unlock(&g_session_table_mutex);
}

int create_session(const char* topic, uint8_t out_session_id[16], ServerSession** out_session_ptr) {
    if (!topic || !out_session_id) {
        return -1;
    }

    bool got_random = false;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, out_session_id, 16);
        close(fd);
        if (n == 16) {
            got_random = true;
        }
    }

    if (!got_random) {
        // Fallback to random_device
        std::random_device rd;
        for (int i = 0; i < 16; ++i) {
            out_session_id[i] = static_cast<uint8_t>(rd() & 0xFF);
        }
    }

    char hex_buf[33];
    session_id_to_hex(out_session_id, hex_buf);

    pthread_mutex_lock(&g_session_table_mutex);
    time_t now = time(nullptr);
    sweep_expired_sessions_locked(now);

    ServerSession sess;
    std::memcpy(sess.session_id, out_session_id, 16);
    std::strncpy(sess.session_id_hex, hex_buf, 33);
    std::strncpy(sess.topic, topic, sizeof(sess.topic) - 1);
    sess.topic[sizeof(sess.topic) - 1] = '\0';
    sess.file_index = 0;
    sess.bytes_sent_highwater = 0;
    sess.current_file_sent = 0;
    sess.last_active = now;
    sess.all_files_sent = false;

    auto res = g_session_table.insert_or_assign(std::string(hex_buf), sess);
    if (out_session_ptr) {
        *out_session_ptr = &(res.first->second);
    }
    pthread_mutex_unlock(&g_session_table_mutex);

    log_event("INFO", "Created new server session '%s' for topic '%s'", hex_buf, topic);
    return 0;
}

int validate_checkpoint(const CheckpointRecord* client_ckpt,
                        const std::unordered_map<std::string, TopicEntry>& topic_map,
                        uint16_t* out_err_code,
                        std::string* out_reason,
                        uint32_t* out_rem_files,
                        uint64_t* out_rem_bytes,
                        ServerSession** out_session_ptr) {
    if (!client_ckpt || !out_err_code || !out_reason) {
        return -1;
    }

    char hex_buf[33];
    session_id_to_hex(client_ckpt->session_id, hex_buf);

    pthread_mutex_lock(&g_session_table_mutex);
    time_t now = time(nullptr);

    // 1. Check 1: 128-bit session_id exists in active session table
    auto it = g_session_table.find(hex_buf);
    if (it == g_session_table.end()) {
        *out_err_code = ERR_SESSION_EXPIRED;
        *out_reason = "Session expired or not found";
        pthread_mutex_unlock(&g_session_table_mutex);
        log_event("INFO", "Validation Check 1 FAIL: Session '%s' not found", hex_buf);
        return -1;
    }

    // 2. Check 2: Session has not exceeded 600-second idle TTL
    if (difftime(now, it->second.last_active) > SESSION_TTL_SEC) {
        g_session_table.erase(it);
        *out_err_code = ERR_SESSION_EXPIRED;
        *out_reason = "Session idle TTL exceeded 600 seconds";
        pthread_mutex_unlock(&g_session_table_mutex);
        log_event("INFO", "Validation Check 2 FAIL: Session '%s' idle TTL expired", hex_buf);
        return -1;
    }

    // Run lazy TTL sweep on remaining table entries
    sweep_expired_sessions_locked(now);

    ServerSession& sess = it->second;

    // 3. Check 3: Requested topic matches session's recorded topic
    if (std::strcmp(client_ckpt->topic, sess.topic) != 0) {
        *out_err_code = ERR_TOPIC_MISMATCH;
        *out_reason = "Topic mismatch: checkpoint requested '" + std::string(client_ckpt->topic) +
                      "' but session bound to '" + std::string(sess.topic) + "'";
        pthread_mutex_unlock(&g_session_table_mutex);
        log_event("INFO", "Validation Check 3 FAIL: %s", out_reason->c_str());
        return -1;
    }

    const TopicEntry* entry = resolve_topic(topic_map, client_ckpt->topic);
    if (!entry) {
        *out_err_code = ERR_TOPIC_NOT_FOUND;
        *out_reason = "Topic not found on server: " + std::string(client_ckpt->topic);
        pthread_mutex_unlock(&g_session_table_mutex);
        log_event("INFO", "Validation Check 3 FAIL: Topic '%s' not in server topic map", client_ckpt->topic);
        return -1;
    }

    uint32_t file_count = static_cast<uint32_t>(entry->files.size());

    // 4. Check 4: Index Range Check (0 <= file_index <= file_count)
    if (client_ckpt->file_index == file_count) {
        // Terminal case: valid iff byte_offset == 0, file_name == "EOF", and all files were sent
        bool name_is_eof = (std::strcmp(client_ckpt->file_name, "EOF") == 0 || client_ckpt->file_name[0] == '\0');
        if (client_ckpt->byte_offset == 0 && name_is_eof && sess.all_files_sent) {
            sess.last_active = now;
            if (out_rem_files) *out_rem_files = 0;
            if (out_rem_bytes) *out_rem_bytes = 0;
            if (out_session_ptr) *out_session_ptr = &sess;
            pthread_mutex_unlock(&g_session_table_mutex);
            log_event("INFO", "Validation Check 4 PASS: Terminal EOF session '%s' verified", hex_buf);
            return 0;
        } else {
            *out_err_code = ERR_INVALID_OFFSET;
            *out_reason = "Terminal index file_index == file_count invalid: offset != 0 or filename != EOF or all_files_sent false";
            pthread_mutex_unlock(&g_session_table_mutex);
            log_event("INFO", "Validation Check 4 FAIL: %s", out_reason->c_str());
            return -1;
        }
    } else if (client_ckpt->file_index > file_count) {
        *out_err_code = ERR_INVALID_OFFSET;
        *out_reason = "file_index " + std::to_string(client_ckpt->file_index) +
                      " exceeds topic file count " + std::to_string(file_count);
        pthread_mutex_unlock(&g_session_table_mutex);
        log_event("INFO", "Validation Check 4 FAIL: %s", out_reason->c_str());
        return -1;
    }

    // Active File Case (file_index < file_count)
    const FileMeta& target_file = entry->files[client_ckpt->file_index];

    // 5. Check 5: Server's filename at file_index matches client's checkpoint file_name
    if (client_ckpt->file_name[0] != '\0' && target_file.filename != client_ckpt->file_name) {
        *out_err_code = ERR_FILENAME_MISMATCH;
        *out_reason = "Filename mismatch at index " + std::to_string(client_ckpt->file_index) +
                      ": expected '" + target_file.filename + "' but got '" + std::string(client_ckpt->file_name) + "'";
        pthread_mutex_unlock(&g_session_table_mutex);
        log_event("INFO", "Validation Check 5 FAIL: %s", out_reason->c_str());
        return -1;
    }

    // 6. Check 6: 0 <= byte_offset <= file_size of that file
    if (client_ckpt->byte_offset > target_file.file_size) {
        *out_err_code = ERR_INVALID_OFFSET;
        *out_reason = "byte_offset " + std::to_string(client_ckpt->byte_offset) +
                      " exceeds file size " + std::to_string(target_file.file_size);
        pthread_mutex_unlock(&g_session_table_mutex);
        log_event("INFO", "Validation Check 6 FAIL: %s", out_reason->c_str());
        return -1;
    }

    // 7. Check 7: Cumulative bytes claimed <= bytes_sent_highwater
    uint64_t cumulative_claimed = 0;
    for (uint32_t i = 0; i < client_ckpt->file_index; ++i) {
        cumulative_claimed += entry->files[i].file_size;
    }
    cumulative_claimed += client_ckpt->byte_offset;

    if (cumulative_claimed > sess.bytes_sent_highwater) {
        *out_err_code = ERR_HIGHWATER_EXCEED;
        *out_reason = "Cumulative claimed bytes " + std::to_string(cumulative_claimed) +
                      " exceeds server highwater " + std::to_string(sess.bytes_sent_highwater);
        pthread_mutex_unlock(&g_session_table_mutex);
        log_event("INFO", "Validation Check 7 FAIL: claimed %llu > highwater %llu",
                  static_cast<unsigned long long>(cumulative_claimed),
                  static_cast<unsigned long long>(sess.bytes_sent_highwater));
        return -1;
    }

    // All 7 Checks Passed!
    sess.last_active = now;
    sess.file_index = client_ckpt->file_index;

    if (out_rem_files) {
        *out_rem_files = file_count - client_ckpt->file_index;
    }

    uint64_t rem_b = target_file.file_size - client_ckpt->byte_offset;
    for (size_t i = client_ckpt->file_index + 1; i < file_count; ++i) {
        rem_b += entry->files[i].file_size;
    }
    if (out_rem_bytes) {
        *out_rem_bytes = rem_b;
    }

    if (out_session_ptr) {
        *out_session_ptr = &sess;
    }

    pthread_mutex_unlock(&g_session_table_mutex);

    log_event("INFO", "Validation PASSED: session '%s' resuming at file_index %u offset %llu (rem_files=%u, rem_bytes=%llu)",
              hex_buf, client_ckpt->file_index,
              static_cast<unsigned long long>(client_ckpt->byte_offset),
              file_count - client_ckpt->file_index,
              static_cast<unsigned long long>(rem_b));
    return 0;
}

ServerSession* find_session(const uint8_t session_id[16]) {
    char hex_buf[33];
    session_id_to_hex(session_id, hex_buf);
    return find_session_hex(hex_buf);
}

ServerSession* find_session_hex(const char* hex32) {
    if (!hex32) return nullptr;
    pthread_mutex_lock(&g_session_table_mutex);
    auto it = g_session_table.find(hex32);
    ServerSession* ptr = (it != g_session_table.end()) ? &(it->second) : nullptr;
    pthread_mutex_unlock(&g_session_table_mutex);
    return ptr;
}

void clear_all_sessions() {
    pthread_mutex_lock(&g_session_table_mutex);
    g_session_table.clear();
    pthread_mutex_unlock(&g_session_table_mutex);
}

size_t get_active_session_count() {
    pthread_mutex_lock(&g_session_table_mutex);
    size_t sz = g_session_table.size();
    pthread_mutex_unlock(&g_session_table_mutex);
    return sz;
}
