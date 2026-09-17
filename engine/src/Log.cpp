#include "helix/Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace helix {

namespace {

std::atomic<int>& levelStorage() {
    static std::atomic<int> level{static_cast<int>(LogLevel::Info)};
    return level;
}

std::mutex& sinkMutex() {
    static std::mutex mutex;
    return mutex;
}

std::ofstream& fileSink() {
    static std::ofstream stream;
    return stream;
}

std::string timestamp() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto time = clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()).count() % 1000;

    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    std::ostringstream out;
    out << std::put_time(&tm, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << millis;
    return out.str();
}

} // namespace

void Log::setLevel(LogLevel level) noexcept {
    levelStorage().store(static_cast<int>(level), std::memory_order_relaxed);
}

LogLevel Log::level() noexcept {
    return static_cast<LogLevel>(levelStorage().load(std::memory_order_relaxed));
}

bool Log::setFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(sinkMutex());
    auto& stream = fileSink();
    if (stream.is_open()) stream.close();
    if (path.empty()) return true;
    stream.open(path, std::ios::app);
    return stream.is_open();
}

const char* Log::levelName(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Off:   return "OFF";
    }
    return "INFO";
}

bool Log::parseLevel(std::string_view text, LogLevel& out) noexcept {
    if (text == "trace") { out = LogLevel::Trace; return true; }
    if (text == "debug") { out = LogLevel::Debug; return true; }
    if (text == "info")  { out = LogLevel::Info;  return true; }
    if (text == "warn")  { out = LogLevel::Warn;  return true; }
    if (text == "error") { out = LogLevel::Error; return true; }
    if (text == "off")   { out = LogLevel::Off;   return true; }
    return false;
}

void Log::write(LogLevel level, std::string_view component, std::string_view message) {
    if (static_cast<int>(level) < levelStorage().load(std::memory_order_relaxed)) return;

    std::ostringstream line;
    line << timestamp() << " [" << levelName(level) << "] " << component << ": " << message;
    const std::string text = line.str();

    std::lock_guard<std::mutex> lock(sinkMutex());
    std::fputs(text.c_str(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);

    auto& stream = fileSink();
    if (stream.is_open()) {
        stream << text << '\n';
        stream.flush();
    }
}

} // namespace helix
