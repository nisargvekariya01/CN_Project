#include "session.h"
#include "checkpoint.h"
#include "protocol.h"
#include "metrics_logger.h"
#include "directory_mapper.h"
#include "common_types.h"

#include <iostream>
#include <vector>
#include <cstring>
#include <cassert>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <thread>

/*
 * test_session_ckpt.cpp - Comprehensive Unit Tests for Member 4 (PR #5, Wave 3)
 *
 * Verifies:
 *   1. 128-bit Session ID generation, uniqueness, and active table management
 *   2. Lazy TTL sweep of expired sessions (SESSION_TTL_SEC = 600s)
 *   3. Strict 7-step boundary-aware resume validation:
 *      - Check 1: Session existence in table (410 ERR_SESSION_EXPIRED)
 *      - Check 2: Idle TTL limit (410 ERR_SESSION_EXPIRED)
 *      - Check 3: Topic mismatch (409 ERR_TOPIC_MISMATCH)
 *      - Check 4a: Terminal case (file_index == file_count, byte_offset == 0, file_name == "EOF")
 *      - Check 4b: file_index > file_count (416 ERR_INVALID_OFFSET)
 *      - Check 5: Filename mismatch at file_index (409 ERR_FILENAME_MISMATCH)
 *      - Check 6: byte_offset > file_size (416 ERR_INVALID_OFFSET)
 *      - Check 7: Cumulative bytes > bytes_sent_highwater (416 ERR_HIGHWATER_EXCEED)
 *   4. Valid resume remaining file and byte calculations
 *   5. Atomic checkpoint save (fsync payload -> fsync tmp -> atomic rename)
 *   6. Checkpoint load and ASCII format parser round-trip
 *   7. Checkpoint cleanup (unlink .ckpt and .ckpt.tmp)
 *   8. open_and_seek_resume_file non-destructive seek and overwrite behavior
 *   9. Multi-threaded concurrency safety of session table operations
 */

static int g_tests_passed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[-] ASSERTION FAILED: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            assert(false); \
        } \
    } while(0)

#define PASS_TEST(name) \
    do { \
        g_tests_passed++; \
        std::cout << "  [PASS] Test " << g_tests_passed << ": " << name << "\n"; \
    } while(0)

static void test_session_creation_and_uniqueness() {
    clear_all_sessions();
    TEST_ASSERT(get_active_session_count() == 0, "Initial session table must be empty");

    uint8_t id1[16];
    ServerSession* sess1 = nullptr;
    int rc1 = create_session("Cane", id1, &sess1);
    TEST_ASSERT(rc1 == 0, "create_session must return 0");
    TEST_ASSERT(sess1 != nullptr, "session pointer must not be null");

    // Check non-zero ID
    bool non_zero = false;
    for (int i = 0; i < 16; ++i) {
        if (id1[i] != 0) non_zero = true;
    }
    TEST_ASSERT(non_zero, "128-bit session ID must not be all zeros");

    TEST_ASSERT(std::strcmp(sess1->topic, "Cane") == 0, "Session topic must be Cane");
    TEST_ASSERT(sess1->file_index == 0, "Initial file_index must be 0");
    TEST_ASSERT(sess1->bytes_sent_highwater == 0, "Initial bytes_sent_highwater must be 0");
    TEST_ASSERT(!sess1->all_files_sent, "all_files_sent must be false initially");
    TEST_ASSERT(get_active_session_count() == 1, "Session count must be 1");

    uint8_t id2[16];
    ServerSession* sess2 = nullptr;
    int rc2 = create_session("Cane", id2, &sess2);
    TEST_ASSERT(rc2 == 0, "Second create_session must return 0");
    TEST_ASSERT(std::memcmp(id1, id2, 16) != 0, "Two generated session IDs must be unique");
    TEST_ASSERT(get_active_session_count() == 2, "Session count must be 2");

    PASS_TEST("Session Creation & 128-Bit Uniqueness");
}

