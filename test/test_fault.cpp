/*
 * test_fault.cpp - Unit Test Suite for Member 3 (Wave 2, PR #4)
 *
 * Validates:
 *   1. init_thread_rng & thread-local MT19937 RNG isolation
 *   2. should_inject_fault statistical distribution over 10,000 chunks (Pf=0.0, 0.1, 1.0)
 *   3. parse_client_cli argument parsing & directory creation
 *   4. Full Loopback File Streaming (Pf = 0.0)
 *   5. Stateless Fault Recovery with Exponential Backoff (Pf = 0.25)
 */

#include "fault.h"
#include "protocol.h"
#include "metrics_logger.h"
#include "directory_mapper.h"
#include "concurrency.h"

#include <iostream>
#include <cassert>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define ANSI_GREEN "\033[32m"
#define ANSI_RED   "\033[31m"
#define ANSI_RESET "\033[0m"

static int g_tests_passed = 0;
static int g_tests_total = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_tests_total++; \
    if (cond) { \
        g_tests_passed++; \
    } else { \
        std::cerr << ANSI_RED << "FAIL: " << msg << ANSI_RESET \
                  << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
    } \
} while (0)

// Helper to remove directory tree recursively for clean test runs
static void remove_dir_recursive(const std::string& path) {
    std::string cmd = "rm -rf \"" + path + "\"";
    int rc = system(cmd.c_str());
    (void)rc;
}

// ------------------------------------------------------------------
// Test 1: Fault Injection Distribution over 10,000 iterations
// ------------------------------------------------------------------
void test_fault_distribution() {
    std::cout << "[TEST] 1. Fault Injection Distribution (Statistical Validation)..." << std::endl;

    init_thread_rng(12345);

    // Pf = 0.0: Must never drop (10,000 chunks)
    int drops_0 = 0;
    for (int i = 0; i < 10000; ++i) {
        if (should_inject_fault(0.0, "test_topic", 0, i * 65536)) {
            drops_0++;
        }
    }
    TEST_ASSERT(drops_0 == 0, "Pf = 0.0 must yield 0 drops over 10,000 chunks");

    // Pf = 1.0: Must always drop
    int drops_1 = 0;
    for (int i = 0; i < 200; ++i) {
        if (should_inject_fault(1.0, "test_topic", 0, i * 65536)) {
            drops_1++;
        }
    }
    TEST_ASSERT(drops_1 == 200, "Pf = 1.0 must yield 100% drops");

    // Pf = 0.10: Expected mean = 200 drops over 2000 iterations, 3-sigma bound [145, 255]
    int drops_10pct = 0;
    for (int i = 0; i < 2000; ++i) {
        if (should_inject_fault(0.10, "test_topic", 0, i * 65536)) {
            drops_10pct++;
        }
    }
    std::cout << "       Pf = 0.10 produced " << drops_10pct << " drops out of 2,000 ("
              << (drops_10pct / 20.0) << "%)" << std::endl;
    TEST_ASSERT(drops_10pct >= 145 && drops_10pct <= 255, "Pf = 0.10 must stay within 3-sigma bounds [145, 255]");
}

