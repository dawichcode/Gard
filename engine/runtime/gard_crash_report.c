// Gard Crash Report & Structured Logging (Tier 9) — Implementation
// Compile: gcc -c -O2 gard_crash_report.c -o gard_crash_report.o
// Archive: ar rcs libgard_crash_report.a gard_crash_report.o

#define _GNU_SOURCE
#include "gard_crash_report.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/utsname.h>
#include <execinfo.h>
#include <dlfcn.h>

// ============================================================
// Ring Buffer for Recent Logs
// ============================================================

#define GARD_LOG_RING_SIZE 100
#define GARD_LOG_ENTRY_MAX 512

typedef struct {
    char text[GARD_LOG_ENTRY_MAX];
    int32_t level;
} GardLogEntry;

static GardLogEntry gard_log_ring_[GARD_LOG_RING_SIZE];
static int32_t gard_log_ring_head_ = 0;   // next write position
static int32_t gard_log_ring_count_ = 0;  // number of entries stored

// ============================================================
// Logging State
// ============================================================

static int32_t gard_log_level_ = GARD_LOG_INFO;
static FILE* gard_log_file_ = NULL;       // NULL means stderr
static int32_t gard_log_format_json_ = 0; // 0 = text, 1 = json
static pthread_mutex_t gard_log_mutex_ = PTHREAD_MUTEX_INITIALIZER;

// ============================================================
// Crash Report State
// ============================================================

static int32_t gard_crash_enabled_ = 1;
static char gard_crash_dir_[512] = ".";

// ============================================================
// Internal Helpers
// ============================================================

static const char* gard_level_name(int32_t level) {
    switch (level) {
        case GARD_LOG_TRACE: return "TRACE";
        case GARD_LOG_DEBUG: return "DEBUG";
        case GARD_LOG_INFO:  return "INFO";
        case GARD_LOG_WARN:  return "WARN";
        case GARD_LOG_ERROR: return "ERROR";
        case GARD_LOG_FATAL: return "FATAL";
        default:             return "UNKNOWN";
    }
}

static void gard_format_timestamp(char* buf, size_t bufsize) {
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    strftime(buf, bufsize, "%Y-%m-%d %H:%M:%S", &tm_buf);
}

static void gard_format_timestamp_iso(char* buf, size_t bufsize) {
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    strftime(buf, bufsize, "%Y-%m-%dT%H:%M:%S", &tm_buf);
}

static void gard_format_timestamp_file(char* buf, size_t bufsize) {
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    strftime(buf, bufsize, "%Y-%m-%d-%H%M%S", &tm_buf);
}