static void test_session_idle_ttl_sweep() {
    clear_all_sessions();
    uint8_t id_expired[16];
    ServerSession* sess_exp = nullptr;
    create_session("Cavallo", id_expired, &sess_exp);

    uint8_t id_active[16];
    ServerSession* sess_act = nullptr;
    create_session("Cavallo", id_active, &sess_act);

    TEST_ASSERT(get_active_session_count() == 2, "Must have 2 sessions initially");

    time_t now = time(nullptr);
    sess_exp->last_active = now - 605; // 605 seconds idle (> 600s TTL)
    sess_act->last_active = now - 50;  // Active (50s idle)

    sweep_expired_sessions(now);

    TEST_ASSERT(get_active_session_count() == 1, "Expired session must be pruned by TTL sweep");
    TEST_ASSERT(find_session(id_expired) == nullptr, "Expired session must not be found");
    TEST_ASSERT(find_session(id_active) != nullptr, "Active session must still exist");

    PASS_TEST("Lazy 600s Idle TTL Session Pruning");
}

static void test_seven_step_validation() {
    clear_all_sessions();

    // Construct mock topic entry
    std::unordered_map<std::string, TopicEntry> topic_map;
    TopicEntry te;
    te.topic_name = "Cane";
    te.dir_path = "/mock/dataset/Cane";

    FileMeta f0{"eb1001.jpg", "/mock/dataset/Cane/eb1001.jpg", 100000};
    FileMeta f1{"eb1002.jpg", "/mock/dataset/Cane/eb1002.jpg", 200000};
    FileMeta f2{"eb1003.jpg", "/mock/dataset/Cane/eb1003.jpg", 300000};

    te.files.push_back(f0);
    te.files.push_back(f1);
    te.files.push_back(f2);
    te.total_bytes = 600000;
    topic_map["Cane"] = te;

    // Create session for "Cane" with highwater = 350,000 (File 0 + File 1 + 50,000 of File 2)
    uint8_t sess_id[16];
    ServerSession* server_sess = nullptr;
    create_session("Cane", sess_id, &server_sess);
    server_sess->bytes_sent_highwater = 350000;
    server_sess->file_index = 2;

    uint16_t err_code = 0;
    std::string reason;
    uint32_t rem_files = 0;
    uint64_t rem_bytes = 0;
    ServerSession* out_sess = nullptr;

    // -------------------------------------------------------------------------
    // Check 1: Session not in table -> 410 ERR_SESSION_EXPIRED
    // -------------------------------------------------------------------------
    CheckpointRecord ckpt1;
    std::memset(&ckpt1, 0, sizeof(ckpt1));
    std::memset(ckpt1.session_id, 0xEE, 16); // Unknown session
    std::strncpy(ckpt1.topic, "Cane", sizeof(ckpt1.topic) - 1);
    ckpt1.file_index = 0;
    ckpt1.byte_offset = 0;
    std::strncpy(ckpt1.file_name, "eb1001.jpg", sizeof(ckpt1.file_name) - 1);

    int rc = validate_checkpoint(&ckpt1, topic_map, &err_code, &reason, &rem_files, &rem_bytes, &out_sess);
    TEST_ASSERT(rc == -1, "Check 1 must return -1");
    TEST_ASSERT(err_code == ERR_SESSION_EXPIRED, "Check 1 error code must be 410 ERR_SESSION_EXPIRED");
    PASS_TEST("Check 1: Unknown Session Rejection (410 ERR_SESSION_EXPIRED)");

    // -------------------------------------------------------------------------
    // Check 2: Session idle TTL exceeded (> 600s) -> 410 ERR_SESSION_EXPIRED
    // -------------------------------------------------------------------------
    server_sess->last_active = time(nullptr) - 601; // Expired
    std::memcpy(ckpt1.session_id, sess_id, 16);

    rc = validate_checkpoint(&ckpt1, topic_map, &err_code, &reason, &rem_files, &rem_bytes, &out_sess);
    TEST_ASSERT(rc == -1, "Check 2 must return -1");
    TEST_ASSERT(err_code == ERR_SESSION_EXPIRED, "Check 2 error code must be 410 ERR_SESSION_EXPIRED");
    TEST_ASSERT(find_session(sess_id) == nullptr, "Expired session must be erased upon Check 2 fail");
    PASS_TEST("Check 2: Idle TTL Expiration (410 ERR_SESSION_EXPIRED)");

    // Recreate fresh session for remaining checks
    create_session("Cane", sess_id, &server_sess);
    server_sess->bytes_sent_highwater = 350000;
    server_sess->file_index = 2;
    std::memcpy(ckpt1.session_id, sess_id, 16);

    // -------------------------------------------------------------------------
    // Check 3: Topic mismatch -> 409 ERR_TOPIC_MISMATCH
    // -------------------------------------------------------------------------
    std::strncpy(ckpt1.topic, "Elefante", sizeof(ckpt1.topic) - 1); // Wrong topic
    rc = validate_checkpoint(&ckpt1, topic_map, &err_code, &reason, &rem_files, &rem_bytes, &out_sess);
    TEST_ASSERT(rc == -1, "Check 3 must return -1");
    TEST_ASSERT(err_code == ERR_TOPIC_MISMATCH, "Check 3 error code must be 409 ERR_TOPIC_MISMATCH");
    PASS_TEST("Check 3: Topic Mismatch Rejection (409 ERR_TOPIC_MISMATCH)");

    // Restore valid topic
    std::strncpy(ckpt1.topic, "Cane", sizeof(ckpt1.topic) - 1);

    // -------------------------------------------------------------------------
    // Check 4a: Active Index Out of Bounds (file_index > file_count) -> 416
    // -------------------------------------------------------------------------
    ckpt1.file_index = 5; // topic only has 3 files (0, 1, 2)
    rc = validate_checkpoint(&ckpt1, topic_map, &err_code, &reason, &rem_files, &rem_bytes, &out_sess);
    TEST_ASSERT(rc == -1, "Check 4 active index out of bounds must return -1");
    TEST_ASSERT(err_code == ERR_INVALID_OFFSET, "Check 4 error code must be 416 ERR_INVALID_OFFSET");
    PASS_TEST("Check 4a: Out of Bounds Index (416 ERR_INVALID_OFFSET)");

    // -------------------------------------------------------------------------
    // Check 4b: Terminal Case (file_index == file_count)
    // -------------------------------------------------------------------------
    ckpt1.file_index = 3; // file_count == 3
    ckpt1.byte_offset = 500; // Invalid terminal offset
    std::strncpy(ckpt1.file_name, "EOF", sizeof(ckpt1.file_name) - 1);
    server_sess->all_files_sent = true;

    rc = validate_checkpoint(&ckpt1, topic_map, &err_code, &reason, &rem_files, &rem_bytes, &out_sess);
    TEST_ASSERT(rc == -1, "Terminal case with non-zero offset must fail");
    TEST_ASSERT(err_code == ERR_INVALID_OFFSET, "Terminal case non-zero offset must be 416");

    // Invalid terminal filename
    ckpt1.byte_offset = 0;
    std::strncpy(ckpt1.file_name, "eb1003.jpg", sizeof(ckpt1.file_name) - 1);
    rc = validate_checkpoint(&ckpt1, topic_map, &err_code, &reason, &rem_files, &rem_bytes, &out_sess);
    TEST_ASSERT(rc == -1, "Terminal case without EOF filename must fail");

    // Invalid terminal all_files_sent flag
    std::strncpy(ckpt1.file_name, "EOF", sizeof(ckpt1.file_name) - 1);
    server_sess->all_files_sent = false;
    rc = validate_checkpoint(&ckpt1, topic_map, &err_code, &reason, &rem_files, &rem_bytes, &out_sess);
    TEST_ASSERT(rc == -1, "Terminal case with all_files_sent=false must fail");

    // Valid Terminal EOF State
    server_sess->all_files_sent = true;
    rc = validate_checkpoint(&ckpt1, topic_map, &err_code, &reason, &rem_files, &rem_bytes, &out_sess);
    TEST_ASSERT(rc == 0, "Valid Terminal EOF state must return 0");
    TEST_ASSERT(rem_files == 0, "rem_files must be 0 for terminal state");
    TEST_ASSERT(rem_bytes == 0, "rem_bytes must be 0 for terminal state");
    PASS_TEST("Check 4b: Terminal EOF State Validation (rem_files=0, rem_bytes=0)");

    // -------------------------------------------------------------------------
    // Check 5: Filename Mismatch at file_index -> 409 ERR_FILENAME_MISMATCH
    // -------------------------------------------------------------------------
    ckpt1.file_index = 1; // Server file 1 is "eb1002.jpg"
    ckpt1.byte_offset = 0;
    std::strncpy(ckpt1.file_name, "wrong_name.jpg", sizeof(ckpt1.file_name) - 1);
    rc = validate_checkpoint(&ckpt1, topic_map, &err_code, &reason, &rem_files, &rem_bytes, &out_sess);
    TEST_ASSERT(rc == -1, "Check 5 filename mismatch must return -1");
    TEST_ASSERT(err_code == ERR_FILENAME_MISMATCH, "Check 5 error code must be 409 ERR_FILENAME_MISMATCH");
    PASS_TEST("Check 5: Filename Mismatch Rejection (409 ERR_FILENAME_MISMATCH)");

    // Restore correct filename
    std::strncpy(ckpt1.file_name, "eb1002.jpg", sizeof(ckpt1.file_name) - 1);

    // -------------------------------------------------------------------------
    // Check 6: Byte Offset Exceeds File Size -> 416 ERR_INVALID_OFFSET
    // -------------------------------------------------------------------------
    ckpt1.byte_offset = 200001; // file 1 size is exactly 200000
    rc = validate_checkpoint(&ckpt1, topic_map, &err_code, &reason, &rem_files, &rem_bytes, &out_sess);
    TEST_ASSERT(rc == -1, "Check 6 byte offset > file_size must return -1");
    TEST_ASSERT(err_code == ERR_INVALID_OFFSET, "Check 6 error code must be 416 ERR_INVALID_OFFSET");
    PASS_TEST("Check 6: Byte Offset Exceeds File Size (416 ERR_INVALID_OFFSET)");

    // -------------------------------------------------------------------------
    // Check 7: Cumulative Claimed Exceeds Server Highwater -> 416 ERR_HIGHWATER_EXCEED
    // -------------------------------------------------------------------------
    // Server highwater is 350,000.
    // If client claims file_index = 2, byte_offset = 60,000:
    // Cumulative claimed = file0 (100,000) + file1 (200,000) + 60,000 = 360,000 > 350,000!
    ckpt1.file_index = 2;
    std::strncpy(ckpt1.file_name, "eb1003.jpg", sizeof(ckpt1.file_name) - 1);
    ckpt1.byte_offset = 60000;
    rc = validate_checkpoint(&ckpt1, topic_map, &err_code, &reason, &rem_files, &rem_bytes, &out_sess);
    TEST_ASSERT(rc == -1, "Check 7 claimed > highwater must return -1");
    TEST_ASSERT(err_code == ERR_HIGHWATER_EXCEED, "Check 7 error code must be 416 ERR_HIGHWATER_EXCEED");
    PASS_TEST("Check 7: Highwater Exceeded Rejection (416 ERR_HIGHWATER_EXCEED)");

    // -------------------------------------------------------------------------
    // Valid Resume: Active file mid-transfer
    // -------------------------------------------------------------------------
    // file_index = 1, byte_offset = 50,000.
    // Cumulative claimed = 100,000 + 50,000 = 150,000 <= 350,000 (Passes Check 7!)
    ckpt1.file_index = 1;
    std::strncpy(ckpt1.file_name, "eb1002.jpg", sizeof(ckpt1.file_name) - 1);
    ckpt1.byte_offset = 50000;

    rc = validate_checkpoint(&ckpt1, topic_map, &err_code, &reason, &rem_files, &rem_bytes, &out_sess);
    TEST_ASSERT(rc == 0, "Valid checkpoint must return 0");
    TEST_ASSERT(rem_files == 2, "rem_files must be 2 (file 1 and file 2)");
    // Remaining bytes: (200,000 - 50,000) for file 1 + 300,000 for file 2 = 450,000 bytes
    TEST_ASSERT(rem_bytes == 450000, "rem_bytes must be 450,000");
    TEST_ASSERT(out_sess != nullptr, "out_session pointer must be returned");
    TEST_ASSERT(out_sess->file_index == 1, "session file_index must be updated to 1");
    PASS_TEST("Valid Mid-Topic Resume: rem_files=2, rem_bytes=450,000");
}

