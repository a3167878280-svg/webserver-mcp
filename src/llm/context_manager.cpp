/**
 * 上下文管理器实现
 *
 * Token 估算基于 Unicode 范围判断，结合中英文的粗略系数。
 * 这不是精确 tokenizer，而是启发式估算 — 用作截断/摘要的判断依据。
 */

#include "context_manager.h"
#include <algorithm>
#include <cstring>

namespace llm {
namespace context {

// ── CJK 范围判断 ──────────────────────────────────────────

static bool is_cjk(uint32_t cp) {
    return (cp >= 0x4E00  && cp <= 0x9FFF)  ||  // CJK Unified Ideographs
           (cp >= 0x3400  && cp <= 0x4DBF)  ||  // CJK Unified Ext-A
           (cp >= 0x3000  && cp <= 0x303F)  ||  // CJK Symbols/Punctuation
           (cp >= 0xFF00  && cp <= 0xFFEF)  ||  // Halfwidth/Fullwidth Forms
           (cp >= 0xAC00  && cp <= 0xD7AF)  ||  // Hangul Syllables
           (cp >= 0x3040  && cp <= 0x30FF)  ||  // Hiragana + Katakana
           (cp >= 0x2E80  && cp <= 0x2FDF)  ||  // CJK Radicals
           (cp >= 0x20000 && cp <= 0x2A6DF) ||  // CJK Unified Ext-B
           (cp >= 0xF900  && cp <= 0xFAFF);      // CJK Compatibility
}

// ── Token 估算 ────────────────────────────────────────────

size_t estimate_tokens(std::string_view text) {
    double tokens = 0.0;
    const char* p = text.data();
    const char* end = p + text.size();

    while (p < end) {
        uint32_t cp;
        int len = 0;

        // 解码 UTF-8
        unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else {
            // 无效 UTF-8 起始字节，跳过
            p++;
            tokens += 0.25;
            continue;
        }

        // 检查越界
        if (p + len > end) {
            p++;
            tokens += 0.25;
            continue;
        }

        // 补全多字节序列的后续字节
        for (int i = 1; i < len; i++) {
            cp = (cp << 6) | (static_cast<unsigned char>(p[i]) & 0x3F);
        }

        tokens += is_cjk(cp) ? CJK_TOKENS_PER_CHAR : ASCII_TOKENS_PER_CHAR;
        p += len;
    }

    return static_cast<size_t>(tokens + 0.5);  // 四舍五入
}

size_t estimate_tokens(const std::vector<Message>& messages) {
    double total = 0.0;
    for (const auto& msg : messages) {
        // 每条消息的基础开销: role + JSON 格式化字段
        total += 4.0;
        total += static_cast<double>(estimate_tokens(msg.content));

        // tool_call_id 开销
        if (!msg.tool_call_id.empty()) {
            total += 4.0 + static_cast<double>(estimate_tokens(msg.tool_call_id));
        }

        // tool_calls JSON 开销：用序列化后的字符串估算
        if (!msg.tool_calls.is_null()) {
            std::string tc_str = msg.tool_calls.dump();
            total += static_cast<double>(estimate_tokens(tc_str));
        }
    }
    return static_cast<size_t>(total + 0.5);
}

// ── 模型预算表 ────────────────────────────────────────────

struct ModelBudgetEntry {
    const char* prefix;
    size_t budget;
};

static const ModelBudgetEntry BUDGET_TABLE[] = {
    {"qwen",     28000},    // Qwen3/Qwen2.5 系列, 32K 窗口
    {"gpt-4o",  100000},    // GPT-4o, 128K 窗口
    {"gpt-4",   100000},
    {"gpt-3.5",  12000},    // GPT-3.5, 16K 窗口
    {"claude",  180000},    // Claude 系列, 200K 窗口
    {"deepseek",120000},    // DeepSeek 系列, 128K 窗口
    {"llama",    28000},    // 本地 Llama 模型, 通常 32K
    {"qwen2",    28000},
    {"gpt-5",   100000},
    {"o1",      100000},
    {"o3",      100000},
    {"o4",      100000},
    {"sonnet",  180000},
    {"opus",    180000},
    {"haiku",   180000},
};

size_t context_budget(const std::string& model_name) {
    // 大小写不敏感前缀匹配
    std::string lower = model_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    for (const auto& entry : BUDGET_TABLE) {
        std::string prefix(entry.prefix);
        std::transform(prefix.begin(), prefix.end(), prefix.begin(), ::tolower);
        if (lower.find(prefix) == 0) {
            return entry.budget;
        }
    }
    return DEFAULT_BUDGET;
}

// ── 工具结果截断 ──────────────────────────────────────────

std::string truncate_tool_result(const std::string& text, size_t max_tokens) {
    if (text.empty() || max_tokens == 0) return text;

    double token_count = 0.0;
    const char* p = text.data();
    const char* end = p + text.size();
    const char* cut = p;  // 截断位置

    while (p < end) {
        const char* char_start = p;
        uint32_t cp;
        int len = 0;

        unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else {
            // 无效字节，跳过
            p++;
            token_count += 0.25;
            if (token_count > static_cast<double>(max_tokens)) break;
            cut = p;
            continue;
        }

        if (p + len > end) {
            p++;
            token_count += 0.25;
            if (token_count > static_cast<double>(max_tokens)) break;
            cut = p;
            continue;
        }

        // 补全多字节序列
        for (int i = 1; i < len; i++) {
            cp = (cp << 6) | (static_cast<unsigned char>(p[i]) & 0x3F);
        }

        double inc = is_cjk(cp) ? CJK_TOKENS_PER_CHAR : ASCII_TOKENS_PER_CHAR;
        p += len;

        if (token_count + inc > static_cast<double>(max_tokens)) {
            break;
        }
        token_count += inc;
        cut = p;
    }

    // 如果整个文本都在预算内，直接返回
    if (cut >= end) return text;

    // 截断并追加标记
    std::string result(text.data(), static_cast<size_t>(cut - text.data()));
    result += "\n\n[... truncated, original: ";
    result += std::to_string(text.size());
    result += " chars, ~";
    result += std::to_string(estimate_tokens(text));
    result += " tokens]";

    return result;
}

// ── 截断判断 ──────────────────────────────────────────────

bool needs_truncation(const std::vector<Message>& messages,
                      const std::string& model_name) {
    size_t budget = context_budget(model_name);
    size_t usage = estimate_tokens(messages);
    return usage > (budget * HEADROOM_PCT / 100);
}

} // namespace context
} // namespace llm
