/**
 * 对话管理器 — 多轮对话的持久化存储
 *
 * ═══════════ 为什么需要这个? ═══════════
 *
 *   HTTP 是无状态的。浏览器刷新后，之前的对话就丢了。
 *   LLM 调用需要完整的历史上下文才能理解"刚才聊到哪了"。
 *
 *   ConversationManager 做的事:
 *     1. 每个对话分配一个唯一 ID
 *     2. 每轮用户消息 + LLM 回复 + 工具调用都追加到历史
 *     3. 每追加一条就写 JSON 文件 (logs/conv_<id>.json)
 *     4. 启动时加载所有 JSON 文件恢复历史
 *     5. 前端通过 /api/conversations 接口管理对话
 *
 * ═══════════ 存储格式 (logs/conv_abc123.json) ═══════════
 *
 *   {
 *     "id": "abc123",
 *     "title": "新对话",
 *     "created_at": "2026-05-30 14:30",
 *     "history": [
 *       {"role":"user", "content":"北京天气怎么样?"},
 *       {"role":"tool", "content":"{\"content\":[{\"text\":\"Beijing: Sunny, 15°C\"}]}"},
 *       {"role":"assistant", "content":"北京今天晴朗，15°C"}
 *     ]
 *   }
 *
 * ═══════════ 线程安全 ═══════════
 *
 *   httplib 多线程处理 HTTP 请求 → 多个请求可能同时操作同一对话
 *   → 全局 m_mutex 保护 m_convs map
 *   → 每对话 mtx 保护单个 conversation 的 history
 */

#pragma once

#include "../llm/llm_client.h"
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <nlohmann/json.hpp>

class Log;  // 前向声明，避免循环依赖

namespace server {

class ConversationManager {
public:
    // 注入异步日志系统，用于对话持久化 (异步落盘)
    explicit ConversationManager(Log* log);
    ~ConversationManager() = default;

    // 给前端的对话摘要 (不含历史内容，只含元信息)
    struct ConvInfo {
        std::string id;
        std::string title;
        std::string log_file;
        std::string created_at;
    };

    // 创建新对话 → 返回 ConvInfo (含生成的 UUID 和创建时间)
    ConvInfo create(const std::string& title = "新对话");

    // 删除对话 → 同时删除 JSON 文件
    bool remove(const std::string& id);

    // 列出所有对话 → 前端显示对话列表
    std::vector<ConvInfo> list() const;

    // 追加一条消息到对话历史 → 同时写文件持久化
    void append_history(const std::string& id, const llm::Message& msg);

    // 获取对话的完整历史 → 喂给 LLM 作为上下文
    std::vector<llm::Message> get_history(const std::string& id) const;

    bool exists(const std::string& id) const;

    void set_title(const std::string& id, const std::string& title);

private:
    struct Conversation {
        std::string id;
        std::string title;
        std::string log_file;                        // 对应 JSON 文件路径
        std::string created_at;
        std::vector<llm::Message> history;           // 完整消息历史
        mutable std::mutex mtx;                      // 保护此对话的 history
    };

    mutable std::mutex m_mutex;                      // 保护 m_convs map
    std::unordered_map<std::string, std::shared_ptr<Conversation>> m_convs;
    Log* m_log;                                      // 异步日志系统 (生命周期由 main 管理)

    void save_to_file(const Conversation& conv);     // 通过 Log::persist() 异步落盘
    void load_all();                                  // 启动时从文件恢复
    static std::string gen_id();                      // 16 位十六进制随机 ID
    static std::string now_str();                     // 当前时间字符串
};

} // namespace server
