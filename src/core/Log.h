// Ember3D — logging.
//
// Owner: Ash · Lead.  Contract: ARCHITECTURE.md §2 (EMBER_LOG_* macros).
//
// Header-only, no dependencies beyond the C++ standard library: every module
// can log without linking anything, and `core` stays at the bottom of the
// graph. Warn/Error go to stderr, the rest to stdout.
//
//   EMBER_LOG_INFO("created %s", name.c_str());
//   EMBER_LOG_ERROR("shader link failed: %s", log.c_str());
//   EMBER_ASSERT(index < count, "index %u out of range", index);
//
// Messages are printf-style. A message is only formatted if its level is
// enabled, so an unused debug log costs one comparison.

#pragma once

#include <cstdarg>
#include <cstdio>

#if defined(_MSC_VER)
  #define EMBER_DEBUGBREAK() __debugbreak()
#else
  #include <csignal>
  #define EMBER_DEBUGBREAK() raise(SIGTRAP)
#endif

namespace ember::log {

enum class Level : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };

inline Level& levelRef() {
  static Level s_level = Level::Info;
  return s_level;
}

inline Level level() { return levelRef(); }
inline void  setLevel(Level l) { levelRef() = l; }
inline bool  enabled(Level l) { return static_cast<int>(l) >= static_cast<int>(levelRef()); }

inline const char* levelName(Level l) {
  switch (l) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info:  return "INFO ";
    case Level::Warn:  return "WARN ";
    case Level::Error: return "ERROR";
    default:           return "OFF  ";
  }
}

// Never call this directly; use the EMBER_LOG_* macros so file/line/level are filled in.
inline void write(Level l, const char* file, int line, const char* fmt, ...) {
  if (!enabled(l)) return;

  std::FILE* out = (static_cast<int>(l) >= static_cast<int>(Level::Warn)) ? stderr : stdout;

  std::fprintf(out, "[%s] %s:%d: ", levelName(l), file ? file : "?", line);
  std::va_list args;
  va_start(args, fmt);
  std::vfprintf(out, fmt, args);
  va_end(args);
  std::fputc('\n', out);
  std::fflush(out);
}

} // namespace ember::log

#define EMBER_LOG_TRACE(...) ::ember::log::write(::ember::log::Level::Trace, __FILE__, __LINE__, __VA_ARGS__)
#define EMBER_LOG_DEBUG(...) ::ember::log::write(::ember::log::Level::Debug, __FILE__, __LINE__, __VA_ARGS__)
#define EMBER_LOG_INFO(...)  ::ember::log::write(::ember::log::Level::Info,  __FILE__, __LINE__, __VA_ARGS__)
#define EMBER_LOG_WARN(...)  ::ember::log::write(::ember::log::Level::Warn,  __FILE__, __LINE__, __VA_ARGS__)
#define EMBER_LOG_ERROR(...) ::ember::log::write(::ember::log::Level::Error, __FILE__, __LINE__, __VA_ARGS__)

// Assertions are always active: a broken invariant in a debug renderer is
// still a broken invariant in a release build.
//
//   EMBER_ASSERT(index < count);                            // just the condition
//   EMBER_ASSERT(index < count, "index %u out of range", index);   // printf-style detail
#define EMBER_ASSERT(cond, ...)                                                     \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      ::ember::log::write(::ember::log::Level::Error, __FILE__, __LINE__,           \
                          "assertion failed: " #cond __VA_OPT__(": ") __VA_ARGS__); \
      EMBER_DEBUGBREAK();                                                           \
    }                                                                               \
  } while (false)
