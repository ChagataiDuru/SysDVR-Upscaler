#pragma once

#include "app/AppConfig.h"

#include <mutex>
#include <string_view>

namespace ns60 {

class Log final {
public:
    static void setLevel(LogLevel level) noexcept;
    static void write(LogLevel level, std::string_view message);
    static void trace(std::string_view message) { write(LogLevel::Trace, message); }
    static void debug(std::string_view message) { write(LogLevel::Debug, message); }
    static void info(std::string_view message) { write(LogLevel::Info, message); }
    static void warning(std::string_view message) { write(LogLevel::Warning, message); }
    static void error(std::string_view message) { write(LogLevel::Error, message); }

private:
    static LogLevel level_;
    static std::mutex mutex_;
};

} // namespace ns60

