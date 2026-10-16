#ifndef DIRECTORY_MAPPER_H
#define DIRECTORY_MAPPER_H

#include "common_types.h"

#include <string>
#include <unordered_map>

/*
 * directory_mapper.h - Member 1 (PR #3, Wave 1)
 *
 * Scans <topics_root_dir> at startup and builds an in-memory, read-only hash map
 * of topics. Inside each topic, files are sorted lexicographically by filename
 * so file_index (0 .. file_count - 1) is 100% deterministic across all clients
 * and resume attempts.
 */

// Scans root_dir and populates topic_map. Returns number of topics mapped, or -1 on error.
int scan_and_map_topics(const std::string& root_dir,
                        std::unordered_map<std::string, TopicEntry>& topic_map);

// Fast O(1) lock-free lookup in the read-only topic hash map. Returns nullptr if not found.
const TopicEntry* get_topic_path(const std::unordered_map<std::string, TopicEntry>& topic_map,
                                 const std::string& topic);

#endif // DIRECTORY_MAPPER_H
