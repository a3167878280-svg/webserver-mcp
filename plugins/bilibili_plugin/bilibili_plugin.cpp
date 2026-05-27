#include "bilibili_plugin.h"
#include "httplib.h"
#include <sstream>
#include <regex>

static mcp::ToolCallResult make_error(const std::string& msg) {
    mcp::ToolCallResult r;
    r.isError = true;
    r.content.push_back(mcp::TextContent{"text", msg});
    return r;
}
static mcp::ToolCallResult make_result(const std::string& text) {
    mcp::ToolCallResult r;
    r.isError = false;
    r.content.push_back(mcp::TextContent{"text", text});
    return r;
}

std::string BilibiliPlugin::http_get(const std::string& url) {
    std::string u = url;
    std::string host, path;
    if (u.find("https://") == 0) u = u.substr(8);
    size_t slash = u.find('/');
    if (slash != std::string::npos) { host = u.substr(0, slash); path = u.substr(slash); }
    else { host = u; path = "/"; }

    httplib::SSLClient cli(host);
    cli.set_connection_timeout(8);
    cli.set_read_timeout(10);
    cli.enable_server_certificate_verification(false);
    httplib::Headers hdrs = {
        {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"},
        {"Referer", "https://www.bilibili.com"}
    };
    auto res = cli.Get(path, hdrs);
    if (!res || res->status != 200) return "";
    return res->body;
}

std::vector<mcp::ToolDef> BilibiliPlugin::get_tools() const {
    mcp::ToolDef t1;
    t1.name = "bilibili_up_videos";
    t1.description = "获取 B站 UP主的最新视频列表 (标题、播放量、链接)";
    t1.inputSchema = {
        {"type", "object"},
        {"properties", {
            {"mid", {{"type", "string"}, {"description", "UP主的用户ID (mid)，从UP主空间URL获取，如 https://space.bilibili.com/455982642 中的 455982642"}}}
        }},
        {"required", {"mid"}}
    };

    mcp::ToolDef t2;
    t2.name = "bilibili_up_info";
    t2.description = "获取 B站 UP主的基本信息 (昵称、签名、粉丝数、关注数)";
    t2.inputSchema = {
        {"type", "object"},
        {"properties", {
            {"mid", {{"type", "string"}, {"description", "UP主的用户ID"}}}
        }},
        {"required", {"mid"}}
    };

    mcp::ToolDef t3;
    t3.name = "bilibili_hot";
    t3.description = "获取 B站当前热门视频列表 (综合热门)";
    t3.inputSchema = {
        {"type", "object"},
        {"properties", nlohmann::json::object()},
        {"required", nlohmann::json::array()}
    };

    return {t1, t2, t3};
}

mcp::ToolCallResult BilibiliPlugin::call_tool(const std::string& name,
                                                const nlohmann::json& args) {
    if (name == "bilibili_up_videos") return handle_up_videos(args);
    if (name == "bilibili_up_info")   return handle_up_info(args);
    if (name == "bilibili_hot")       return handle_hot(args);
    return make_error("Unknown tool: " + name);
}

mcp::ToolCallResult BilibiliPlugin::handle_up_videos(const nlohmann::json& args) {
    std::string mid = args.value("mid", "");
    if (mid.empty()) return make_error("Missing parameter: mid (UP主用户ID)");

    std::string url = "https://api.bilibili.com/x/space/wbi/arc/search?mid=" + mid + "&ps=10&tid=0&pn=1&order=pubdate";
    std::string data = http_get(url);
    if (data.empty()) return make_error("Failed to fetch B站 API");

    try {
        auto j = nlohmann::json::parse(data);
        if (j.value("code", -1) != 0)
            return make_error("B站 API error: " + j.value("message", "unknown"));

        auto& list = j["data"]["list"]["vlist"];
        if (!list.is_array() || list.empty())
            return make_result("该UP主暂无视频");

        std::ostringstream oss;
        oss << "UP主 (mid=" << mid << ") 最新视频:\n\n";
        int count = 0;
        for (auto& v : list) {
            if (count++ >= 10) break;
            std::string bvid = v.value("bvid", "");
            std::string title = v.value("title", "");
            int play = v.value("play", 0);
            int comment = v.value("comment", 0);
            std::string created = std::to_string(v.value("created", 0));
            oss << count << ". " << title << "\n";
            oss << "   播放: " << play << "  评论: " << comment << "\n";
            oss << "   https://www.bilibili.com/video/" << bvid << "\n\n";
        }
        return make_result(oss.str());
    } catch (...) {
        return make_error("Failed to parse B站 response");
    }
}

mcp::ToolCallResult BilibiliPlugin::handle_up_info(const nlohmann::json& args) {
    std::string mid = args.value("mid", "");
    if (mid.empty()) return make_error("Missing parameter: mid");

    std::string url = "https://api.bilibili.com/x/space/acc/info?mid=" + mid;
    std::string data = http_get(url);
    if (data.empty()) return make_error("Failed to fetch B站 API");

    try {
        auto j = nlohmann::json::parse(data);
        if (j.value("code", -1) != 0)
            return make_error("B站 API error: " + j.value("message", "unknown"));

        auto& d = j["data"];
        std::ostringstream oss;
        oss << "=== UP主信息 ===\n";
        oss << "昵称: " << d.value("name", "N/A") << "\n";
        oss << "性别: " << d.value("sex", "N/A") << "\n";
        oss << "签名: " << d.value("sign", "N/A") << "\n";
        oss << "等级: LV" << d.value("level", 0) << "\n";
        oss << "粉丝: " << d.value("follower", 0) << "\n";
        oss << "关注: " << d.value("following", 0) << "\n";
        oss << "主页: https://space.bilibili.com/" << mid << "\n";
        return make_result(oss.str());
    } catch (...) {
        return make_error("Failed to parse B站 response");
    }
}

mcp::ToolCallResult BilibiliPlugin::handle_hot(const nlohmann::json& /*args*/) {
    std::string url = "https://api.bilibili.com/x/web-interface/popular?ps=20";
    std::string data = http_get(url);
    if (data.empty()) return make_error("Failed to fetch B站 hot list");

    try {
        auto j = nlohmann::json::parse(data);
        if (j.value("code", -1) != 0)
            return make_error("B站 API error: " + j.value("message", "unknown"));

        auto& list = j["data"]["list"];
        if (!list.is_array()) return make_error("Unexpected API response");

        std::ostringstream oss;
        oss << "=== B站热门视频 ===\n\n";
        int count = 0;
        for (auto& v : list) {
            if (count++ >= 20) break;
            std::string bvid = v.value("bvid", "");
            std::string title = v.value("title", "");
            std::string owner = v["owner"].value("name", "unknown");
            int play = v["stat"].value("view", 0);
            int danmu = v["stat"].value("danmaku", 0);
            oss << count << ". " << title << " (UP: " << owner << ")\n";
            oss << "   播放: " << play << "  弹幕: " << danmu << "\n";
            oss << "   https://www.bilibili.com/video/" << bvid << "\n\n";
        }
        return make_result(oss.str());
    } catch (...) {
        return make_error("Failed to parse B站 response");
    }
}

extern "C" plugin::IPlugin* create_plugin() { return new BilibiliPlugin(); }
extern "C" void destroy_plugin(plugin::IPlugin* p) { delete p; }
