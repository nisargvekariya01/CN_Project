/*
 * test_mapper.cpp - Member 1 standalone unit test (PR #3, Wave 1)
 *
 * Verifies:
 *   1. parse_server_cli() positional validation and flags for Part 1 & Part 2
 *   2. scan_and_map_topics() topic directory scanning, stat queries, total_bytes calculation
 *   3. Deterministic lexicographic sorting of TopicEntry.files
 *   4. get_topic_path() O(1) hash map lookup and not-found behavior
 *   5. create_listening_socket() binding and SO_REUSEADDR setup
 *
 * Build: g++ -O2 -Wall -Wextra -Werror -pthread -std=c++17 \
 *            -I. -I../Part1 test_mapper.cpp ../Part1/directory_mapper.cpp \
 *            ../Part1/concurrency.cpp ../Part1/protocol.cpp ../Part1/metrics_logger.cpp \
 *            -o test_mapper
 * Run:   ./test_mapper
 */

#include "directory_mapper.h"
#include "concurrency.h"
#include "protocol.h"
#include "metrics_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <assert.h>
#include <string>
#include <vector>
#include <iostream>

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* what) {
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        printf("  [FAIL] %s\n", what);
    }
}

static void check_u64(uint64_t got, uint64_t want, const char* what) {
    if (got == want) {
        g_pass++;
    } else {
        g_fail++;
        printf("  [FAIL] %s (got %llu, want %llu)\n", what,
               static_cast<unsigned long long>(got),
               static_cast<unsigned long long>(want));
    }
}

// Forward declare parse_server_cli from server.cpp or define test harness
extern int parse_server_cli(int argc, char* argv[], int part_num, uint16_t* out_port,
                            std::string* out_root_dir, double* out_pf, int* out_threaded_mode,
                            uint32_t* out_seed);

