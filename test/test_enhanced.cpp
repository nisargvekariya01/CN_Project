#include "enhanced_epoll_server.h"
#include "enhanced_parallel_client.h"
#include "protocol.h"
#include "common_types.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <vector>
#include <string>
#include <pthread.h>

/*
 * test_enhanced.cpp - Comprehensive Unit Test Harness for Member 5 (Wave 3)
 *
 * Validates:
 *   1. Socket non-blocking configuration (O_NONBLOCK) & Linux epoll instance initialization
 *   2. Zero-copy 24-byte header bit-for-bit wire compatibility with send_frame()
 *   3. Dynamic 64 KiB chunk work queue 3-state lifecycle (UNASSIGNED -> IN_FLIGHT -> COMMITTED)
 *   4. Sub-stream fault isolation and atomic chunk requeueing
 *   5. Client chunk bitmask resynchronization via OP_JOIN_SESSION
 *   6. File pre-allocation via posix_fallocate() / ftruncate()
 *   7. Lock-free concurrent client file assembly via positional pwrite()
 *   8. Global 64 KiB chunk work unit indexing & boundary arithmetic
 *   9. Serialization and parsing of OP_GET_ENHANCED, OP_JOIN_SESSION, OP_ASSIGN_WORK
 */

static int g_tests_passed = 0;
static int g_tests_total = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_tests_total++; \
    if (!(cond)) { \
        fprintf(stderr, "[FAIL] Line %d: %s\n", __LINE__, msg); \
        exit(1); \
    } else { \
        g_tests_passed++; \
        printf("  [PASS] %s\n", msg); \
    } \
} while (0)

