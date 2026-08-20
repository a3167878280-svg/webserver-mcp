/**
 * 上下文管理器 — Token 估算 + 智能截断 + 预算管理
 *
 * ═══════════ 为什么需要这个? ═══════════
 *
 *   长对话中 messages 数组无限增长，小模型（qwen3-4b 32K）窗口很快不够用。
 *   以前按轮数截断（5/10/20轮）太粗糙 — 一轮工具调用可能产生几千字的输出。
 *
 *   这个模块提供:
 *     1. 基于字符类型的 token 估算（中英文分开算，不依赖 tokenizer）
 *     2. 已知模型的上下文预算查询
 *     3. 长工具结果的截断
 *     4. 是否需要截断的判断
 *
 * ═══════════ Token 估算策略 ═══════════
 *
 *   这不是精确 tokenizer，而是启发式估算。目的是做出截断/摘要决策，
 *   不需要精确到个位数。误差在 ±30% 内就足够好了。
 *
 *   规则:
 *     CJK 字符 (中文/日文/韩文) — 约 1 token/字
 *     ASCII/拉丁字符           — 约 0.25 token/字 (4 字/token)
 *
 *   这符合大多数 LLM tokenizer (BPE/SentencePiece) 的行为:
 *   大部分中文常见字是独立 token，英文词被拆成子词。
 */

#pragma once

#include "llm_client.h"
#include <string>
#include <string_view>
#include <vector>
#include <cstddef>

namespace llm {
namespace context {

// ── 预算常量 ──
constexpr double CJK_TOKENS_PER_CHAR   = 1.0;
constexpr double ASCII_TOKENS_PER_CHAR = 0.25;
constexpr size_t DEFAULT_BUDGET        = 28000;  // 32K 窗口预留 4K 给输出
constexpr size_t HEADROOM_PCT          = 70;     // 超出 70% 预算就触发截断/摘要

/**
 * 估算单段文本的 token 数
 *
 * CJK 范围: U+4E00-U+9FFF (CJK Unified), U+3400-U+4DBF (CJK Ext-A),
 *           U+3000-U+303F (CJK 标点), U+FF00-U+FFEF (全角),
 *           U+AC00-U+D7AF (韩文), U+3040-U+30FF (日文假名)
 */
size_t estimate_tokens(std::string_view text);

/// 估算整个 messages 数组的 token 数（含 role/formatting 开销）
size_t estimate_tokens(const std::vector<Message>& messages);

/**
 * 查询模型的上下文预算（输入端上限，已预留输出空间）
 *
 * 通过模型名前缀匹配:
 *   qwen*    → 28K   (32K 窗口)
 *   gpt-4*   → 100K  (128K 窗口)
 *   claude*   → 180K  (200K 窗口)
 *   deepseek* → 120K
 *   llama*    → 28K
 *   其他       → 28K (保守默认)
 */
size_t context_budget(const std::string& model_name);

/**
 * 截断过长的工具结果
 *
 * 按 token 预算截断文本，末尾追加截断标记。
 * 如果文本本身不超预算则原样返回。
 */
std::string truncate_tool_result(const std::string& text, size_t max_tokens);

/// 判断 messages 是否超出模型预算的 70%，需要干预
bool needs_truncation(const std::vector<Message>& messages,
                      const std::string& model_name);

} // namespace context
} // namespace llm
