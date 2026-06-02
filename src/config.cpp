#include "config.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <iostream>
#include <filesystem>
#include <vector>

AppConfig AppConfig::load_from_file(const std::string& path) {
    AppConfig cfg;

    // 依次尝试: 当前目录 → 父目录 (方便从 build/ 目录直接运行)
    std::vector<std::string> candidates = {path, "config.json"};
    if (path.find('/') == std::string::npos) {
        candidates.push_back("../" + path);
    }

    std::string found_path;
    for (auto& p : candidates) {
        if (std::filesystem::exists(p)) {
            found_path = p;
            break;
        }
    }

    if (found_path.empty()) {
        std::cerr << "Cannot open config file: " << path
                  << ", using defaults." << std::endl;
        return load_default();
    }

    try {
        std::ifstream f(found_path);
        nlohmann::json j = nlohmann::json::parse(f);
        cfg.mode                = j.value("mode", cfg.mode);
        cfg.port                = j.value("port", cfg.port);
        cfg.log_file            = j.value("log_file", cfg.log_file);
        cfg.log_buf_size        = j.value("log_buf_size", cfg.log_buf_size);
        cfg.log_split_lines     = j.value("log_split_lines", cfg.log_split_lines);
        cfg.log_max_queue_size  = j.value("log_max_queue_size", cfg.log_max_queue_size);
        cfg.close_log           = j.value("close_log", cfg.close_log);
        cfg.thread_num          = j.value("thread_num", cfg.thread_num);
        cfg.plugin_dir          = j.value("plugin_dir", cfg.plugin_dir);
        cfg.llm_base_url        = j.value("llm_base_url", cfg.llm_base_url);
        cfg.llm_model           = j.value("llm_model", cfg.llm_model);
    } catch (const std::exception& e) {
        std::cerr << "Config parse error: " << e.what()
                  << ", using defaults." << std::endl;
        return load_default();
    }
    return cfg;
}

AppConfig AppConfig::load_default() {
    return AppConfig{};
}
