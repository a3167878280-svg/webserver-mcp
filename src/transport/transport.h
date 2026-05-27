#pragma once

#include <functional>
#include <string>

namespace transport {

class Transport {
public:
    virtual ~Transport() = default;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void send(const std::string& json_message) = 0;

    using MessageCallback = std::function<void(const std::string&)>;
    virtual void set_on_message(MessageCallback callback) = 0;
};

} // namespace transport
