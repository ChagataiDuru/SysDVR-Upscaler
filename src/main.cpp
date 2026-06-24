#include "app/AppConfig.h"
#include "app/ApplicationEntry.h"
#include "utility/Log.h"

#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef NS60_VERSION
#define NS60_VERSION "development"
#endif
#ifndef NS60_DEFAULT_VALIDATION
#define NS60_DEFAULT_VALIDATION 0
#endif

namespace {
int run(const std::vector<std::string>& arguments) {
    try {
        const auto parsed = ns60::parseCommandLine(arguments, NS60_DEFAULT_VALIDATION != 0);
        if (parsed.action == ns60::ParseAction::Help) { std::cout << ns60::commandLineHelp(); return 0; }
        if (parsed.action == ns60::ParseAction::Version) { std::cout << "NexusStream60 " NS60_VERSION "\n"; return 0; }
        if (!parsed.config) {
            std::cerr << "Error: " << parsed.error << "\n\n" << ns60::commandLineHelp();
            return 2;
        }
        ns60::Log::setLevel(parsed.config->logLevel);
        return ns60::runApplication(std::move(*parsed.config));
    } catch (const std::exception& error) {
        ns60::Log::write(ns60::LogLevel::Critical, error.what());
        return 1;
    }
}

#ifdef _WIN32
std::string utf8(const wchar_t* text) {
    if (!text) return {};
    const int wideLength = static_cast<int>(std::char_traits<wchar_t>::length(text));
    if (wideLength == 0) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, text, wideLength, nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, wideLength, result.data(), required, nullptr, nullptr);
    return result;
}
#endif
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) arguments.push_back(utf8(argv[i]));
    return run(arguments);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) arguments.emplace_back(argv[i]);
    return run(arguments);
}
#endif

