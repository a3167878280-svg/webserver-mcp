#pragma once

#include <string>

struct AppConfig {
    std::string mode = "stdio";   // "stdio" 或 "http"
    int port = 9006;
    std::string log_file = "ServerLog";
    int log_buf_size = 8192;
    int log_split_lines = 5000000;
    int log_max_queue_size = 0;   // 0 = 同步写，>0 = 异步队列长度
    int close_log = 0;            // 1 = 关闭日志
    int thread_num = 4;
    std::string plugin_dir = "./plugins";
    std::string llm_base_url = "https://api.openai.com/v1";
    std::string llm_model = "gpt-4o";

    static AppConfig load_from_file(const std::string& path);
    static AppConfig load_default();
};
