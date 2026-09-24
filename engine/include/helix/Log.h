// Prosty, bezpieczny wątkowo logger. NIE wolno go wołać z wątku audio.
#pragma once

#include <mutex>
#include <string>
#include <string_view>

namespace helix {

enum class LogLevel : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };

/// Logger pisze na stderr i (opcjonalnie) do pliku. Świadomie NIE jest
/// realtime-safe — wątek audio nie ma prawa go dotykać (spec §22).
class Log {
public:
    static void setLevel(LogLevel level) noexcept;
    static LogLevel level() noexcept;

    /// Włącza duplikowanie logów do pliku. Pusty path wyłącza.
    static bool setFile(const std::string& path);

    static void write(LogLevel level, std::string_view component, std::string_view message);

    static void trace(std::string_view c, std::string_view m) { write(LogLevel::Trace, c, m); }
    static void debug(std::string_view c, std::string_view m) { write(LogLevel::Debug, c, m); }
    static void info (std::string_view c, std::string_view m) { write(LogLevel::Info,  c, m); }
    static void warn (std::string_view c, std::string_view m) { write(LogLevel::Warn,  c, m); }
    static void error(std::string_view c, std::string_view m) { write(LogLevel::Error, c, m); }

    static const char* levelName(LogLevel level) noexcept;
    static bool parseLevel(std::string_view text, LogLevel& out) noexcept;
};

} // namespace helix
