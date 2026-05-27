#include "config.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <iostream>

AppConfig AppConfig::load_from_file(const std::string& path) {
    AppConfig cfg;
    try {
        std::ifstream f(path);
        if (!f.is_open()) {
            std::cerr << "Cannot open config file: " << path
                      << ", using defaults." << std::endl;
            return load_default();
        }
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