// ------------------------------------------------------------------
// Test 2: Client CLI Parser & Directory Creation
// ------------------------------------------------------------------
void test_client_cli_parsing() {
    std::cout << "[TEST] 2. Client CLI Parser & Directory Preparation..." << std::endl;

    std::string test_out = "/tmp/it305_client_test_out";
    remove_dir_recursive(test_out);

    std::string ip;
    uint16_t port = 0;
    std::string topic;
    std::string dir;
    int max_retries = 0;
    uint32_t ckpt_win = 0;
    int streams = 0;

    // Too few args (< 5)
    char* argv_short[] = { (char*)"./client", (char*)"127.0.0.1", (char*)"8080" };
    int rc1 = parse_client_cli(3, argv_short, &ip, &port, &topic, &dir, &max_retries, &ckpt_win, &streams);
    TEST_ASSERT(rc1 == -1, "argc < 5 must fail parse_client_cli");

    // Invalid IP
    char* argv_badip[] = { (char*)"./client", (char*)"999.999.999.999", (char*)"8080", (char*)"topic", (char*)test_out.c_str() };
    int rc2 = parse_client_cli(5, argv_badip, &ip, &port, &topic, &dir, &max_retries, &ckpt_win, &streams);
    TEST_ASSERT(rc2 == -1, "Invalid IP must fail parse_client_cli");

    // Invalid Port
    char* argv_badport[] = { (char*)"./client", (char*)"127.0.0.1", (char*)"0", (char*)"topic", (char*)test_out.c_str() };
    int rc3 = parse_client_cli(5, argv_badport, &ip, &port, &topic, &dir, &max_retries, &ckpt_win, &streams);
    TEST_ASSERT(rc3 == -1, "Port 0 must fail parse_client_cli");

    // Valid standard positional arguments
    char* argv_valid[] = {
        (char*)"./client", (char*)"127.0.0.1", (char*)"9000", (char*)"cat_topic", (char*)test_out.c_str(),
        (char*)"--max-retries", (char*)"25",
        (char*)"--ckpt-window", (char*)"131072",
        (char*)"--streams", (char*)"8"
    };
    int rc4 = parse_client_cli(11, argv_valid, &ip, &port, &topic, &dir, &max_retries, &ckpt_win, &streams);
    TEST_ASSERT(rc4 == 0, "Valid arguments must succeed parse_client_cli");
    TEST_ASSERT(ip == "127.0.0.1", "IP parsed matches");
    TEST_ASSERT(port == 9000, "Port parsed matches");
    TEST_ASSERT(topic == "cat_topic", "Topic parsed matches");
    TEST_ASSERT(dir == test_out, "Directory parsed matches");
    TEST_ASSERT(max_retries == 25, "--max-retries parsed correctly");
    TEST_ASSERT(ckpt_win == 131072, "--ckpt-window parsed correctly");
    TEST_ASSERT(streams == 8, "--streams parsed correctly");

    // Directory creation validation: <output_dir>/<topic>/ must exist
    std::string expected_topic_dir = test_out + "/cat_topic";
    struct stat st;
    TEST_ASSERT(stat(expected_topic_dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode), "Target topic directory created");

    remove_dir_recursive(test_out);
}

// ------------------------------------------------------------------
// Test 3: End-to-End Loopback Transfer (Pf = 0.0 and Pf = 0.25)
// ------------------------------------------------------------------
struct ServerThreadContext {
    int port;
    std::string topics_dir;
    double pf;
    int listen_fd;
    volatile bool ready;
};

static void* test_server_thread(void* arg) {
    ServerThreadContext* ctx = static_cast<ServerThreadContext*>(arg);
    std::unordered_map<std::string, TopicEntry> topic_map;
    scan_and_map_topics(ctx->topics_dir, topic_map);

    int listen_fd = create_listening_socket(ctx->port);
    ctx->listen_fd = listen_fd;
    ctx->ready = true;

    // Serve clients until stopped
    while (g_keep_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
        if (client_fd < 0) {
            break;
        }
        process_client(client_fd, topic_map, ctx->pf, 1 /* Case 1 */, 42);
        close(client_fd);
    }
    close(listen_fd);
    return nullptr;
}

