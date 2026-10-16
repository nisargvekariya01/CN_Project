#include "directory_mapper.h"
#include "metrics_logger.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <algorithm>

int scan_and_map_topics(const std::string& root_dir,
                        std::unordered_map<std::string, TopicEntry>& topic_map) {
    if (root_dir.empty()) {
        log_event("ERROR", "Empty topics root directory provided");
        return -1;
    }

    std::string norm_root = root_dir;
    while (norm_root.size() > 1 && norm_root.back() == '/') {
        norm_root.pop_back();
    }

    DIR* root_dp = opendir(norm_root.c_str());
    if (!root_dp) {
        log_event("ERROR", "Failed to opendir topics root: %s", norm_root.c_str());
        return -1;
    }

    topic_map.clear();
    struct dirent* topic_ent = nullptr;

    while ((topic_ent = readdir(root_dp)) != nullptr) {
        if (strcmp(topic_ent->d_name, ".") == 0 || strcmp(topic_ent->d_name, "..") == 0) {
            continue;
        }

        std::string topic_path = norm_root + "/" + topic_ent->d_name;
        struct stat st_topic;
        if (stat(topic_path.c_str(), &st_topic) != 0 || !S_ISDIR(st_topic.st_mode)) {
            continue;
        }

        DIR* topic_dp = opendir(topic_path.c_str());
        if (!topic_dp) {
            log_event("ERROR", "Failed to opendir topic directory: %s", topic_path.c_str());
            continue;
        }

        TopicEntry te;
        te.topic_name = topic_ent->d_name;
        te.dir_path = topic_path;
        te.total_bytes = 0;

        struct dirent* file_ent = nullptr;
        while ((file_ent = readdir(topic_dp)) != nullptr) {
            if (strcmp(file_ent->d_name, ".") == 0 || strcmp(file_ent->d_name, "..") == 0) {
                continue;
            }

            std::string file_path = topic_path + "/" + file_ent->d_name;
            struct stat st_file;
            if (stat(file_path.c_str(), &st_file) != 0 || !S_ISREG(st_file.st_mode)) {
                continue;
            }

            FileMeta fm;
            fm.filename = file_ent->d_name;
            fm.full_path = file_path;
            fm.file_size = static_cast<uint64_t>(st_file.st_size);

            te.files.push_back(fm);
            te.total_bytes += fm.file_size;
        }
        closedir(topic_dp);

        // Lexicographically sort by filename so file_index is 100% deterministic across runs
        std::sort(te.files.begin(), te.files.end(), [](const FileMeta& a, const FileMeta& b) {
            return a.filename < b.filename;
        });

        topic_map[te.topic_name] = te;
        log_event("INFO", "Indexed topic '%s': %zu files, %llu total bytes",
                  te.topic_name.c_str(), te.files.size(),
                  static_cast<unsigned long long>(te.total_bytes));
    }

    closedir(root_dp);
    log_event("INFO", "Successfully loaded %zu topics from %s", topic_map.size(), norm_root.c_str());
    return static_cast<int>(topic_map.size());
}

const TopicEntry* get_topic_path(const std::unordered_map<std::string, TopicEntry>& topic_map,
                                 const std::string& topic) {
    // Exact match first (fast O(1) hash map lookup)
    auto it = topic_map.find(topic);
    if (it != topic_map.end()) {
        return &(it->second);
    }
    // Case-insensitive fallback (e.g. handles "Cane" vs "cane")
    for (const auto& pair : topic_map) {
        if (strcasecmp(pair.first.c_str(), topic.c_str()) == 0) {
            return &(pair.second);
        }
    }
    return nullptr;
}
