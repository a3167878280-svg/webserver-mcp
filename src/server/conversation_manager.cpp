/**
 * 对话管理器实现 — 用异步日志系统做多轮对话持久化
 *
 * 写路径: save_to_file() → Log::persist() → persist 队列 → 独立线程异步落盘
 * 读路径: load_all() 在构造时同步执行，从 JSON 文件恢复内存状态
 *
 * 和日志系统共用异步基础设施 (block_queue + 独立写线程)，
 * 不再自己开 std::ofstream 同步写文件。
 */

#include "conversation_manager.h"
#include "../log/log.h"
#include "../common.h"
#include <filesystem>
#include <fstream>
#include <random>
#include <ctime>
#include <sstream>
#include <iomanip>

namespace server {

namespace fs = std::filesystem;

static std::string g_log_dir = "logs";  // 对话 JSON 文件存储目录

ConversationManager::ConversationManager(Log* log) : m_log(log) {
    fs::create_directories(g_log_dir);  // 确保目录存在
    load_all();                          // 恢复之前保存的对话
}

std::string ConversationManager::gen_id() {
    static const char* hex = "0123456789abcdef";
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    std::string id;
    for (int i = 0; i < 16; ++i) id += hex[dis(gen)];
    return id;
}

std::string ConversationManager::now_str() {
    auto t = std::time(nullptr);
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M");
    return oss.str();
}

ConversationManager::ConvInfo ConversationManager::create(const std::string& title) {
    auto conv = std::make_shared<Conversation>();
    conv->id = gen_id();
    conv->title = title;
    conv->log_file = g_log_dir + "/conv_" + conv->id + ".json";
    conv->created_at = now_str();

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_convs[conv->id] = conv;
    }

    save_to_file(*conv);
    LOG_INFO("Created conversation: %s (%s)", conv->id.c_str(), title.c_str());

    return {conv->id, conv->title, conv->log_file, conv->created_at};
}

bool ConversationManager::remove(const std::string& id) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_convs.find(id);
    if (it == m_convs.end()) return false;

    // 删除日志文件
    std::error_code ec;
    if (fs::exists(it->second->log_file)) {
        fs::remove(it->second->log_file, ec);
    }

    LOG_INFO("Deleted conversation: %s", id.c_str());
    m_convs.erase(it);
    return true;
}

std::vector<ConversationManager::ConvInfo> ConversationManager::list() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<ConvInfo> result;
    for (auto& kv : m_convs) {
        result.push_back({kv.second->id, kv.second->title,
                          kv.second->log_file, kv.second->created_at});
    }
    return result;
}

void ConversationManager::append_history(const std::string& id, const llm::Message& msg) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_convs.find(id);
    if (it == m_convs.end()) return;

    auto& conv = *it->second;
    std::lock_guard<std::mutex> clk(conv.mtx);
    conv.history.push_back(msg);
    save_to_file(conv);
}

std::vector<llm::Message> ConversationManager::get_history(const std::string& id) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_convs.find(id);
    if (it == m_convs.end()) return {};

    std::lock_guard<std::mutex> clk(it->second->mtx);
    return it->second->history;
}

bool ConversationManager::exists(const std::string& id) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_convs.find(id) != m_convs.end();
}

void ConversationManager::set_title(const std::string& id, const std::string& title) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_convs.find(id);
    if (it == m_convs.end()) return;
    it->second->title = title;
    save_to_file(*it->second);
}

/**
 * 存盘 — 将对话完整状态 JSON 推入 Log 的异步持久化队列
 *
 * 不再同步写 std::ofstream: 由 Log 系统的 persist 线程异步落盘，
 * 队列满时降级为同步写 (由 Log::persist() 内部处理)
 */
void ConversationManager::save_to_file(const Conversation& conv) {
    nlohmann::json j;
    j["id"] = conv.id;
    j["title"] = conv.title;
    j["created_at"] = conv.created_at;
    nlohmann::json hist = nlohmann::json::array();
    for (auto& m : conv.history) {
        nlohmann::json mj;
        mj["role"] = m.role;
        mj["content"] = m.content;
        hist.push_back(mj);
    }
    j["history"] = hist;

    m_log->persist(conv.log_file, j.dump(2));
}

/**
 * 启动恢复 — 扫描 logs/ 目录，把所有 .json 文件加载回内存
 * JSON 格式错误的文件会被静默跳过 (catch-all)
 */
void ConversationManager::load_all() {
    if (!fs::exists(g_log_dir)) return;

    for (auto& entry : fs::directory_iterator(g_log_dir)) {
        if (!entry.is_regular_file()) continue;
        auto path = entry.path();
        if (path.extension() != ".json") continue;

        std::ifstream f(path);
        if (!f) continue;
        try {
            auto j = nlohmann::json::parse(f);
            auto conv = std::make_shared<Conversation>();
            conv->id = j.value("id", "");
            conv->title = j.value("title", "未命名");
            conv->log_file = path.string();
            conv->created_at = j.value("created_at", "");

            if (j.contains("history") && j["history"].is_array()) {
                for (auto& mj : j["history"]) {
                    llm::Message m;
                    m.role = mj.value("role", "user");
                    m.content = mj.value("content", "");
                    conv->history.push_back(m);
                }
            }

            if (!conv->id.empty()) {
                m_convs[conv->id] = conv;
                LOG_INFO("Loaded conversation: %s (%zu messages)",
                         conv->id.c_str(), conv->history.size());
            }
        } catch (...) {}  // JSON 损坏或格式不兼容 → 静默跳过
    }
}

} // namespace server