static void test_checkpoint_durability_and_format() {
    std::string test_dir = "test_ckpt_dir";
    mkdir(test_dir.c_str(), 0755);

    CheckpointRecord ckpt;
    std::memset(&ckpt, 0, sizeof(ckpt));
    for (int i = 0; i < 16; ++i) {
        ckpt.session_id[i] = static_cast<uint8_t>(i * 17);
    }
    session_id_to_hex(ckpt.session_id, ckpt.session_id_hex);
    std::strncpy(ckpt.topic, "Farfalla", sizeof(ckpt.topic) - 1);
    ckpt.file_index = 7;
    ckpt.byte_offset = 131072;
    ckpt.files_done = 7;
    std::strncpy(ckpt.file_name, "butterfly_007.png", sizeof(ckpt.file_name) - 1);

    // Create a mock active file descriptor
    std::string mock_file = test_dir + "/butterfly_007.png";
    int mock_fd = open(mock_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(mock_fd >= 0, "Mock file open must succeed");
    const char dummy[] = "DURABLE_PAYLOAD_CHUNK_DATA";
    ssize_t dw = write(mock_fd, dummy, sizeof(dummy));
    TEST_ASSERT(dw == static_cast<ssize_t>(sizeof(dummy)), "Mock payload write must succeed");

    // Save checkpoint with active file fd (Step a: fsync payload -> Step b: atomic tmp rename)
    int rc = save_checkpoint(test_dir.c_str(), &ckpt, mock_fd, -1);
    TEST_ASSERT(rc == 0, "save_checkpoint must return 0");
    close(mock_fd);

    // Verify final checkpoint exists and tmp file does not remain
    std::string final_ckpt_path = test_dir + "/.it305_Farfalla.ckpt";
    std::string tmp_ckpt_path = test_dir + "/.it305_Farfalla.ckpt.tmp";
    TEST_ASSERT(access(final_ckpt_path.c_str(), F_OK) == 0, "Final .ckpt file must exist");
    TEST_ASSERT(access(tmp_ckpt_path.c_str(), F_OK) != 0, "Temporary .ckpt.tmp file must not remain");

    // Load checkpoint and verify exact bit-for-bit fidelity
    CheckpointRecord loaded;
    std::memset(&loaded, 0, sizeof(loaded));
    bool load_ok = load_checkpoint(test_dir.c_str(), "Farfalla", &loaded);
    TEST_ASSERT(load_ok, "load_checkpoint must succeed");
    TEST_ASSERT(std::memcmp(loaded.session_id, ckpt.session_id, 16) == 0, "session_id binary mismatch");
    TEST_ASSERT(std::strcmp(loaded.session_id_hex, ckpt.session_id_hex) == 0, "session_id_hex mismatch");
    TEST_ASSERT(std::strcmp(loaded.topic, "Farfalla") == 0, "topic mismatch");
    TEST_ASSERT(loaded.file_index == 7, "file_index mismatch");
    TEST_ASSERT(loaded.byte_offset == 131072, "byte_offset mismatch");
    TEST_ASSERT(loaded.files_done == 7, "files_done mismatch");
    TEST_ASSERT(std::strcmp(loaded.file_name, "butterfly_007.png") == 0, "file_name mismatch");
    PASS_TEST("Atomic Checkpoint Save & Load Round-Trip");

    // Checkpoint cleanup test
    remove_checkpoint(test_dir.c_str(), "Farfalla");
    TEST_ASSERT(access(final_ckpt_path.c_str(), F_OK) != 0, "Checkpoint file must be removed");
    bool load_removed = load_checkpoint(test_dir.c_str(), "Farfalla", &loaded);
    TEST_ASSERT(!load_removed, "load_checkpoint on removed file must return false");
    PASS_TEST("Checkpoint Removal & Clean Unlink");

    unlink(mock_file.c_str());
    rmdir(test_dir.c_str());
}

static void test_open_and_seek_resume_file() {
    std::string test_path = "test_seek_file.bin";
    unlink(test_path.c_str());

    // 1. Create a 1000-byte file filled with 'A'
    int fd_init = open(test_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fd_init >= 0, "open fd_init must succeed");
    std::vector<uint8_t> initial_data(1000, 'A');
    ssize_t w = write(fd_init, initial_data.data(), initial_data.size());
    TEST_ASSERT(w == 1000, "Initial write must write 1000 bytes");
    close(fd_init);

    // 2. Open with open_and_seek_resume_file at offset 500
    int resume_fd = open_and_seek_resume_file(test_path.c_str(), 500);
    TEST_ASSERT(resume_fd >= 0, "open_and_seek_resume_file must succeed");

    // Write 200 bytes of 'B'
    std::vector<uint8_t> patch(200, 'B');
    ssize_t wp = write(resume_fd, patch.data(), patch.size());
    TEST_ASSERT(wp == 200, "Patch write must write 200 bytes");
    close(resume_fd);

    // 3. Verify file contents: bytes 0..499 are 'A', bytes 500..699 are 'B', bytes 700..999 are 'A'
    int read_fd = open(test_path.c_str(), O_RDONLY);
    TEST_ASSERT(read_fd >= 0, "open read_fd must succeed");
    std::vector<uint8_t> read_buf(1000);
    ssize_t r = read(read_fd, read_buf.data(), read_buf.size());
    TEST_ASSERT(r == 1000, "Read must return 1000 bytes");
    close(read_fd);

    for (int i = 0; i < 500; ++i) {
        TEST_ASSERT(read_buf[i] == 'A', "Bytes 0..499 must remain unmodified 'A'");
    }
    for (int i = 500; i < 700; ++i) {
        TEST_ASSERT(read_buf[i] == 'B', "Bytes 500..699 must be overwritten 'B'");
    }
    for (int i = 700; i < 1000; ++i) {
        TEST_ASSERT(read_buf[i] == 'A', "Bytes 700..999 must remain 'A'");
    }

    unlink(test_path.c_str());
    PASS_TEST("Safe Non-Destructive In-Place File Resume (NEVER O_TRUNC/O_APPEND)");
}

static void test_concurrent_session_access() {
    clear_all_sessions();
    const int NUM_THREADS = 8;
    const int SESSIONS_PER_THREAD = 20;

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([t]() {
            for (int s = 0; s < SESSIONS_PER_THREAD; ++s) {
                uint8_t id[16];
                ServerSession* sess = nullptr;
                std::string topic = "Topic_" + std::to_string(t);
                create_session(topic.c_str(), id, &sess);
                find_session(id);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    TEST_ASSERT(get_active_session_count() == NUM_THREADS * SESSIONS_PER_THREAD,
                "Concurrent session count must equal total spawned sessions");
    PASS_TEST("Multi-Threaded Session Table Concurrency Safety (Mutex Synchronization)");
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << " IT305 Fault-Tolerant Distribution Service - Member 4 Unit Tests\n";
    std::cout << " Wave 3: Stateful Session Table, 7-Check Validation & Checkpoint\n";
    std::cout << "=================================================================\n";

    test_session_creation_and_uniqueness();
    test_session_idle_ttl_sweep();
    test_seven_step_validation();
    test_checkpoint_durability_and_format();
    test_open_and_seek_resume_file();
    test_concurrent_session_access();

    std::cout << "\n=================================================================\n";
    std::cout << " [ALL " << g_tests_passed << " MEMBER 4 UNIT TESTS PASSED SUCCESSFULLY]\n";
    std::cout << "=================================================================\n";
    return 0;
}