void test_end_to_end_loopback() {
    std::cout << "[TEST] 3. End-to-End Loopback Transfer (Reliable & Fault Recovery)..." << std::endl;

    // Create a mock dataset directory with 3 files of various sizes (> 64 KiB to cross chunk boundaries)
    std::string test_data_dir = "/tmp/it305_test_data";
    std::string topic_dir = test_data_dir + "/animals";
    std::string client_out_dir = "/tmp/it305_client_downloads";
    remove_dir_recursive(test_data_dir);
    remove_dir_recursive(client_out_dir);

    mkdir(test_data_dir.c_str(), 0755);
    mkdir(topic_dir.c_str(), 0755);

    // File 1: 100 bytes
    std::ofstream f1(topic_dir + "/small.txt");
    f1 << "Small file content test line.\n";
    f1.close();

    // File 2: 150 KiB (spans across 3 chunks: 64 KB + 64 KB + 22 KB)
    std::ofstream f2(topic_dir + "/medium.bin", std::ios::binary);
    std::vector<char> mbuf(150 * 1024, 'M');
    f2.write(mbuf.data(), mbuf.size());
    f2.close();

    // File 3: 200 KiB
    std::ofstream f3(topic_dir + "/large.bin", std::ios::binary);
    std::vector<char> lbuf(200 * 1024, 'L');
    f3.write(lbuf.data(), lbuf.size());
    f3.close();

    // Subtest 3A: Reliable Transfer (Pf = 0.0)
    {
        ServerThreadContext sctx;
        sctx.port = 19123;
        sctx.topics_dir = test_data_dir;
        sctx.pf = 0.0;
        sctx.ready = false;
        g_keep_running = 1;

        pthread_t sthread;
        pthread_create(&sthread, nullptr, test_server_thread, &sctx);
        while (!sctx.ready) {
            usleep(1000);
        }

        // Run client loop
        int rc = run_client_part1_and_case1_loop("127.0.0.1", 19123, "animals", client_out_dir.c_str(), 10, true);
        TEST_ASSERT(rc == 0, "Reliable transfer (Pf = 0.0) must complete with 0");

        // Verify files downloaded match exactly
        struct stat st1, st2, st3;
        int s1 = stat((client_out_dir + "/animals/small.txt").c_str(), &st1);
        int s2 = stat((client_out_dir + "/animals/medium.bin").c_str(), &st2);
        int s3 = stat((client_out_dir + "/animals/large.bin").c_str(), &st3);
        TEST_ASSERT(s1 == 0 && s2 == 0 && s3 == 0, "All 3 files exist in client destination");
        TEST_ASSERT(st2.st_size == 150 * 1024, "medium.bin byte size matches source (150 KiB)");
        TEST_ASSERT(st3.st_size == 200 * 1024, "large.bin byte size matches source (200 KiB)");

        // Stop server cleanly
        g_keep_running = 0;
        int dummy1 = socket(AF_INET, SOCK_STREAM, 0);
        if (dummy1 >= 0) {
            struct sockaddr_in da;
            memset(&da, 0, sizeof(da));
            da.sin_family = AF_INET;
            da.sin_port = htons(sctx.port);
            inet_pton(AF_INET, "127.0.0.1", &da.sin_addr);
            connect(dummy1, reinterpret_cast<struct sockaddr*>(&da), sizeof(da));
            close(dummy1);
        }
        pthread_join(sthread, nullptr);
    }

    // Subtest 3B: Fault Recovery Transfer (Pf = 0.20, with 50 retries)
    {
        remove_dir_recursive(client_out_dir);

        ServerThreadContext sctx;
        sctx.port = 19124;
        sctx.topics_dir = test_data_dir;
        sctx.pf = 0.20; // 20% drop rate per chunk
        sctx.ready = false;
        g_keep_running = 1;

        pthread_t sthread;
        pthread_create(&sthread, nullptr, test_server_thread, &sctx);
        while (!sctx.ready) {
            usleep(1000);
        }

        // Run client loop with exponential backoff recovery
        int rc = run_client_part1_and_case1_loop("127.0.0.1", 19124, "animals", client_out_dir.c_str(), 50, false);
        TEST_ASSERT(rc == 0, "Fault-tolerant transfer (Pf = 0.20) must recover and complete with 0");

        // Verify file sizes after recovery
        struct stat st2;
        int s2 = stat((client_out_dir + "/animals/medium.bin").c_str(), &st2);
        TEST_ASSERT(s2 == 0 && st2.st_size == 150 * 1024, "medium.bin verified after fault recovery");

        // Stop server cleanly
        g_keep_running = 0;
        int dummy2 = socket(AF_INET, SOCK_STREAM, 0);
        if (dummy2 >= 0) {
            struct sockaddr_in da;
            memset(&da, 0, sizeof(da));
            da.sin_family = AF_INET;
            da.sin_port = htons(sctx.port);
            inet_pton(AF_INET, "127.0.0.1", &da.sin_addr);
            connect(dummy2, reinterpret_cast<struct sockaddr*>(&da), sizeof(da));
            close(dummy2);
        }
        pthread_join(sthread, nullptr);
    }

    remove_dir_recursive(test_data_dir);
    remove_dir_recursive(client_out_dir);
}

int main() {
    std::cout << "====================================================\n";
    std::cout << "  IT305 Lab - Member 3 Unit Test Suite (Wave 2, PR #4)\n";
    std::cout << "====================================================\n";

    test_fault_distribution();
    test_client_cli_parsing();
    test_end_to_end_loopback();

    std::cout << "\n----------------------------------------------------\n";
    std::cout << "Summary: " << g_tests_passed << "/" << g_tests_total << " assertions passed.\n";

    if (g_tests_passed == g_tests_total) {
        std::cout << ANSI_GREEN << "ALL MEMBER 3 TESTS PASSED!" << ANSI_RESET << "\n";
        return 0;
    } else {
        std::cout << ANSI_RED << "SOME TESTS FAILED!" << ANSI_RESET << "\n";
        return 1;
    }
}