// Escape a string for JSON output (minimal: escape quotes and backslashes)
static void gard_json_escape(char* dst, size_t dstsize, const char* src) {
    if (!src) { dst[0] = '\0'; return; }
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dstsize - 2; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            if (j + 2 >= dstsize) break;
            dst[j++] = '\\';
            dst[j++] = src[i];
        } else if (src[i] == '\n') {
            if (j + 2 >= dstsize) break;
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else if (src[i] == '\r') {
            if (j + 2 >= dstsize) break;
            dst[j++] = '\\';
            dst[j++] = 'r';
        } else if (src[i] == '\t') {
            if (j + 2 >= dstsize) break;
            dst[j++] = '\\';
            dst[j++] = 't';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

// ============================================================
// Ring Buffer Operations
// ============================================================

static void gard_log_ring_push(int32_t level, const char* formatted) {
    // No mutex here — caller must hold gard_log_mutex_
    GardLogEntry* entry = &gard_log_ring_[gard_log_ring_head_];
    entry->level = level;
    strncpy(entry->text, formatted, GARD_LOG_ENTRY_MAX - 1);
    entry->text[GARD_LOG_ENTRY_MAX - 1] = '\0';

    gard_log_ring_head_ = (gard_log_ring_head_ + 1) % GARD_LOG_RING_SIZE;
    if (gard_log_ring_count_ < GARD_LOG_RING_SIZE) {
        gard_log_ring_count_++;
    }
}

// ============================================================
// Structured Logging API
// ============================================================

void gard_log(int32_t level, const char* module, const char* message) {
    if (level < gard_log_level_) return;
    if (!module) module = "runtime";
    if (!message) message = "";

    char timestamp[32];
    char line[GARD_LOG_ENTRY_MAX];

    pthread_mutex_lock(&gard_log_mutex_);

    if (gard_log_format_json_) {
        gard_format_timestamp_iso(timestamp, sizeof(timestamp));
        char esc_module[128];
        char esc_message[384];
        gard_json_escape(esc_module, sizeof(esc_module), module);
        gard_json_escape(esc_message, sizeof(esc_message), message);
        snprintf(line, sizeof(line),
            "{\"timestamp\":\"%s\",\"level\":\"%s\",\"module\":\"%s\",\"message\":\"%s\"}",
            timestamp, gard_level_name(level), esc_module, esc_message);
    } else {
        gard_format_timestamp(timestamp, sizeof(timestamp));
        snprintf(line, sizeof(line), "[%s] [%s] [%s] %s",
            timestamp, gard_level_name(level), module, message);
    }

    // Write to output
    FILE* out = gard_log_file_ ? gard_log_file_ : stderr;
    fprintf(out, "%s\n", line);
    fflush(out);

    // Push to ring buffer (store text format for crash reports)
    char ring_text[GARD_LOG_ENTRY_MAX];
    if (gard_log_format_json_) {
        // For ring buffer, always store text format for readability in crash reports
        char ts2[32];
        gard_format_timestamp(ts2, sizeof(ts2));
        snprintf(ring_text, sizeof(ring_text), "[%s] [%s] [%s] %s",
            ts2, gard_level_name(level), module, message);
    } else {
        strncpy(ring_text, line, sizeof(ring_text) - 1);
        ring_text[sizeof(ring_text) - 1] = '\0';
    }
    gard_log_ring_push(level, ring_text);

    pthread_mutex_unlock(&gard_log_mutex_);
}

void gard_log_set_level(int32_t level) {
    if (level < GARD_LOG_TRACE) level = GARD_LOG_TRACE;
    if (level > GARD_LOG_FATAL) level = GARD_LOG_FATAL;
    gard_log_level_ = level;
}

void gard_log_set_output(const char* path) {
    pthread_mutex_lock(&gard_log_mutex_);
    if (gard_log_file_ && gard_log_file_ != stderr && gard_log_file_ != stdout) {
        fclose(gard_log_file_);
        gard_log_file_ = NULL;
    }
    if (path) {
        gard_log_file_ = fopen(path, "a");
        // If open fails, fall back to stderr
        if (!gard_log_file_) {
            gard_log_file_ = NULL;
        }
    } else {
        gard_log_file_ = NULL;
    }
    pthread_mutex_unlock(&gard_log_mutex_);
}

void gard_log_set_format(const char* format) {
    if (format && strcmp(format, "json") == 0) {
        gard_log_format_json_ = 1;
    } else {
        gard_log_format_json_ = 0;
    }
}

void gard_log_trace(const char* module, const char* message) {
    gard_log(GARD_LOG_TRACE, module, message);
}

void gard_log_debug(const char* module, const char* message) {
    gard_log(GARD_LOG_DEBUG, module, message);
}

void gard_log_info(const char* module, const char* message) {
    gard_log(GARD_LOG_INFO, module, message);
}

void gard_log_warn(const char* module, const char* message) {
    gard_log(GARD_LOG_WARN, module, message);
}

void gard_log_error(const char* module, const char* message) {
    gard_log(GARD_LOG_ERROR, module, message);
}

void gard_log_fatal(const char* module, const char* message) {
    gard_log(GARD_LOG_FATAL, module, message);
}

// ============================================================
// Crash Report Configuration
// ============================================================

void gard_crash_report_enable(int32_t enabled) {
    gard_crash_enabled_ = enabled ? 1 : 0;
}

void gard_crash_report_set_dir(const char* dir) {
    if (dir) {
        strncpy(gard_crash_dir_, dir, sizeof(gard_crash_dir_) - 1);
        gard_crash_dir_[sizeof(gard_crash_dir_) - 1] = '\0';
    } else {
        strcpy(gard_crash_dir_, ".");
    }
}

// ============================================================
// Crash Report Generation
// ============================================================

// External references from gard_runtime.c (source location info)
extern const char* gard_source_file_;
extern int32_t gard_panic_line_;
extern int32_t gard_panic_col_;

// Weak reference to gard_memory_usage from gard_watchdog.c
// If the watchdog is not linked, this resolves to NULL and we report 0.
int64_t gard_memory_usage(void) __attribute__((weak));

void gard_crash_report_write(const char* type, const char* message) {
    if (!gard_crash_enabled_) return;
    if (!type) type = "UnknownError";
    if (!message) message = "(no message)";

    // Generate timestamp for filename
    char ts_file[32];
    gard_format_timestamp_file(ts_file, sizeof(ts_file));

    char ts_human[32];
    gard_format_timestamp(ts_human, sizeof(ts_human));

    // Build filename
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/gard-crash-%s.log", gard_crash_dir_, ts_file);

    // Open file for writing (use low-level open for signal safety where possible,
    // but fprintf is more practical for formatted output)
    FILE* f = fopen(filepath, "w");
    if (!f) {
        // If we can't write the crash report, silently fail
        return;
    }

    // === Header ===
    fprintf(f, "=== Gard Crash Report ===\n");
    fprintf(f, "Timestamp: %s\n", ts_human);

    // Source location
    if (gard_source_file_ && gard_panic_line_ > 0) {
        fprintf(f, "Source:    %s:%d:%d\n", gard_source_file_, gard_panic_line_,
                gard_panic_col_ > 0 ? gard_panic_col_ : 1);
    } else if (gard_source_file_) {
        fprintf(f, "Source:    %s\n", gard_source_file_);
    } else {
        fprintf(f, "Source:    (unknown)\n");
    }

    fprintf(f, "Type:      %s\n", type);
    fprintf(f, "Message:   %s\n", message);

    // === Stack Trace ===
    fprintf(f, "\n--- Stack Trace ---\n");

    void* frames[64];
    int count = backtrace(frames, 64);

    if (count > 0) {
        int printed = 0;
        int start = 3; // Skip internal frames
        if (start >= count) start = 0;

        for (int i = start; i < count && printed < 20; i++) {
            Dl_info info;
            void* adjusted = (void*)((char*)frames[i] - 1);
            if (dladdr(adjusted, &info) && info.dli_sname) {
                const char* name = info.dli_sname;
                // Skip internal frames
                if (strcmp(name, "__libc_start_main") == 0) break;
                if (strcmp(name, "__libc_start_call_main") == 0) break;
                if (strcmp(name, "_start") == 0) break;
                if (strcmp(name, "gard_signal_handler") == 0) continue;
                if (strcmp(name, "gard_panic") == 0) continue;
                if (strcmp(name, "gard_panic_null") == 0) continue;
                if (strcmp(name, "gard_panic_index") == 0) continue;
                if (strcmp(name, "gard_panic_arithmetic") == 0) continue;
                if (strcmp(name, "gard_crash_report_write") == 0) continue;

                fprintf(f, "  -> %s()\n", name);
                printed++;
            }
        }

        if (printed == 0) {
            fprintf(f, "  (no user frames captured)\n");
        }
    } else {
        fprintf(f, "  (stack trace unavailable)\n");
    }

    // === System Info ===
    fprintf(f, "\n--- System Info ---\n");

    struct utsname uts;
    if (uname(&uts) == 0) {
        fprintf(f, "Platform:  %s %s\n", uts.sysname, uts.machine);
    } else {
        fprintf(f, "Platform:  (unknown)\n");
    }

    fprintf(f, "PID:       %d\n", (int)getpid());

    int64_t mem = gard_memory_usage ? gard_memory_usage() : 0;
    if (mem > 0) {
        int64_t mb = mem / (1024 * 1024);
        if (mb > 0) {
            fprintf(f, "Memory:    %ld MB tracked\n", (long)mb);
        } else {
            fprintf(f, "Memory:    %ld bytes tracked\n", (long)mem);
        }
    } else {
        fprintf(f, "Memory:    0 bytes tracked\n");
    }

    // === Environment ===
    fprintf(f, "\n--- Environment ---\n");

    const char* env_vars[] = { "GARD_ENV", "GARD_LOG_LEVEL", "GARD_DEBUG", NULL };
    int env_printed = 0;
    for (int i = 0; env_vars[i]; i++) {
        const char* val = getenv(env_vars[i]);
        if (val) {
            fprintf(f, "%s=%s\n", env_vars[i], val);
            env_printed++;
        }
    }
    if (env_printed == 0) {
        fprintf(f, "(no Gard environment variables set)\n");
    }

    // === Recent Logs (from ring buffer) ===
    fprintf(f, "\n--- Recent Logs ---\n");

    // We don't lock the mutex here because we may be in a signal handler
    // and the mutex might already be held. Read the ring buffer best-effort.
    if (gard_log_ring_count_ > 0) {
        int start_idx;
        int total = gard_log_ring_count_;
        if (total >= GARD_LOG_RING_SIZE) {
            start_idx = gard_log_ring_head_; // oldest entry
        } else {
            start_idx = 0;
        }

        for (int i = 0; i < total; i++) {
            int idx = (start_idx + i) % GARD_LOG_RING_SIZE;
            if (gard_log_ring_[idx].text[0] != '\0') {
                fprintf(f, "  %s\n", gard_log_ring_[idx].text);
            }
        }
    } else {
        fprintf(f, "  (no recent log entries)\n");
    }

    fprintf(f, "\n=== End of Crash Report ===\n");
    fclose(f);

    // Print notice to stderr
    fprintf(stderr, "Crash report written to: %s\n", filepath);
}
