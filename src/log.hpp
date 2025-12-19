#pragma once

#include <hyprland/src/debug/log/Logger.hpp>
#include <format>
#include <utility>

// Define all the constants that exist in Log namespace
// From the error output, we can see these exist:
// Log::TRACE, Log::DEBUG, Log::INFO, Log::WARN, Log::ERR, Log::CRIT
inline constexpr auto TRACE = Log::TRACE;
inline constexpr auto DEBUG = Log::DEBUG;
inline constexpr auto INFO  = Log::INFO;
inline constexpr auto WARN  = Log::WARN;
inline constexpr auto ERR   = Log::ERR;
inline constexpr auto CRIT  = Log::CRIT;

// For backward compatibility with LOG constant used in original code
inline constexpr auto LOG   = Log::DEBUG;

// Note: Log::NONE might not exist, so don't define NONE unless you need it

/**
 * @brief Main logging function for hy3 plugin
 */
template <typename... Args>
void hy3_log(Hyprutils::CLI::eLogLevel level, std::format_string<Args...> fmt, Args&&... args) {
	if (!Log::logger) {
		return;
	}

	try {
		auto msg = std::vformat(fmt.get(), std::make_format_args(args...));
		Log::logger->log(level, "[hy3] {}", msg);
	} catch (const std::format_error& e) {
		// Fallback for format errors
		Log::logger->log(Log::ERR, "[hy3] Format error in log message: {}", e.what());
	} catch (...) {
		Log::logger->log(Log::ERR, "[hy3] Unknown error during logging");
	}
}

// Convenience functions
template <typename... Args>
void hy3_trace(std::format_string<Args...> fmt, Args&&... args) {
	hy3_log(Log::TRACE, std::move(fmt), std::forward<Args>(args)...);
}

template <typename... Args>
void hy3_debug(std::format_string<Args...> fmt, Args&&... args) {
	hy3_log(Log::DEBUG, std::move(fmt), std::forward<Args>(args)...);
}

template <typename... Args>
void hy3_info(std::format_string<Args...> fmt, Args&&... args) {
	hy3_log(Log::INFO, std::move(fmt), std::forward<Args>(args)...);
}

template <typename... Args>
void hy3_warn(std::format_string<Args...> fmt, Args&&... args) {
	hy3_log(Log::WARN, std::move(fmt), std::forward<Args>(args)...);
}

template <typename... Args>
void hy3_error(std::format_string<Args...> fmt, Args&&... args) {
	hy3_log(Log::ERR, std::move(fmt), std::forward<Args>(args)...);
}

template <typename... Args>
void hy3_critical(std::format_string<Args...> fmt, Args&&... args) {
	hy3_log(Log::CRIT, std::move(fmt), std::forward<Args>(args)...);
}