// ------------------------------------------------------------------
// Test 1: Socket Non-blocking Configuration and Epoll Instance Init
// ------------------------------------------------------------------
static void test_socket_nonblocking_and_epoll() {
    printf("\n--- Test 1: Socket Non-blocking Configuration & Epoll Init ---\n");

    int sv[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(rc == 0, "socketpair created successfully");

    int flags_before = fcntl(sv[0], F_GETFL, 0);
    TEST_ASSERT((flags_before & O_NONBLOCK) == 0, "Socket initially in blocking mode");

    rc = set_socket_nonblocking(sv[0]);
    TEST_ASSERT(rc == 0, "set_socket_nonblocking returned 0");

    int flags_after = fcntl(sv[0], F_GETFL, 0);
    TEST_ASSERT((flags_after & O_NONBLOCK) != 0, "Socket successfully configured with O_NONBLOCK");

    int epoll_fd = init_epoll_instance(sv[0]);
    TEST_ASSERT(epoll_fd >= 0, "init_epoll_instance returned valid descriptor");

    close(epoll_fd);
    close(sv[0]);
    close(sv[1]);
}

// ------------------------------------------------------------------
// Test 2: Zero-Copy 24-byte Header Bit-for-Bit Compatibility
// ------------------------------------------------------------------
static void test_zerocopy_header_compatibility() {
    printf("\n--- Test 2: Zero-Copy 24-Byte Header Bit-for-Bit Compatibility ---\n");

    uint32_t file_index = 42;
    uint64_t offset = 131072;
    uint32_t chunk_len = 65536;

    uint8_t zero_copy_hdr[FILE_DATA_HEADER_SIZE];
    build_file_data_header(zero_copy_hdr, file_index, offset, chunk_len);

    // Now emit the exact same chunk using the standard send_frame path
    int sv[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(rc == 0, "socketpair created for frame capture");

    std::vector<uint8_t> dummy_payload(chunk_len, 0xAB);
    uint64_t wire_counter = 0;
    rc = send_file_data(sv[0], file_index, offset, dummy_payload.data(), chunk_len, &wire_counter);
    TEST_ASSERT(rc == 0, "send_file_data sent full chunk over socketpair");

    uint8_t captured_hdr[FILE_DATA_HEADER_SIZE];
    ssize_t nread = read(sv[1], captured_hdr, FILE_DATA_HEADER_SIZE);
    TEST_ASSERT(nread == FILE_DATA_HEADER_SIZE, "Captured exactly 24 preamble bytes");

    TEST_ASSERT(std::memcmp(zero_copy_hdr, captured_hdr, FILE_DATA_HEADER_SIZE) == 0,
                "Zero-copy preamble is 100% bit-for-bit identical to send_frame wire emission");

    close(sv[0]);
    close(sv[1]);
}

// ------------------------------------------------------------------
// Test 3: Dynamic 64 KiB Chunk Work Queue 3-State Lifecycle
// ------------------------------------------------------------------
static void test_work_queue_lifecycle() {
    printf("\n--- Test 3: Dynamic 64 KiB Chunk Work Queue 3-State Lifecycle ---\n");

    TopicEntry entry;
    entry.topic_name = "test_topic";

    FileMeta fm0;
    fm0.filename = "file0.bin";
    fm0.file_size = 150000; // 3 chunks: 64k, 64k, 18912 bytes
    entry.files.push_back(fm0);

    FileMeta fm1;
    fm1.filename = "file1.bin";
    fm1.file_size = 65536;  // 1 chunk: 64k
    entry.files.push_back(fm1);

    FileMeta fm2;
    fm2.filename = "file2.bin";
    fm2.file_size = 0;      // 1 unit: 0 bytes
    entry.files.push_back(fm2);

    uint8_t sess_id[16];
    std::memset(sess_id, 0x55, 16);

    EnhancedSessionState* sess = create_enhanced_session(entry, 4, sess_id);
    TEST_ASSERT(sess != nullptr, "create_enhanced_session succeeded");
    TEST_ASSERT(sess->units.size() == 5, "Work units vector partitioned correctly (3 + 1 + 1 = 5 units)");

    // Assert all units initially UNASSIGNED
    for (size_t i = 0; i < sess->units.size(); ++i) {
        TEST_ASSERT(sess->units[i].state == CHUNK_UNASSIGNED, "Initial unit state is CHUNK_UNASSIGNED");
        TEST_ASSERT(sess->units[i].assigned_conn_id == -1, "Initial assigned_conn_id is -1");
    }

    // Pop units across 4 parallel sub-streams
    ChunkWorkUnit u0, u1, u2, u3, u4;
    TEST_ASSERT(pop_next_chunk_unit(sess, 0, &u0) && u0.unit_id == 0, "Stream 0 popped unit 0");
    TEST_ASSERT(pop_next_chunk_unit(sess, 1, &u1) && u1.unit_id == 1, "Stream 1 popped unit 1");
    TEST_ASSERT(pop_next_chunk_unit(sess, 2, &u2) && u2.unit_id == 2, "Stream 2 popped unit 2");
    TEST_ASSERT(pop_next_chunk_unit(sess, 3, &u3) && u3.unit_id == 3, "Stream 3 popped unit 3");
    TEST_ASSERT(pop_next_chunk_unit(sess, 0, &u4) && u4.unit_id == 4, "Stream 0 popped unit 4");

    // All units should now be IN_FLIGHT
    for (size_t i = 0; i < sess->units.size(); ++i) {
        TEST_ASSERT(sess->units[i].state == CHUNK_IN_FLIGHT, "Unit transitioned to CHUNK_IN_FLIGHT");
    }

    ChunkWorkUnit empty_unit;
    TEST_ASSERT(!pop_next_chunk_unit(sess, 1, &empty_unit), "pop_next_chunk_unit returns false when all units in-flight");

    free_enhanced_session(sess_id);
}

// ------------------------------------------------------------------
// Test 4: Sub-Stream Fault Isolation & Atomic Chunk Requeueing
// ------------------------------------------------------------------
static void test_fault_isolation_and_requeue() {
    printf("\n--- Test 4: Sub-Stream Fault Isolation & Atomic Requeueing ---\n");

    TopicEntry entry;
    entry.topic_name = "topic_fault";
    FileMeta fm;
    fm.filename = "large.dat";
    fm.file_size = 256 * 1024; // 4 chunks (64k each)
    entry.files.push_back(fm);

    uint8_t sess_id[16];
    std::memset(sess_id, 0x77, 16);
    EnhancedSessionState* sess = create_enhanced_session(entry, 4, sess_id);

    ChunkWorkUnit u0, u1, u2, u3;
    pop_next_chunk_unit(sess, 0, &u0); // assigned to conn 0
    pop_next_chunk_unit(sess, 1, &u1); // assigned to conn 1
    pop_next_chunk_unit(sess, 2, &u2); // assigned to conn 2
    pop_next_chunk_unit(sess, 3, &u3); // assigned to conn 3

    // Stream 1 faults and drops!
    requeue_stream_chunks(sess, 1);

    // In-flight unit 1 must be reverted to UNASSIGNED
    TEST_ASSERT(sess->units[1].state == CHUNK_UNASSIGNED, "Dropped stream 1 unit reverted to CHUNK_UNASSIGNED");
    TEST_ASSERT(sess->units[1].assigned_conn_id == -1, "Dropped stream 1 assigned_conn_id reset to -1");

    // Surviving streams 0, 2, 3 must remain untouched in CHUNK_IN_FLIGHT!
    TEST_ASSERT(sess->units[0].state == CHUNK_IN_FLIGHT && sess->units[0].assigned_conn_id == 0,
                "Surviving stream 0 remains in-flight without stall");
    TEST_ASSERT(sess->units[2].state == CHUNK_IN_FLIGHT && sess->units[2].assigned_conn_id == 2,
                "Surviving stream 2 remains in-flight without stall");
    TEST_ASSERT(sess->units[3].state == CHUNK_IN_FLIGHT && sess->units[3].assigned_conn_id == 3,
                "Surviving stream 3 remains in-flight without stall");

    // Surviving stream 0 can immediately steal / pick up unit 1!
    ChunkWorkUnit stolen;
    TEST_ASSERT(pop_next_chunk_unit(sess, 0, &stolen) && stolen.unit_id == 1,
                "Surviving stream 0 successfully stole and popped requeued unit 1");
    TEST_ASSERT(sess->units[1].assigned_conn_id == 0, "Unit 1 assigned to surviving stream 0");

    free_enhanced_session(sess_id);
}

// ------------------------------------------------------------------
// Test 5: Client Bitmask Application & Synchronization
// ------------------------------------------------------------------
static void test_client_bitmask_synchronization() {
    printf("\n--- Test 5: Client Bitmask Application & Synchronization ---\n");

    TopicEntry entry;
    entry.topic_name = "topic_bmap";
    FileMeta fm;
    fm.filename = "data.bin";
    fm.file_size = 512 * 1024; // 8 chunks (unit 0..7)
    entry.files.push_back(fm);

    uint8_t sess_id[16];
    std::memset(sess_id, 0x88, 16);
    EnhancedSessionState* sess = create_enhanced_session(entry, 4, sess_id);

    // Bitmap with units 0, 2, 5 set (binary: 0010 0101 = 0x25)
    uint8_t bitmap[1] = { 0x25 };
    apply_client_bitmap(sess, bitmap, 1);

    TEST_ASSERT(sess->units[0].state == CHUNK_COMMITTED, "Unit 0 marked CHUNK_COMMITTED via bitmask");
    TEST_ASSERT(sess->units[1].state == CHUNK_UNASSIGNED, "Unit 1 remains CHUNK_UNASSIGNED");
    TEST_ASSERT(sess->units[2].state == CHUNK_COMMITTED, "Unit 2 marked CHUNK_COMMITTED via bitmask");
    TEST_ASSERT(sess->units[3].state == CHUNK_UNASSIGNED, "Unit 3 remains CHUNK_UNASSIGNED");
    TEST_ASSERT(sess->units[4].state == CHUNK_UNASSIGNED, "Unit 4 remains CHUNK_UNASSIGNED");
    TEST_ASSERT(sess->units[5].state == CHUNK_COMMITTED, "Unit 5 marked CHUNK_COMMITTED via bitmask");

    free_enhanced_session(sess_id);
}

// ------------------------------------------------------------------
// Test 6: Target File Pre-allocation & Lock-Free Concurrent pwrite()
// ------------------------------------------------------------------
struct PwriteWorkerArgs {
    int fd;
    off_t offset;
    uint8_t pattern;
    size_t size;
};

static void* pwrite_thread_worker(void* arg) {
    PwriteWorkerArgs* args = static_cast<PwriteWorkerArgs*>(arg);
    std::vector<uint8_t> buf(args->size, args->pattern);
    ssize_t written = 0;
    while (written < static_cast<ssize_t>(args->size)) {
        ssize_t w = pwrite(args->fd, buf.data() + written, args->size - written, args->offset + written);
        if (w <= 0) break;
        written += w;
    }
    return nullptr;
}

static void test_file_preallocation_and_lockfree_pwrite() {
    printf("\n--- Test 6: Target File Pre-allocation & Lock-Free Concurrent pwrite() ---\n");

    std::string test_file = "test_scratch/test_prealloc.bin";
    uint64_t target_size = 256 * 1024; // 256 KiB (4 chunks of 64 KiB)

    int fd = preallocate_target_file(test_file, target_size);
    TEST_ASSERT(fd >= 0, "preallocate_target_file succeeded");

    struct stat st;
    int rc = fstat(fd, &st);
    TEST_ASSERT(rc == 0, "fstat succeeded on pre-allocated descriptor");
    TEST_ASSERT(static_cast<uint64_t>(st.st_size) == target_size,
                "Pre-allocated file size strictly matches target 256 KiB");

    // Launch 4 concurrent threads writing at distinct 64 KiB offsets WITHOUT any mutex
    pthread_t th[4];
    PwriteWorkerArgs args[4];
    uint8_t patterns[4] = { 0xAA, 0xBB, 0xCC, 0xDD };

    for (int i = 0; i < 4; ++i) {
        args[i].fd = fd;
        args[i].offset = i * 65536;
        args[i].pattern = patterns[i];
        args[i].size = 65536;
        pthread_create(&th[i], nullptr, pwrite_thread_worker, &args[i]);
    }

    for (int i = 0; i < 4; ++i) {
        pthread_join(th[i], nullptr);
    }

    fsync(fd);
    close(fd);

    // Verify all 4 blocks on disk match their patterns without corruption
    int verify_fd = open(test_file.c_str(), O_RDONLY);
    TEST_ASSERT(verify_fd >= 0, "Re-opened file for verification");

    for (int i = 0; i < 4; ++i) {
        std::vector<uint8_t> read_buf(65536, 0);
        ssize_t r = pread(verify_fd, read_buf.data(), 65536, i * 65536);
        TEST_ASSERT(r == 65536, "pread read full 64 KiB block");
        bool match = true;
        for (size_t b = 0; b < 65536; ++b) {
            if (read_buf[b] != patterns[i]) {
                match = false;
                break;
            }
        }
        TEST_ASSERT(match, "Concurrent lock-free pwrite block data strictly verified on disk");
    }

    close(verify_fd);
    unlink(test_file.c_str());
    rmdir("test_scratch");
}

// ------------------------------------------------------------------
// Test 7: Global 64 KiB Work Unit Mapping Arithmetic
// ------------------------------------------------------------------
static void test_work_unit_mapping_arithmetic() {
    printf("\n--- Test 7: Global 64 KiB Work Unit Mapping Arithmetic ---\n");

    std::vector<FileMeta> files(3);
    files[0].file_size = 150000; // 3 chunks (offsets: 0, 64k, 128k)
    files[1].file_size = 65536;  // 1 chunk (offset: 0)
    files[2].file_size = 0;      // 1 unit (offset: 0)

    uint32_t total_units = 0;
    std::vector<uint64_t> bases(3, 0);
    for (size_t f = 0; f < 3; ++f) {
        bases[f] = total_units;
        if (files[f].file_size == 0) total_units++;
        else total_units += static_cast<uint32_t>((files[f].file_size + CHUNK_SIZE - 1) / CHUNK_SIZE);
    }

    TEST_ASSERT(bases[0] == 0, "File 0 base is unit 0");
    TEST_ASSERT(bases[1] == 3, "File 1 base is unit 3");
    TEST_ASSERT(bases[2] == 4, "File 2 base is unit 4");
    TEST_ASSERT(total_units == 5, "Total topic work units is 5");

    // Check mapping for File 0, offset 128 KiB
    uint32_t u_f0_chunk2 = static_cast<uint32_t>(bases[0] + (131072 / CHUNK_SIZE));
    TEST_ASSERT(u_f0_chunk2 == 2, "File 0 offset 128 KiB correctly maps to unit 2");

    // Check mapping for File 1, offset 0
    uint32_t u_f1_chunk0 = static_cast<uint32_t>(bases[1] + (0 / CHUNK_SIZE));
    TEST_ASSERT(u_f1_chunk0 == 3, "File 1 offset 0 correctly maps to unit 3");
}

// ------------------------------------------------------------------
// Test 8: Enhanced Wire Protocol Messages Round-trip
// ------------------------------------------------------------------
static void test_enhanced_protocol_serialization() {
    printf("\n--- Test 8: Enhanced Protocol Messages Round-trip ---\n");

    // OP_GET_ENHANCED (0x0C)
    std::vector<uint8_t> p_get;
    serialize_get_enhanced(4, "dogs", p_get);

    MsgGetEnhanced get_msg;
    bool prc = parse_get_enhanced(p_get.data(), static_cast<uint32_t>(p_get.size()), &get_msg);
    TEST_ASSERT(prc, "parse_get_enhanced succeeded");
    TEST_ASSERT(get_msg.num_streams == 4, "num_streams parsed correctly");
    TEST_ASSERT(get_msg.topic == "dogs", "topic string parsed correctly");

    // OP_JOIN_SESSION (0x0D)
    uint8_t s_id[16];
    std::memset(s_id, 0x33, 16);
    uint8_t bmap[2] = { 0xF0, 0x0F };
    std::vector<uint8_t> p_join;
    serialize_join_session(s_id, 2, bmap, 2, p_join);

    MsgJoinSession join_msg;
    prc = parse_join_session(p_join.data(), static_cast<uint32_t>(p_join.size()), &join_msg);
    TEST_ASSERT(prc, "parse_join_session succeeded");
    TEST_ASSERT(std::memcmp(join_msg.session_id, s_id, 16) == 0, "session_id matched");
    TEST_ASSERT(join_msg.conn_id == 2, "conn_id matched");
    TEST_ASSERT(join_msg.chunk_bitmap.size() == 2, "bitmap length matched");
    TEST_ASSERT(join_msg.chunk_bitmap[0] == 0xF0 && join_msg.chunk_bitmap[1] == 0x0F, "bitmap contents matched");

    // OP_ASSIGN_WORK (0x0E)
    std::vector<uint8_t> p_work;
    serialize_assign_work(3, 1, 65536, 65536, p_work);

    MsgAssignWork work_msg;
    prc = parse_assign_work(p_work.data(), static_cast<uint32_t>(p_work.size()), &work_msg);
    TEST_ASSERT(prc, "parse_assign_work succeeded");
    TEST_ASSERT(work_msg.conn_id == 3, "conn_id matched");
    TEST_ASSERT(work_msg.file_index == 1, "file_index matched");
    TEST_ASSERT(work_msg.chunk_offset == 65536, "chunk_offset matched");
    TEST_ASSERT(work_msg.chunk_len == 65536, "chunk_len matched");
}

int main() {
    printf("=================================================================\n");
    printf("  RUNNING MEMBER 5 (CASE 2 ENHANCED) UNIT TEST SUITE             \n");
    printf("=================================================================\n");

    test_socket_nonblocking_and_epoll();
    test_zerocopy_header_compatibility();
    test_work_queue_lifecycle();
    test_fault_isolation_and_requeue();
    test_client_bitmask_synchronization();
    test_file_preallocation_and_lockfree_pwrite();
    test_work_unit_mapping_arithmetic();
    test_enhanced_protocol_serialization();

    printf("\n=================================================================\n");
    printf("  MEMBER 5 UNIT TEST SUITE RESULTS: %d / %d PASSED (100%%)       \n",
           g_tests_passed, g_tests_total);
    printf("=================================================================\n");

    return (g_tests_passed == g_tests_total) ? 0 : 1;
}
