#include "utility/Log.h"

#include <chrono>
#include <iomanip>
#include <iostream>

namespace ns60 {
LogLevel Log::level_ = LogLevel::Info;
std::mutex Log::mutex_;

void Log::setLevel(LogLevel level) noexcept { level_ = level; }

void Log::write(LogLevel level, std::string_view message) {
    if (static_cast<int>(level) < static_cast<int>(level_)) return;
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::lock_guard lock(mutex_);
    std::clog << '[' << std::put_time(&local, "%H:%M:%S") << "] [" << toString(level) << "] " << message << '\n';
}
} // namespace ns60

