// Gard Crash Report & Structured Logging (Tier 9)
// Provides crash report generation and a structured logging system with ring buffer.
// Compile: gcc -c -O2 gard_crash_report.c -o gard_crash_report.o
// Archive: ar rcs libgard_crash_report.a gard_crash_report.o

#ifndef GARD_CRASH_REPORT_H
#define GARD_CRASH_REPORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Crash Report Generation
// ============================================================

// Write a crash report file (gard-crash-YYYY-MM-DD-HHMMSS.log) to the configured
// output directory. Prints a one-line notice to stderr with the filename.
// type: error class name (e.g. "NullReferenceError")
// message: human-readable description of the error
// This function is designed to be safe to call from signal handlers where possible.
void gard_crash_report_write(const char* type, const char* message);

// Enable or disable crash report generation (default: enabled).
// enabled: 1 = enabled, 0 = disabled
void gard_crash_report_enable(int32_t enabled);

// Set the output directory for crash report files (default: current directory ".").
// dir: path to directory (must exist). NULL resets to current directory.
void gard_crash_report_set_dir(const char* dir);

// ============================================================
// Structured Logging
// ============================================================

// Log levels
#define GARD_LOG_TRACE  0
#define GARD_LOG_DEBUG  1
#define GARD_LOG_INFO   2
#define GARD_LOG_WARN   3
#define GARD_LOG_ERROR  4
#define GARD_LOG_FATAL  5

// Core logging function.
// level: one of GARD_LOG_TRACE..GARD_LOG_FATAL
// module: module/component name (e.g. "scheduler", "gc")
// message: log message text
void gard_log(int32_t level, const char* module, const char* message);

// Set the minimum log level. Messages below this level are discarded.
// Default: GARD_LOG_INFO
void gard_log_set_level(int32_t level);

// Set the log output file path. NULL = stderr (default).
// The file is opened in append mode. Each write is flushed immediately.
void gard_log_set_output(const char* path);

// Set the log output format.
// format: "text" (default) or "json"
void gard_log_set_format(const char* format);

// Convenience logging functions
void gard_log_trace(const char* module, const char* message);
void gard_log_debug(const char* module, const char* message);
void gard_log_info(const char* module, const char* message);
void gard_log_warn(const char* module, const char* message);
void gard_log_error(const char* module, const char* message);
void gard_log_fatal(const char* module, const char* message);

#ifdef __cplusplus
}
#endif

#endif // GARD_CRASH_REPORT_H
