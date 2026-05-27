#include "weather_plugin.h"
#include "httplib.h"
#include <sstream>
#include <algorithm>

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

std::vector<mcp::ToolDef> WeatherPlugin::get_tools() const {
    mcp::ToolDef t;
    t.name = "query_weather";
    t.description = "查询指定城市的当前天气信息 (温度、天气状况、湿度、风速等)";
    t.inputSchema = {
        {"type", "object"},
        {"properties", {
            {"city", {
                {"type", "string"},
                {"description", "城市名称，支持中文或英文，如: Beijing, 北京, London, Tokyo"}
            }}
        }},
        {"required", {"city"}}
    };
    return {t};
}

mcp::ToolCallResult WeatherPlugin::call_tool(const std::string& name,
                                               const nlohmann::json& args) {
    if (name == "query_weather") return handle_query_weather(args);
    return make_error("Unknown tool: " + name);
}

std::string WeatherPlugin::http_get(const std::string& url) {
    // 解析 URL
    std::string host, path;
    std::string u = url;
    if (u.find("http://") == 0) u = u.substr(7);
    else if (u.find("https://") == 0) u = u.substr(8);

    size_t slash = u.find('/');
    if (slash != std::string::npos) {
        host = u.substr(0, slash);
        path = u.substr(slash);
    } else {
        host = u;
        path = "/";
    }

    httplib::Client cli(host);
    cli.set_connection_timeout(8);
    cli.set_read_timeout(10);

    auto res = cli.Get(path);
    if (!res || res->status != 200) {
        return "Failed to fetch weather data";
    }
    return res->body;
}

mcp::ToolCallResult WeatherPlugin::handle_query_weather(const nlohmann::json& args) {
    std::string city = args.value("city", "");
    if (city.empty()) return make_error("Missing required parameter: city");

    // wttr.in API: 免费天气服务
    // format=4: 简短一行 "City: Weather, Temp"
    // format=j1: 完整 JSON (数据量大，LLM 处理慢)
    std::string url = "http://wttr.in/" + city + "?format=4&m";
    std::string data = http_get(url);
    if (data.empty() || data.find("Failed") == 0) {
        url = "http://wttr.in/" + city + "?format=j1";
        data = http_get(url);
        // 从 JSON 提取摘要
        if (!data.empty() && data.find("Failed") != 0) {
            try {
                auto j = nlohmann::json::parse(data);
                auto& cc = j["current_condition"][0];
                std::ostringstream oss;
                oss << j["nearest_area"][0]["areaName"][0]["value"].get<std::string>()
                    << ": " << cc["weatherDesc"][0]["value"].get<std::string>()
                    << ", " << cc["temp_C"].get<std::string>() << "C"
                    << ", Humidity: " << cc["humidity"].get<std::string>() << "%"
                    << ", Wind: " << cc["windspeedKmph"].get<std::string>() << "km/h";
                data = oss.str();
            } catch (...) {}
        }
    }

    return make_result(data);
}

extern "C" plugin::IPlugin* create_plugin() { return new WeatherPlugin(); }
extern "C" void destroy_plugin(plugin::IPlugin* p) { delete p; }