int main() {
    printf("=========================================\n");
    printf(" IT305 Server Core & Mapper Unit Tests (Member 1)\n");
    printf("=========================================\n\n");

    // -------------------------------------------------------------------------
    // Test 1: CLI Argument Parsing
    // -------------------------------------------------------------------------
    printf("--- Test 1: CLI parsing & positional validation ---\n");
    {
        uint16_t port = 0;
        std::string root_dir;
        double pf = 0.0;
        int threaded = 1;
        uint32_t seed = 0;

        // Part 1: Valid positional CLI
        char* v1[] = {(char*)"server", (char*)"8080", (char*)"."};
        int rc1 = parse_server_cli(3, v1, 1, &port, &root_dir, &pf, &threaded, &seed);
        check(rc1 == 0, "Part 1 valid CLI accepted");
        check(port == 8080, "Port parsed as 8080");
        check(root_dir == ".", "Root dir parsed as '.'");
        check(threaded == 1, "Default mode is multi-threaded");

        // Part 1 with flags
        char* v2[] = {(char*)"server", (char*)"9000", (char*)".", (char*)"--single-threaded", (char*)"--seed", (char*)"42"};
        int rc2 = parse_server_cli(6, v2, 1, &port, &root_dir, &pf, &threaded, &seed);
        check(rc2 == 0, "Part 1 with flags accepted");
        check(port == 9000, "Port parsed as 9000");
        check(threaded == 0, "Single threaded flag parsed");
        check(seed == 42, "Seed parsed as 42");

        // Part 1 invalid port (< 1024)
        char* v3[] = {(char*)"server", (char*)"80", (char*)"."};
        int rc3 = parse_server_cli(3, v3, 1, &port, &root_dir, &pf, &threaded, &seed);
        check(rc3 == -1, "Privileged port < 1024 rejected");

        // Part 1 invalid non-existent dir
        char* v4[] = {(char*)"server", (char*)"8080", (char*)"/non_existent_folder_xyz_123"};
        int rc4 = parse_server_cli(3, v4, 1, &port, &root_dir, &pf, &threaded, &seed);
        check(rc4 == -1, "Non-existent directory rejected");

        // Part 2 valid positional CLI (argc >= 4)
        char* v5[] = {(char*)"server", (char*)"8080", (char*)".", (char*)"0.35"};
        int rc5 = parse_server_cli(4, v5, 2, &port, &root_dir, &pf, &threaded, &seed);
        check(rc5 == 0, "Part 2 valid CLI accepted");
        check(pf == 0.35, "Failure probability parsed as 0.35");

        // Part 2 invalid pf (> 1.0)
        char* v6[] = {(char*)"server", (char*)"8080", (char*)".", (char*)"1.5"};
        int rc6 = parse_server_cli(4, v6, 2, &port, &root_dir, &pf, &threaded, &seed);
        check(rc6 == -1, "Failure probability > 1.0 rejected");
    }

    // -------------------------------------------------------------------------
    // Test 2: Topic Directory Scanning, Mapping, and Deterministic Sorting
    // -------------------------------------------------------------------------
    printf("\n--- Test 2: Topic Directory Scanning & Deterministic Sorting ---\n");
    {
        // Create mock topic directory structure in /tmp/it305_test_topics
        const char* base_dir = "/tmp/it305_test_topics";
        mkdir(base_dir, 0755);

        // Topic 1: Cane
        std::string cane_dir = std::string(base_dir) + "/Cane";
        mkdir(cane_dir.c_str(), 0755);

        // Write files out of alphabetical order
        FILE* f1 = fopen((cane_dir + "/zebra.jpg").c_str(), "wb");
        fwrite("12345678", 1, 8, f1); // 8 bytes
        fclose(f1);

        FILE* f2 = fopen((cane_dir + "/apple.jpg").c_str(), "wb");
        fwrite("1234", 1, 4, f2); // 4 bytes
        fclose(f2);

        FILE* f3 = fopen((cane_dir + "/mango.jpg").c_str(), "wb");
        fwrite("123456", 1, 6, f3); // 6 bytes
        fclose(f3);

        // Topic 2: Cavallo
        std::string cavallo_dir = std::string(base_dir) + "/Cavallo";
        mkdir(cavallo_dir.c_str(), 0755);
        FILE* f4 = fopen((cavallo_dir + "/horse.png").c_str(), "wb");
        fwrite("horse_bytes", 1, 11, f4);
        fclose(f4);

        std::unordered_map<std::string, TopicEntry> topic_map;
        int count = scan_and_map_topics(base_dir, topic_map);
        check(count == 2, "Found exactly 2 topics (Cane, Cavallo)");

        // Verify Cane
        const TopicEntry* cane_entry = get_topic_path(topic_map, "Cane");
        check(cane_entry != nullptr, "get_topic_path('Cane') found");
        if (cane_entry) {
            check(cane_entry->files.size() == 3, "Cane has 3 files");
            check_u64(cane_entry->total_bytes, 18, "Cane total_bytes = 8 + 4 + 6 = 18");

            // Verify strict lexicographical sorting: apple.jpg, mango.jpg, zebra.jpg
            check(cane_entry->files[0].filename == "apple.jpg", "File 0 is apple.jpg");
            check_u64(cane_entry->files[0].file_size, 4, "apple.jpg size is 4");
            check(cane_entry->files[1].filename == "mango.jpg", "File 1 is mango.jpg");
            check_u64(cane_entry->files[1].file_size, 6, "mango.jpg size is 6");
            check(cane_entry->files[2].filename == "zebra.jpg", "File 2 is zebra.jpg");
            check_u64(cane_entry->files[2].file_size, 8, "zebra.jpg size is 8");
        }

        // Verify Cavallo
        const TopicEntry* cavallo_entry = get_topic_path(topic_map, "Cavallo");
        check(cavallo_entry != nullptr, "get_topic_path('Cavallo') found");
        if (cavallo_entry) {
            check(cavallo_entry->files.size() == 1, "Cavallo has 1 file");
            check_u64(cavallo_entry->total_bytes, 11, "Cavallo total_bytes = 11");
        }

        // Verify Non-existent topic
        const TopicEntry* missing = get_topic_path(topic_map, "Elefante");
        check(missing == nullptr, "get_topic_path('Elefante') returned nullptr as expected");

        // Clean up test files
        unlink((cane_dir + "/zebra.jpg").c_str());
        unlink((cane_dir + "/apple.jpg").c_str());
        unlink((cane_dir + "/mango.jpg").c_str());
        rmdir(cane_dir.c_str());
        unlink((cavallo_dir + "/horse.png").c_str());
        rmdir(cavallo_dir.c_str());
        rmdir(base_dir);
    }

    // -------------------------------------------------------------------------
    // Test 3: TCP Listening Socket Creation & Setup
    // -------------------------------------------------------------------------
    printf("\n--- Test 3: TCP Listening Socket Setup ---\n");
    {
        register_signal_handlers();
        uint16_t test_port = 18080;
        int lfd = create_listening_socket(test_port);
        check(lfd >= 0, "Created listening socket on port 18080");
        if (lfd >= 0) {
            close(lfd);
        }
    }

    printf("\n=========================================\n");
    printf(" PASSED: %d   FAILED: %d\n", g_pass, g_fail);
    printf("=========================================\n");

    return (g_fail == 0) ? 0 : 1;
}
