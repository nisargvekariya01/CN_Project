#include "common_types.h"
#include "directory_mapper.h"
#include "concurrency.h"
#include "metrics_logger.h"

#include <stdio.h>
#include <unistd.h>
#include <iostream>

#ifndef PART_NUM
#define PART_NUM 1
#endif

#ifndef CASE_MODE
#define CASE_MODE 0
#endif

int main(int argc, char* argv[]) {
    uint16_t port = 0;
    std::string root_dir;
    double pf = 0.0;
    int threaded_mode = 1;
    uint32_t seed = 0;

    if (parse_server_cli(argc, argv, PART_NUM, &port, &root_dir, &pf, &threaded_mode, &seed) != 0) {
        return 1;
    }

    register_signal_handlers();

    log_event("INFO", "Starting Server (Part=%d, CaseMode=%d, Port=%u, pf=%.2f, Threaded=%d)",
              PART_NUM, CASE_MODE, port, pf, threaded_mode);

    std::unordered_map<std::string, TopicEntry> topic_map;
    int topics_count = scan_and_map_topics(root_dir, topic_map);
    if (topics_count < 0) {
        fprintf(stderr, "Failed to load topics from directory: %s\n", root_dir.c_str());
        return 1;
    }

    int listen_fd = create_listening_socket(port);
    if (listen_fd < 0) {
        fprintf(stderr, "Failed to initialize server socket on port %u\n", port);
        return 1;
    }

    if (threaded_mode == 0) {
        run_sequential_loop(listen_fd, topic_map, pf, CASE_MODE, seed);
    } else {
        run_concurrent_loop(listen_fd, topic_map, pf, CASE_MODE, seed);
    }

    log_event("INFO", "Server shutdown complete");
    close(listen_fd);
    return 0;
}
