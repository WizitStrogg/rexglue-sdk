/**
 * @file        core/logging.cpp
 * @brief       Logging infrastructure implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <spdlog/details/os.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/platform/env.h>
#include <rex/thread.h>

#if REX_PLATFORM_WIN32
#include <spdlog/sinks/msvc_sink.h>
#else
#include <spdlog/sinks/stdout_sinks.h>
#endif

REXCVAR_DEFINE_STRING(log_level, "info", "Log",
                      "Global log level: trace, debug, info, warn, error, critical, off")
    .allowed({"trace", "debug", "info", "warn", "error", "critical", "off"});

REXCVAR_DEFINE_STRING(log_file, "", "Log", "Log file path (empty = auto sequential naming)");

REXCVAR_DEFINE_BOOL(log_verbose, false, "Log", "Enable verbose logging (sets level to trace)")
    .debug_only();

REXCVAR_DEFINE_BOOL(log_noisy, false, "Log", "Enable noisy/high-frequency log macros");

REXCVAR_DEFINE_INT32(log_flush_interval, 0, "Log", "Periodic flush interval in seconds (0 = off)")
    .range(0, 60)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(log_file_async, true, "Log",
                    "Write the log file from a background thread, so threads that log (GPU "
                    "commands, presenter, game) never wait on the disk. Error lines and "
                    "explicit flushes still reach the file before the call returns.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex {

namespace {

// File sink that keeps disk writes off the threads that log. Lines are
// formatted into memory and a writer thread appends them to the file, so one
// slow write (antivirus scan, busy disk) can't stall the GPU command thread
// or the presenter for hundreds of milliseconds. Error and critical lines,
// FlushSync and destruction write everything queued before returning; a
// regular flush (flush_on level, flush_every) only wakes the writer.
class AsyncFileSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  explicit AsyncFileSink(const spdlog::filename_t& filename) {
    spdlog::details::os::create_dir(spdlog::details::os::dir_name(filename));
    if (spdlog::details::os::fopen_s(&file_, filename, SPDLOG_FILENAME_T("ab"))) {
      spdlog::throw_spdlog_ex(
          "Failed opening file " + spdlog::details::os::filename_to_str(filename), errno);
    }
    writer_ = std::thread([this] { WriterLoop(); });
  }

  ~AsyncFileSink() override {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    wake_cv_.notify_one();
    writer_.join();
    std::fclose(file_);
  }

  // Writes everything logged so far, including a write already in flight.
  void FlushSync() {
    std::lock_guard lock(mutex_);
    WritePendingLocked();
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    pending_.append(formatted.data(), formatted.size());
    if (msg.level >= spdlog::level::err || pending_.size() >= kMaxPendingBytes) {
      // Errors often come right before a crash; also bound memory if the
      // disk stalls for a long time.
      WritePendingLocked();
    } else if (pending_.size() >= kWakeBytes) {
      WakeWriter();
    }
  }

  void flush_() override {
    if (!pending_.empty())
      WakeWriter();
  }

 private:
  static constexpr size_t kWakeBytes = 64 * 1024;
  static constexpr size_t kMaxPendingBytes = 8 * 1024 * 1024;
  static constexpr std::chrono::milliseconds kWriteInterval{100};

  void WakeWriter() {
    wake_ = true;
    wake_cv_.notify_one();
  }

  // With mutex_ held. Lock order is mutex_ then io_mutex_ everywhere, and the
  // writer takes io_mutex_ before releasing mutex_, so file order matches
  // logging order.
  void WritePendingLocked() {
    std::lock_guard io(io_mutex_);
    WriteToFile(pending_);
    pending_.clear();
  }

  void WriteToFile(const std::string& data) {
    if (data.empty())
      return;
    std::fwrite(data.data(), 1, data.size(), file_);
    std::fflush(file_);
  }

  void WriterLoop() {
    rex::thread::set_current_thread_name("Log Writer");
    std::unique_lock lock(mutex_);
    while (true) {
      wake_cv_.wait_for(lock, kWriteInterval, [this] { return stop_ || wake_; });
      wake_ = false;
      if (!pending_.empty()) {
        writing_.swap(pending_);
        std::unique_lock io(io_mutex_);
        lock.unlock();
        WriteToFile(writing_);
        writing_.clear();
        io.unlock();
        lock.lock();
      }
      if (stop_ && pending_.empty())
        return;
    }
  }

  std::FILE* file_ = nullptr;
  std::string pending_;  // Guarded by mutex_.
  std::string writing_;  // Writer thread only.
  std::mutex io_mutex_;  // Held while writing to file_.
  std::condition_variable wake_cv_;
  bool wake_ = false;  // Guarded by mutex_.
  bool stop_ = false;  // Guarded by mutex_.
  std::thread writer_;
};

std::vector<LogCategoryEntry> g_registry;
spdlog::sink_ptr g_console_sink;
spdlog::sink_ptr g_file_sink;
std::shared_ptr<AsyncFileSink> g_async_file_sink;  // g_file_sink when it's async.
spdlog::sink_ptr g_early_sink;
std::vector<spdlog::sink_ptr> g_extra_sinks;
bool g_early_initialized = false;
bool g_initialized = false;
std::mutex g_mutex;
LogConfig g_config;

std::optional<int> RunNumber(const std::filesystem::path& file, std::string_view prefix) {
  if (file.extension() != ".log") {
    return std::nullopt;
  }
  std::string stem = file.stem().string();
  if (!stem.starts_with(prefix)) {
    return std::nullopt;
  }
  std::string_view digits(stem);
  digits.remove_prefix(prefix.size());
  digits = digits.substr(0, digits.find('.'));
  int number = 0;
  auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
  if (ec != std::errc() || ptr != digits.data() + digits.size()) {
    return std::nullopt;
  }
  return number;
}

void PruneLogDirectory(const std::filesystem::path& logs_dir, std::string_view app_name,
                       uint64_t budget_bytes) {
  std::string prefix = std::string(app_name) + "_";
  std::map<int, std::vector<std::filesystem::path>> runs;
  uint64_t total = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(logs_dir, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    auto run = RunNumber(entry.path(), prefix);
    if (!run) {
      continue;
    }
    uint64_t bytes = entry.file_size(ec);
    if (ec) {
      continue;
    }
    runs[*run].push_back(entry.path());
    total += bytes;
  }
  for (const auto& [run, files] : runs) {
    if (total <= budget_bytes) {
      break;
    }
    for (const auto& file : files) {
      uint64_t bytes = std::filesystem::file_size(file, ec);
      if (!ec && std::filesystem::remove(file, ec)) {
        total -= bytes;
      }
    }
  }
}

std::filesystem::path NextSequentialLogPath(const std::filesystem::path& logs_dir,
                                            std::string_view app_name) {
  std::error_code ec;
  std::filesystem::create_directories(logs_dir, ec);
  std::string prefix = std::string(app_name) + "_";
  int max_seq = 0;
  for (const auto& entry : std::filesystem::directory_iterator(logs_dir, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    if (auto run = RunNumber(entry.path(), prefix)) {
      max_seq = std::max(max_seq, *run);
    }
  }
  return logs_dir / fmt::format("{}_{:03d}.log", app_name, max_seq + 1);
}

std::vector<spdlog::sink_ptr> BuildDefaultSinks() {
  std::vector<spdlog::sink_ptr> sinks;
  if (g_early_sink)
    sinks.push_back(g_early_sink);
  if (g_console_sink)
    sinks.push_back(g_console_sink);
  if (g_file_sink)
    sinks.push_back(g_file_sink);
  for (auto& s : g_extra_sinks)
    sinks.push_back(s);
  return sinks;
}

// Build sinks for a specific category (handles per-category sinks from config)
std::vector<spdlog::sink_ptr> BuildCategorySinks(const std::string& name) {
  auto it = g_config.category_sinks.find(name);
  if (it != g_config.category_sinks.end()) {
    if (g_config.category_sinks_exclusive) {
      // Replace default sinks entirely
      return it->second;
    }
    // Additive: default sinks + category-specific sinks
    auto sinks = BuildDefaultSinks();
    for (auto& s : it->second)
      sinks.push_back(s);
    return sinks;
  }
  return BuildDefaultSinks();
}

// Resolve per-category level from config, or return default
spdlog::level::level_enum ResolveCategoryLevel(const std::string& name) {
  auto it = g_config.category_levels.find(name);
  if (it != g_config.category_levels.end())
    return it->second;
  return g_config.default_level;
}

// Create a logger and register it
std::shared_ptr<spdlog::logger> CreateCategoryLogger(const std::string& name) {
  auto sinks = BuildCategorySinks(name);
  auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
  logger->set_level(ResolveCategoryLevel(name));
  logger->flush_on(g_config.flush_level);
  spdlog::register_logger(logger);
  return logger;
}

}  // namespace

void InitLoggingEarly() {
  std::lock_guard lock(g_mutex);
  if (g_early_initialized || g_initialized)
    return;

#if REX_PLATFORM_WIN32
  auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
  auto sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
#endif
  sink->set_level(spdlog::level::trace);
  sink->set_pattern("[%l] [%n] %v");
  g_early_sink = sink;

  g_config.default_level = kDefaultLogLevel;

  // Create loggers for any categories already registered during static init
  for (auto& entry : g_registry) {
    if (!entry.name.empty() && !entry.logger)
      entry.logger = CreateCategoryLogger(entry.name);
  }

  // Set default logger to "core" if registered
  for (auto& entry : g_registry) {
    if (entry.name == "core") {
      spdlog::set_default_logger(entry.logger);
      break;
    }
  }
  g_early_initialized = true;
}

void InitLogging(const LogConfig& config) {
  std::lock_guard lock(g_mutex);

  if (g_initialized) {
    g_config = config;
    for (auto& entry : g_registry) {
      if (!entry.logger)
        continue;
      entry.logger->set_level(ResolveCategoryLevel(entry.name));
      entry.logger->flush_on(config.flush_level);
      if (g_config.category_levels.count(entry.name))
        entry.has_explicit_level = true;
    }
    return;
  }

  g_config = config;

  // Early sink handling:
  //   Windows: the early msvc_sink is the persistent debug channel for GUI
  //     apps and does not conflict with the stdout console sink, so keep it.
  //   Non-Windows: drop the early stdout sink unconditionally so file-only
  //     configs don't leak to stdout and console configs don't duplicate.
#if !REX_PLATFORM_WIN32
  if (g_early_sink) {
    for (auto& entry : g_registry) {
      if (entry.logger)
        std::erase(entry.logger->sinks(), g_early_sink);
    }
    g_early_sink.reset();
  }
#endif

  // Console sink (stdout, colored). Intended for console-subsystem processes.
  if (config.log_to_console) {
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    sink->set_level(spdlog::level::trace);
    sink->set_pattern(config.console_pattern);
    g_console_sink = sink;
  }

  std::filesystem::path resolved_path = config.log_file;
  if (resolved_path.empty() && !config.app_name.empty()) {
    auto log_dir =
        config.log_dir.empty() ? std::filesystem::current_path() / "logs" : config.log_dir;
    if (config.dir_budget_bytes > 0) {
      PruneLogDirectory(log_dir, config.app_name, config.dir_budget_bytes);
    }
    resolved_path = NextSequentialLogPath(log_dir, config.app_name);
  }
  if (!resolved_path.empty()) {
    spdlog::sink_ptr sink;
    if (REXCVAR_GET(log_file_async)) {
      g_async_file_sink = std::make_shared<AsyncFileSink>(resolved_path.string());
      sink = g_async_file_sink;
    } else {
      sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(resolved_path.string(), false);
    }
    sink->set_level(spdlog::level::trace);
    sink->set_pattern(config.file_pattern);
    g_file_sink = sink;
  }

  g_extra_sinks = config.extra_sinks;

  // Rebuild all loggers with new sinks
  for (auto& entry : g_registry) {
    if (entry.name.empty())
      continue;
    if (entry.logger)
      spdlog::drop(entry.name);
    entry.logger = CreateCategoryLogger(entry.name);
    if (g_config.category_levels.count(entry.name))
      entry.has_explicit_level = true;
  }

  // Set default logger to "core"
  for (auto& entry : g_registry) {
    if (entry.name == "core") {
      spdlog::set_default_logger(entry.logger);
      break;
    }
  }
  g_initialized = true;

  if (config.flush_interval.count() > 0)
    spdlog::flush_every(config.flush_interval);
}

void InitLogging(std::filesystem::path log_file, spdlog::level::level_enum level) {
  LogConfig config;
  config.log_file = std::move(log_file);
  config.default_level = level;
  InitLogging(config);
}

void ShutdownLogging() {
  std::lock_guard lock(g_mutex);
  if (!g_initialized && !g_early_initialized)
    return;

  for (auto& entry : g_registry)
    if (entry.logger)
      entry.logger->flush();
  if (g_async_file_sink)
    g_async_file_sink->FlushSync();

  spdlog::shutdown();
  g_registry.clear();
  g_console_sink.reset();
  g_file_sink.reset();
  g_async_file_sink.reset();
  g_early_sink.reset();
  g_extra_sinks.clear();
  g_initialized = false;
  g_early_initialized = false;
}

void FlushLogging() {
  std::lock_guard lock(g_mutex);
  for (auto& entry : g_registry)
    if (entry.logger)
      entry.logger->flush();
  if (g_async_file_sink)
    g_async_file_sink->FlushSync();
}

LogCategoryId RegisterLogCategory(const char* name) {
  std::lock_guard lock(g_mutex);

  // Check for duplicates
  for (size_t i = 0; i < g_registry.size(); ++i) {
    if (g_registry[i].name == name) {
      return LogCategoryId{static_cast<uint16_t>(i)};
    }
  }

  uint16_t id = static_cast<uint16_t>(g_registry.size());
  LogCategoryEntry entry;
  entry.name = name;
  if (g_initialized || g_early_initialized) {
    entry.logger = CreateCategoryLogger(name);
  }
  g_registry.push_back(std::move(entry));
  return LogCategoryId{id};
}

LogCategoryId RegisterLogSubcategory(const char* name, LogCategoryId parent) {
  std::lock_guard lock(g_mutex);

  std::string parent_name;
  if (parent.id < g_registry.size())
    parent_name = g_registry[parent.id].name;
  std::string full_name = parent_name + "." + name;

  for (size_t i = 0; i < g_registry.size(); ++i) {
    if (g_registry[i].name == full_name)
      return LogCategoryId{static_cast<uint16_t>(i)};
  }

  uint16_t id = static_cast<uint16_t>(g_registry.size());
  LogCategoryEntry entry;
  entry.name = full_name;
  entry.parent = parent;
  if (g_initialized || g_early_initialized) {
    entry.logger = CreateCategoryLogger(full_name);
    if (g_config.category_levels.count(full_name))
      entry.has_explicit_level = true;
  }
  g_registry.push_back(std::move(entry));
  return LogCategoryId{id};
}

std::optional<LogCategoryId> FindCategory(const std::string& name) {
  std::lock_guard lock(g_mutex);
  for (size_t i = 0; i < g_registry.size(); ++i) {
    if (g_registry[i].name == name) {
      return LogCategoryId{static_cast<uint16_t>(i)};
    }
  }
  return std::nullopt;
}

std::span<const LogCategoryEntry> GetAllCategories() {
  // No lock: only safe to call from main thread or after init.
  return {g_registry.data(), g_registry.size()};
}

spdlog::logger* GetLoggerRaw(LogCategoryId category) {
  if (!g_initialized && !g_early_initialized)
    InitLoggingEarly();
  if (category.id < g_registry.size()) {
    return g_registry[category.id].logger.get();
  }
  return nullptr;
}

std::shared_ptr<spdlog::logger> GetLogger(LogCategoryId category) {
  if (!g_initialized && !g_early_initialized)
    InitLoggingEarly();
  if (category.id < g_registry.size()) {
    return g_registry[category.id].logger;
  }
  return nullptr;
}

std::shared_ptr<spdlog::logger> GetLogger() {
  return GetLogger(rex::log::core());
}

void SetCategoryLevel(LogCategoryId category, spdlog::level::level_enum level) {
  if (category.id < g_registry.size()) {
    if (auto& logger = g_registry[category.id].logger) {
      logger->set_level(level);
    }
  }
}

void SetAllLevels(spdlog::level::level_enum level) {
  for (auto& entry : g_registry) {
    if (entry.logger)
      entry.logger->set_level(level);
  }
}

void SetRootLevel(LogCategoryId root, spdlog::level::level_enum level) {
  SetCategoryLevel(root, level);
  for (auto& entry : g_registry) {
    if (entry.parent.has_value() && entry.parent->id == root.id && !entry.has_explicit_level &&
        entry.logger)
      entry.logger->set_level(level);
  }
}

void RegisterLogLevelCallback() {
  rex::cvar::RegisterChangeCallback("log_level", [](std::string_view, std::string_view value) {
    if (auto level = ParseLogLevel(std::string(value))) {
      for (size_t i = 0; i < g_registry.size(); ++i) {
        auto& entry = g_registry[i];
        if (!entry.parent.has_value() && entry.logger && !entry.has_explicit_level)
          SetRootLevel(LogCategoryId{static_cast<uint16_t>(i)}, *level);
      }
    }
  });
}

void AddSink(spdlog::sink_ptr sink) {
  std::lock_guard lock(g_mutex);
  g_extra_sinks.push_back(sink);
  for (auto& entry : g_registry) {
    if (entry.logger)
      entry.logger->sinks().push_back(sink);
  }
}

void AddSink(LogCategoryId category, spdlog::sink_ptr sink) {
  std::lock_guard lock(g_mutex);
  if (category.id < g_registry.size()) {
    if (auto& logger = g_registry[category.id].logger) {
      logger->sinks().push_back(sink);
    }
  }
}

void RemoveSink(spdlog::sink_ptr sink) {
  std::lock_guard lock(g_mutex);
  std::erase(g_extra_sinks, sink);
  for (auto& entry : g_registry) {
    if (entry.logger)
      std::erase(entry.logger->sinks(), sink);
  }
}

void RemoveSink(LogCategoryId category, spdlog::sink_ptr sink) {
  std::lock_guard lock(g_mutex);
  if (category.id < g_registry.size()) {
    if (auto& logger = g_registry[category.id].logger) {
      std::erase(logger->sinks(), sink);
    }
  }
}

void ReplaceConsoleSink(spdlog::sink_ptr sink) {
  std::lock_guard lock(g_mutex);
  for (auto& entry : g_registry) {
    if (!entry.logger)
      continue;
    auto& sinks = entry.logger->sinks();
    if (g_console_sink)
      std::erase(sinks, g_console_sink);
    if (sink)
      sinks.push_back(sink);
  }
  g_console_sink = sink;
}

void SetConsolePattern(const std::string& pattern) {
  if (g_console_sink)
    g_console_sink->set_pattern(pattern);
}

void SetFilePattern(const std::string& pattern) {
  if (g_file_sink)
    g_file_sink->set_pattern(pattern);
}

// ==========================================================================
// CLI Helpers
// ==========================================================================

std::optional<spdlog::level::level_enum> ParseLogLevel(const std::string& level_str) {
  static const std::unordered_map<std::string, spdlog::level::level_enum> level_map = {
      {"trace", spdlog::level::trace},  {"debug", spdlog::level::debug},
      {"info", spdlog::level::info},    {"warn", spdlog::level::warn},
      {"warning", spdlog::level::warn}, {"error", spdlog::level::err},
      {"err", spdlog::level::err},      {"critical", spdlog::level::critical},
      {"off", spdlog::level::off},
  };

  std::string lower = level_str;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  auto it = level_map.find(lower);
  if (it != level_map.end())
    return it->second;
  return std::nullopt;
}

spdlog::level::level_enum ParseLogLevelOr(const std::string& level_str,
                                          spdlog::level::level_enum default_level) {
  return ParseLogLevel(level_str).value_or(default_level);
}

LogConfig BuildLogConfig(const std::string& cli_level,
                         const std::map<std::string, std::string>& category_levels) {
  return BuildLogConfig(nullptr, cli_level, category_levels);
}

LogConfig BuildLogConfig(const char* log_file, const std::string& cli_level,
                         const std::map<std::string, std::string>& category_levels) {
  LogConfig config;
  if (log_file) {
    config.log_file = log_file;
  }

  // Build-type default
  config.default_level = kDefaultLogLevel;

  // Environment variable
  if (auto env_level = rex::platform::env::get("REX_LOG_LEVEL")) {
    if (auto level = ParseLogLevel(*env_level))
      config.default_level = *level;
  } else if (auto env_level2 = rex::platform::env::get("SPDLOG_LEVEL")) {
    if (auto level = ParseLogLevel(*env_level2))
      config.default_level = *level;
  }

  // CLI global level overrides environment
  if (!cli_level.empty()) {
    if (auto level = ParseLogLevel(cli_level))
      config.default_level = *level;
  }

  // Per-category CLI levels (string-keyed)
  for (const auto& [cat_name, level_str] : category_levels) {
    if (level_str.empty())
      continue;
    if (auto level = ParseLogLevel(level_str)) {
      config.category_levels[cat_name] = *level;
    }
  }

  return config;
}

void ApplyLogCvarOverrides(LogConfig& config) {
  if (rex::cvar::HasNonDefaultValue("log_file")) {
    config.log_file = std::string(REXCVAR_GET(log_file));
  }
  if (rex::cvar::HasNonDefaultValue("log_flush_interval")) {
    config.flush_interval = std::chrono::seconds(REXCVAR_GET(log_flush_interval));
  }
}

const LogConfig& LoggingConfig() {
  return g_config;
}

std::map<std::string, std::string> ParseCategoryLevelsFromConfig(
    const std::filesystem::path& config_path) {
  std::map<std::string, std::string> result;
  if (!std::filesystem::exists(config_path))
    return result;

  try {
    auto config = toml::parse_file(config_path.string());
    auto* log_table = config["log"].as_table();
    if (!log_table)
      return result;
    auto* levels_table = (*log_table)["levels"].as_table();
    if (!levels_table)
      return result;

    for (const auto& [key, value] : *levels_table) {
      if (value.is_string())
        result[std::string(key)] = value.as_string()->get();
    }
  } catch (const toml::parse_error&) {}

  return result;
}

}  // namespace rex
