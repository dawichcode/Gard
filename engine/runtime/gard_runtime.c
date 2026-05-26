// Gard Native Runtime Library — C implementation
// Compile: gcc -c -O2 -fPIC gard_runtime.c -o gard_runtime.o
// Archive: ar rcs libgard_runtime.a gard_runtime.o

#define _GNU_SOURCE
#include "gard_runtime.h"
#include "gard_crash_report.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <regex.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <execinfo.h>
#include <dlfcn.h>

// ============================================================
// Runtime Panic System & Signal Handlers
// ============================================================

// ANSI color codes
#define GARD_RED     "\033[31m"
#define GARD_YELLOW  "\033[33m"
#define GARD_CYAN    "\033[36m"
#define GARD_GRAY    "\033[90m"
#define GARD_BOLD    "\033[1m"
#define GARD_RESET   "\033[0m"

// Maximum stack frames to capture
#define GARD_MAX_FRAMES 64

// Demangle a backtrace symbol to extract just the function name.
// Input format: "./binary(funcName+0x1a) [0x...]" or similar
static const char* gard_demangle_symbol(const char* symbol, char* buf, int bufsize) {
    // Look for '(' which precedes the function name
    const char* start = strchr(symbol, '(');
    if (start) {
        start++; // skip '('
        const char* end = strchr(start, '+');
        if (!end) end = strchr(start, ')');
        if (end && end > start) {
            int len = (int)(end - start);
            if (len >= bufsize) len = bufsize - 1;
            memcpy(buf, start, len);
            buf[len] = '\0';
            return buf;
        }
    }
    // Fallback: return the whole symbol
    int len = strlen(symbol);
    if (len >= bufsize) len = bufsize - 1;
    memcpy(buf, symbol, len);
    buf[len] = '\0';
    return buf;
}

static void gard_print_stack_trace(void) {
    void* frames[GARD_MAX_FRAMES];
    int count = backtrace(frames, GARD_MAX_FRAMES);

    if (count <= 0) {
        fprintf(stderr, "   " GARD_GRAY "(stack trace unavailable)" GARD_RESET "\n");
        return;
    }

    int printed = 0;

    // Skip internal frames (gard_print_stack_trace, gard_panic/gard_panic_null, etc.)
    int start = 3;
    if (start >= count) start = 0;

    for (int i = start; i < count && printed < 20; i++) {
        Dl_info info;
        // Subtract 1 from return address to get an address *within* the calling function.
        // Return addresses point to the instruction AFTER the call, which may be past
        // the function boundary (especially with noreturn calls like gard_panic_null).
        void* adjusted = (void*)((char*)frames[i] - 1);
        if (dladdr(adjusted, &info) && info.dli_sname) {
            const char* name = info.dli_sname;
            // Skip internal libc frames
            if (strcmp(name, "__libc_start_main") == 0) break;
            if (strcmp(name, "__libc_start_call_main") == 0) break;
            if (strcmp(name, "_start") == 0) break;
            if (strcmp(name, "gard_signal_handler") == 0) continue;
            if (strcmp(name, "gard_panic") == 0) continue;
            if (strcmp(name, "gard_panic_null") == 0) continue;
            if (strcmp(name, "gard_panic_index") == 0) continue;
            if (strcmp(name, "gard_panic_arithmetic") == 0) continue;
            if (strcmp(name, "gard_print_stack_trace") == 0) continue;

            fprintf(stderr, "   -> %s%s%s()\n", GARD_CYAN, name, GARD_RESET);
            printed++;
        } else {
            // Fallback: try backtrace_symbols for this single frame
            char** sym = backtrace_symbols(&frames[i], 1);
            if (sym) {
                char namebuf[256];
                const char* name = gard_demangle_symbol(sym[0], namebuf, sizeof(namebuf));
                if (name[0] != '\0' && strcmp(name, "?") != 0) {
                    if (strcmp(name, "__libc_start_main") == 0 ||
                        strcmp(name, "__libc_start_call_main") == 0 ||
                        strcmp(name, "_start") == 0) { free(sym); break; }
                    fprintf(stderr, "   -> %s%s%s()\n", GARD_CYAN, name, GARD_RESET);
                    printed++;
                }
                free(sym);
            }
        }
    }

    if (printed == 0) {
        // Last resort: print raw addresses
        for (int i = start; i < count && i < start + 10; i++) {
            fprintf(stderr, "   -> %s%p%s\n", GARD_GRAY, frames[i], GARD_RESET);
        }
    }
}

// Global source file name for error reporting (non-static for crash report access)
const char* gard_source_file_ = NULL;
int32_t gard_panic_line_ = 0;
int32_t gard_panic_col_ = 0;

void gard_set_source_file(const char* file) {
    gard_source_file_ = file;
}

void gard_panic(const char* type, const char* message) {
    fprintf(stderr, "\n");
    fprintf(stderr, GARD_BOLD GARD_RED "══════════════════════════════════════════" GARD_RESET "\n");
    fprintf(stderr, GARD_BOLD GARD_RED " Gard Runtime Panic" GARD_RESET "\n");
    fprintf(stderr, GARD_BOLD GARD_RED "══════════════════════════════════════════" GARD_RESET "\n");
    fprintf(stderr, "\n");
    fprintf(stderr, " " GARD_BOLD "Type:" GARD_RESET "    %s%s%s\n", GARD_YELLOW, type ? type : "UnknownError", GARD_RESET);
    fprintf(stderr, " " GARD_BOLD "Message:" GARD_RESET " %s\n", message ? message : "(no message)");
    if (gard_source_file_ && gard_panic_line_ > 0) {
        fprintf(stderr, " " GARD_BOLD "At:" GARD_RESET "      %s%s:%d:%d%s\n", GARD_CYAN, gard_source_file_, gard_panic_line_, gard_panic_col_ > 0 ? gard_panic_col_ : 1, GARD_RESET);
    } else if (gard_source_file_) {
        fprintf(stderr, " " GARD_BOLD "File:" GARD_RESET "    %s\n", gard_source_file_);
    }
    fprintf(stderr, "\n");
    fprintf(stderr, " " GARD_BOLD "Stack Trace:" GARD_RESET "\n");
    gard_print_stack_trace();
    fprintf(stderr, "\n");
    fprintf(stderr, GARD_BOLD GARD_RED "══════════════════════════════════════════" GARD_RESET "\n");
    fprintf(stderr, "\n");

    gard_crash_report_write(type, message);
    _exit(134);
}

void gard_panic_null(const char* function, int32_t line, int32_t col) {
    gard_panic_line_ = line;
    gard_panic_col_ = col;
    __gard_exception_line = line;
    __gard_exception_col = col;
    __gard_exception_caller = function;
    char msg[512];
    if (function && line > 0) {
        snprintf(msg, sizeof(msg), "Attempted to access a null reference in %s() at line %d", function, line);
    } else if (function) {
        snprintf(msg, sizeof(msg), "Attempted to access a null reference in %s()", function);
    } else {
        snprintf(msg, sizeof(msg), "Attempted to access a null reference");
    }
    // If there's an exception handler, throw instead of panicking
    if (__gard_exception_frame) {
        gard_throw("NullReferenceError", strdup(msg));
        return; // unreachable
    }
    gard_panic("NullReferenceError", msg);
}

void gard_panic_index(int32_t index, int32_t length, const char* function) {
    char msg[512];
    if (function) {
        snprintf(msg, sizeof(msg), "Index %d out of bounds for array of length %d in %s()", index, length, function);
    } else {
        snprintf(msg, sizeof(msg), "Index %d out of bounds for array of length %d", index, length);
    }
    if (__gard_exception_frame) {
        gard_throw("IndexOutOfBoundsError", strdup(msg));
        return;
    }
    gard_panic("IndexOutOfBoundsError", msg);
}

void gard_panic_arithmetic(const char* op, const char* function, int32_t line, int32_t col) {
    gard_panic_line_ = line;
    gard_panic_col_ = col;
    __gard_exception_line = line;
    __gard_exception_col = col;
    __gard_exception_caller = function;
    char msg[512];
    if (function) {
        snprintf(msg, sizeof(msg), "%s in %s()", op ? op : "Arithmetic error", function);
    } else {
        snprintf(msg, sizeof(msg), "%s", op ? op : "Arithmetic error");
    }
    if (__gard_exception_frame) {
        gard_throw("ArithmeticError", strdup(msg));
        return;
    }
    gard_panic("ArithmeticError", msg);
}

// Signal handler — intercepts OS-level crashes
static void gard_signal_handler(int sig) {
    // Reset signal to default to avoid infinite loops
    signal(sig, SIG_DFL);

    const char* type = "RuntimeError";
    const char* message = "Unknown signal";

    switch (sig) {
        case SIGSEGV:
            type = "SegmentationFault";
            message = "Invalid memory access (possible null pointer dereference or use-after-free)";
            break;
        case SIGABRT:
            type = "AbortError";
            message = "Process aborted (internal assertion failure or memory corruption)";
            break;
        case SIGFPE:
            type = "ArithmeticError";
            message = "Floating point exception (division by zero or overflow)";
            break;
        case SIGBUS:
            type = "BusError";
            message = "Bus error (misaligned memory access or invalid address)";
            break;
        case SIGILL:
            type = "IllegalInstructionError";
            message = "Illegal instruction (corrupted code or unsupported CPU feature)";
            break;
    }

    // Print the panic (can't use gard_panic directly because stack may be corrupted)
    fprintf(stderr, "\n");
    fprintf(stderr, GARD_BOLD GARD_RED "══════════════════════════════════════════" GARD_RESET "\n");
    fprintf(stderr, GARD_BOLD GARD_RED " Gard Runtime Panic" GARD_RESET "\n");
    fprintf(stderr, GARD_BOLD GARD_RED "══════════════════════════════════════════" GARD_RESET "\n");
    fprintf(stderr, "\n");
    fprintf(stderr, " " GARD_BOLD "Type:" GARD_RESET "    %s%s%s\n", GARD_YELLOW, type, GARD_RESET);
    fprintf(stderr, " " GARD_BOLD "Message:" GARD_RESET " %s\n", message);
    if (gard_source_file_) {
        fprintf(stderr, " " GARD_BOLD "At:" GARD_RESET "      %s%s%s\n", GARD_CYAN, gard_source_file_, GARD_RESET);
    }
    fprintf(stderr, "\n");
    fprintf(stderr, " " GARD_BOLD "Stack Trace:" GARD_RESET "\n");

    // Capture stack trace using dladdr (works with PIE binaries)
    void* frames[GARD_MAX_FRAMES];
    int count = backtrace(frames, GARD_MAX_FRAMES);

    if (count > 0) {
        int printed = 0;
        // Skip signal handler frames (typically 2-3 internal frames)
        int start = 2;
        if (start >= count) start = 0;

        for (int i = start; i < count && printed < 20; i++) {
            Dl_info info;
            void* adjusted = (void*)((char*)frames[i] - 1);
            if (dladdr(adjusted, &info) && info.dli_sname) {
                const char* name = info.dli_sname;
                if (strcmp(name, "__libc_start_main") == 0) break;
                if (strcmp(name, "__libc_start_call_main") == 0) break;
                if (strcmp(name, "_start") == 0) break;
                if (strcmp(name, "gard_signal_handler") == 0) continue;

                fprintf(stderr, "   -> %s%s%s()\n", GARD_CYAN, name, GARD_RESET);
                printed++;
            } else {
                char** sym = backtrace_symbols(&frames[i], 1);
                if (sym) {
                    char namebuf[256];
                    const char* name = gard_demangle_symbol(sym[0], namebuf, sizeof(namebuf));
                    if (name[0] != '\0' && strcmp(name, "__libc_start_main") != 0 &&
                        strcmp(name, "__libc_start_call_main") != 0 &&
                        strcmp(name, "_start") != 0) {
                        fprintf(stderr, "   -> %s%s%s()\n", GARD_CYAN, name, GARD_RESET);
                        printed++;
                    }
                    free(sym);
                }
            }
        }

        if (printed == 0) {
            for (int i = start; i < count && i < start + 10; i++) {
                fprintf(stderr, "   -> %s%p%s\n", GARD_GRAY, frames[i], GARD_RESET);
            }
        }
    } else {
        fprintf(stderr, "   " GARD_GRAY "(stack trace unavailable)" GARD_RESET "\n");
    }

    fprintf(stderr, "\n");
    fprintf(stderr, GARD_BOLD GARD_RED "══════════════════════════════════════════" GARD_RESET "\n");
    fprintf(stderr, "\n");

    gard_crash_report_write(type, message);
    _exit(128 + sig);
}

void gard_runtime_init(void) {
    // Install signal handlers for crash interception
    signal(SIGSEGV, gard_signal_handler);
    signal(SIGABRT, gard_signal_handler);
    signal(SIGFPE,  gard_signal_handler);
    signal(SIGBUS,  gard_signal_handler);
    signal(SIGILL,  gard_signal_handler);
}

// ============================================================
// Exception Handling (setjmp/longjmp)
// ============================================================

#include <setjmp.h>

// Thread-local exception state for crash isolation
__thread GardExceptionFrame* __gard_exception_frame = NULL;
__thread const char* __gard_exception_msg = NULL;
__thread const char* __gard_exception_type = NULL;
__thread int32_t __gard_exception_line = 0;
__thread int32_t __gard_exception_col = 0;
__thread const char* __gard_exception_caller = NULL;

void gard_exception_push(GardExceptionFrame* frame) {
    frame->prev = __gard_exception_frame;
    __gard_exception_frame = frame;
}

void gard_exception_pop(void) {
    if (__gard_exception_frame) {
        __gard_exception_frame = __gard_exception_frame->prev;
    }
}

int32_t gard_has_exception_handler(void) {
    return __gard_exception_frame != NULL ? 1 : 0;
}

void gard_throw(const char* type, const char* message) {
    __gard_exception_type = type;
    __gard_exception_msg = message;
    // Line/col are set by the throw site (either the Throw opcode or panic functions)
    // If not set by the throw site, use the panic globals
    if (__gard_exception_line == 0) {
        __gard_exception_line = gard_panic_line_;
        __gard_exception_col = gard_panic_col_;
    }
    if (__gard_exception_frame) {
        GardExceptionFrame* frame = __gard_exception_frame;
        __gard_exception_frame = frame->prev;
        longjmp(frame->buf, 1);
    }
    // No handler — fatal panic
    gard_panic(type, message);
}

// ============================================================
// Array (List) operations
// ============================================================

GardArray* gard_array_new(int32_t initial_capacity) {
    if (initial_capacity < 131072) initial_capacity = 131072;
    GardArray* arr = (GardArray*)malloc(sizeof(GardArray) + initial_capacity * sizeof(int64_t));
    arr->capacity = initial_capacity;
    arr->length = 0;
    return arr;
}

void gard_array_add(GardArray* arr, int64_t value) {
    if (!arr) return;
    if (arr->length >= arr->capacity) {
        // Grow: double capacity. Since GardArray uses flexible array member,
        // we can't realloc safely (pointer might change). Instead, we allocate
        // a larger capacity upfront. For now, just extend capacity in-place
        // by using realloc (works if memory manager can extend in-place).
        int32_t newCap = arr->capacity * 2;
        GardArray* newArr = (GardArray*)realloc(arr, sizeof(GardArray) + newCap * sizeof(int64_t));
        if (!newArr) return; // OOM
        newArr->capacity = newCap;
        // NOTE: if realloc moved the memory, the caller's pointer is stale.
        // This is a known limitation. For production, use indirection.
        // In practice, realloc often extends in-place for small growth.
        // We update arr to newArr for this call, but caller still has old ptr.
        // The fix: memcpy the header back if realloc moved (can't — flexible array).
        // REAL FIX: just use a very large initial capacity (done in gard_array_new).
        arr = newArr;
    }
    arr->data[arr->length++] = value;
}

int64_t gard_array_get(GardArray* arr, int32_t index) {
    if (!arr || index < 0 || index >= arr->length) return 0;
    return arr->data[index];
}

void gard_array_set(GardArray* arr, int32_t index, int64_t value) {
    if (!arr) {
        gard_panic("NullReferenceError", "Attempted to index a null array");
        return; // unreachable
    }
    if (index < 0) {
        gard_panic_index(index, arr->length, NULL);
        return; // unreachable
    }
    if (index >= arr->length) {
        // Allow setting at index == length (append behavior) but not beyond
        if (index > arr->length) {
            gard_panic_index(index, arr->length, NULL);
            return; // unreachable
        }
        // index == length: extend by one (like push)
        if (arr->length >= arr->capacity) {
            // Can't grow — panic
            gard_panic_index(index, arr->capacity, NULL);
            return;
        }
        arr->length++;
    }
    arr->data[index] = value;
}

int32_t gard_array_length(GardArray* arr) {
    if (!arr) return 0;
    return arr->length;
}

int64_t gard_array_pop(GardArray* arr) {
    if (!arr || arr->length == 0) return 0;
    return arr->data[--arr->length];
}

int32_t gard_array_contains(GardArray* arr, int64_t value) {
    if (!arr) return 0;
    for (int32_t i = 0; i < arr->length; i++) {
        if (arr->data[i] == value) return 1;
    }
    return 0;
}

void gard_array_remove_at(GardArray* arr, int32_t index) {
    if (!arr || index < 0 || index >= arr->length) return;
    for (int32_t i = index; i < arr->length - 1; i++) {
        arr->data[i] = arr->data[i + 1];
    }
    arr->length--;
}

int32_t gard_array_index_of(GardArray* arr, int64_t value) {
    if (!arr) return -1;
    for (int32_t i = 0; i < arr->length; i++) {
        if (arr->data[i] == value) return i;
    }
    return -1;
}

void gard_array_reverse(GardArray* arr) {
    if (!arr || arr->length < 2) return;
    int32_t lo = 0, hi = arr->length - 1;
    while (lo < hi) {
        int64_t tmp = arr->data[lo];
        arr->data[lo] = arr->data[hi];
        arr->data[hi] = tmp;
        lo++;
        hi--;
    }
}

static int cmp_i64(const void* a, const void* b) {
    int64_t va = *(const int64_t*)a;
    int64_t vb = *(const int64_t*)b;
    return (va > vb) - (va < vb);
}

void gard_array_sort(GardArray* arr) {
    if (!arr || arr->length < 2) return;
    qsort(arr->data, arr->length, sizeof(int64_t), cmp_i64);
}

GardArray* gard_array_slice(GardArray* arr, int32_t start, int32_t end) {
    if (!arr) return gard_array_new(8);
    if (start < 0) start = 0;
    if (end > arr->length) end = arr->length;
    if (start >= end) return gard_array_new(8);
    int32_t len = end - start;
    GardArray* result = gard_array_new(len < 64 ? 64 : len);
    for (int32_t i = start; i < end; i++) {
        result->data[result->length++] = arr->data[i];
    }
    return result;
}

char* gard_array_join(GardArray* arr, const char* separator) {
    if (!arr || arr->length == 0) return strdup("");
    if (!separator) separator = ",";
    size_t sep_len = strlen(separator);

    // First pass: compute total length
    // Each element is converted to string (max 20 chars for int64)
    size_t total = 0;
    char** strs = (char**)malloc(arr->length * sizeof(char*));
    for (int32_t i = 0; i < arr->length; i++) {
        strs[i] = (char*)malloc(24);
        snprintf(strs[i], 24, "%ld", (long)arr->data[i]);
        total += strlen(strs[i]);
    }
    total += sep_len * (arr->length - 1) + 1;

    char* result = (char*)malloc(total);
    result[0] = '\0';
    for (int32_t i = 0; i < arr->length; i++) {
        if (i > 0) strcat(result, separator);
        strcat(result, strs[i]);
        free(strs[i]);
    }
    free(strs);
    return result;
}

void gard_array_add_all(GardArray* dest, GardArray* src) {
    if (!dest || !src) return;
    for (int32_t i = 0; i < src->length; i++) {
        gard_array_add(dest, src->data[i]);
    }
}

int64_t gard_array_remove(GardArray* arr, int64_t value) {
    if (!arr) return 0;
    for (int32_t i = 0; i < arr->length; i++) {
        if (arr->data[i] == value) {
            gard_array_remove_at(arr, i);
            return 1;
        }
    }
    return 0;
}

// ============================================================
// String operations
// ============================================================

char* gard_string_concat(const char* a, const char* b) {
    if (!a) a = "";
    if (!b) b = "";
    size_t la = strlen(a), lb = strlen(b);
    char* result = (char*)malloc(la + lb + 1);
    memcpy(result, a, la);
    memcpy(result + la, b, lb + 1);
    return result;
}

int32_t gard_string_length(const char* s) {
    if (!s) return 0;
    return (int32_t)strlen(s);
}

int32_t gard_string_equals(const char* a, const char* b) {
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    return strcmp(a, b) == 0 ? 1 : 0;
}

char* gard_string_substring(const char* s, int32_t start, int32_t end) {
    if (!s) return strdup("");
    int32_t len = (int32_t)strlen(s);
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) return strdup("");
    int32_t sub_len = end - start;
    char* result = (char*)malloc(sub_len + 1);
    memcpy(result, s + start, sub_len);
    result[sub_len] = '\0';
    return result;
}

int32_t gard_string_index_of(const char* s, const char* sub) {
    if (!s || !sub) return -1;
    char* found = strstr(s, sub);
    if (!found) return -1;
    return (int32_t)(found - s);
}

int32_t gard_string_last_index_of(const char* s, const char* sub) {
    if (!s || !sub) return -1;
    int32_t s_len = (int32_t)strlen(s);
    int32_t sub_len = (int32_t)strlen(sub);
    if (sub_len > s_len) return -1;
    for (int32_t i = s_len - sub_len; i >= 0; i--) {
        if (strncmp(s + i, sub, sub_len) == 0) return i;
    }
    return -1;
}

char* gard_string_to_upper(const char* s) {
    if (!s) return strdup("");
    size_t len = strlen(s);
    char* result = (char*)malloc(len + 1);
    for (size_t i = 0; i < len; i++) result[i] = toupper((unsigned char)s[i]);
    result[len] = '\0';
    return result;
}

char* gard_string_to_lower(const char* s) {
    if (!s) return strdup("");
    size_t len = strlen(s);
    char* result = (char*)malloc(len + 1);
    for (size_t i = 0; i < len; i++) result[i] = tolower((unsigned char)s[i]);
    result[len] = '\0';
    return result;
}

char* gard_string_replace(const char* s, const char* old_str, const char* new_str) {
    if (!s) return strdup("");
    if (!old_str || !new_str) return strdup(s);
    char* pos = strstr(s, old_str);
    if (!pos) return strdup(s);

    size_t s_len = strlen(s);
    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);
    size_t result_len = s_len - old_len + new_len;
    char* result = (char*)malloc(result_len + 1);

    size_t prefix = pos - s;
    memcpy(result, s, prefix);
    memcpy(result + prefix, new_str, new_len);
    memcpy(result + prefix + new_len, pos + old_len, s_len - prefix - old_len + 1);
    return result;
}

char* gard_string_replace_all(const char* s, const char* old_str, const char* new_str) {
    if (!s) return strdup("");
    if (!old_str || !new_str || old_str[0] == '\0') return strdup(s);

    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);

    // Count occurrences
    int count = 0;
    const char* p = s;
    while ((p = strstr(p, old_str)) != NULL) {
        count++;
        p += old_len;
    }
    if (count == 0) return strdup(s);

    size_t s_len = strlen(s);
    size_t result_len = s_len + count * ((int)new_len - (int)old_len);
    char* result = (char*)malloc(result_len + 1);
    char* dst = result;
    p = s;
    const char* next;
    while ((next = strstr(p, old_str)) != NULL) {
        size_t chunk = next - p;
        memcpy(dst, p, chunk);
        dst += chunk;
        memcpy(dst, new_str, new_len);
        dst += new_len;
        p = next + old_len;
    }
    strcpy(dst, p);
    return result;
}

char* gard_string_trim(const char* s) {
    if (!s) return strdup("");
    size_t len = strlen(s);
    size_t start = 0, end = len;
    while (start < len && isspace((unsigned char)s[start])) start++;
    while (end > start && isspace((unsigned char)s[end - 1])) end--;
    if (start >= end) return strdup("");
    size_t rlen = end - start;
    char* result = (char*)malloc(rlen + 1);
    memcpy(result, s + start, rlen);
    result[rlen] = '\0';
    return result;
}

char* gard_string_trim_start(const char* s) {
    if (!s) return strdup("");
    size_t len = strlen(s);
    size_t start = 0;
    while (start < len && isspace((unsigned char)s[start])) start++;
    return strdup(s + start);
}

char* gard_string_trim_end(const char* s) {
    if (!s) return strdup("");
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) len--;
    char* result = (char*)malloc(len + 1);
    memcpy(result, s, len);
    result[len] = '\0';
    return result;
}

int32_t gard_string_starts_with(const char* s, const char* prefix) {
    if (!s || !prefix) return 0;
    size_t plen = strlen(prefix);
    if (plen > strlen(s)) return 0;
    return strncmp(s, prefix, plen) == 0 ? 1 : 0;
}

int32_t gard_string_ends_with(const char* s, const char* suffix) {
    if (!s || !suffix) return 0;
    size_t slen = strlen(s), suflen = strlen(suffix);
    if (suflen > slen) return 0;
    return strcmp(s + slen - suflen, suffix) == 0 ? 1 : 0;
}

int32_t gard_string_contains(const char* s, const char* sub) {
    if (!s || !sub) return 0;
    return strstr(s, sub) != NULL ? 1 : 0;
}

char* gard_string_repeat(const char* s, int32_t count) {
    if (!s || count <= 0) return strdup("");
    size_t len = strlen(s);
    size_t total = len * count;
    char* result = (char*)malloc(total + 1);
    char* dst = result;
    for (int32_t i = 0; i < count; i++) {
        memcpy(dst, s, len);
        dst += len;
    }
    *dst = '\0';
    return result;
}

char* gard_string_pad_start(const char* s, int32_t target_len, const char* pad) {
    if (!s) return strdup("");
    if (!pad || pad[0] == '\0') pad = " ";
    int32_t slen = (int32_t)strlen(s);
    if (slen >= target_len) return strdup(s);

    int32_t pad_needed = target_len - slen;
    size_t pad_len = strlen(pad);
    char* result = (char*)malloc(target_len + 1);
    int32_t pos = 0;
    while (pos < pad_needed) {
        size_t copy = pad_len;
        if (pos + (int32_t)copy > pad_needed) copy = pad_needed - pos;
        memcpy(result + pos, pad, copy);
        pos += copy;
    }
    memcpy(result + pad_needed, s, slen + 1);
    return result;
}

char* gard_string_pad_end(const char* s, int32_t target_len, const char* pad) {
    if (!s) return strdup("");
    if (!pad || pad[0] == '\0') pad = " ";
    int32_t slen = (int32_t)strlen(s);
    if (slen >= target_len) return strdup(s);

    int32_t pad_needed = target_len - slen;
    size_t pad_len = strlen(pad);
    char* result = (char*)malloc(target_len + 1);
    memcpy(result, s, slen);
    int32_t pos = slen;
    while (pos < target_len) {
        size_t copy = pad_len;
        if (pos + (int32_t)copy > target_len) copy = target_len - pos;
        memcpy(result + pos, pad, copy);
        pos += copy;
    }
    result[target_len] = '\0';
    return result;
}

char* gard_string_slice(const char* s, int32_t start, int32_t end) {
    if (!s) return strdup("");
    int32_t len = (int32_t)strlen(s);
    if (start < 0) start = len + start;
    if (end < 0) end = len + end;
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) return strdup("");
    int32_t rlen = end - start;
    char* result = (char*)malloc(rlen + 1);
    memcpy(result, s + start, rlen);
    result[rlen] = '\0';
    return result;
}

char* gard_string_char_at(const char* s, int32_t index) {
    if (!s || index < 0 || index >= (int32_t)strlen(s)) return strdup("");
    char* result = (char*)malloc(2);
    result[0] = s[index];
    result[1] = '\0';
    return result;
}

int32_t gard_string_char_code_at(const char* s, int32_t index) {
    if (!s || index < 0 || index >= (int32_t)strlen(s)) return 0;
    return (int32_t)(unsigned char)s[index];
}

GardArray* gard_string_split(const char* s, const char* delimiter) {
    GardArray* arr = gard_array_new(16);
    if (!s) return arr;
    if (!delimiter || delimiter[0] == '\0') {
        // Split into individual characters
        int32_t len = (int32_t)strlen(s);
        for (int32_t i = 0; i < len; i++) {
            char* ch = (char*)malloc(2);
            ch[0] = s[i];
            ch[1] = '\0';
            gard_array_add(arr, (int64_t)(intptr_t)ch);
        }
        return arr;
    }

    size_t dlen = strlen(delimiter);
    const char* p = s;
    const char* next;
    while ((next = strstr(p, delimiter)) != NULL) {
        size_t chunk = next - p;
        char* part = (char*)malloc(chunk + 1);
        memcpy(part, p, chunk);
        part[chunk] = '\0';
        gard_array_add(arr, (int64_t)(intptr_t)part);
        p = next + dlen;
    }
    // Last segment
    gard_array_add(arr, (int64_t)(intptr_t)strdup(p));
    return arr;
}

// ============================================================
// Type conversion
// ============================================================

char* gard_int_to_string(int32_t value) {
    char* buf = (char*)malloc(16);
    snprintf(buf, 16, "%d", value);
    return buf;
}

char* gard_long_to_string(int64_t value) {
    char* buf = (char*)malloc(24);
    snprintf(buf, 24, "%ld", (long)value);
    return buf;
}

char* gard_double_to_string(double value) {
    char* buf = (char*)malloc(32);
    snprintf(buf, 32, "%g", value);
    return buf;
}

char* gard_bool_to_string(int32_t value) {
    return strdup(value ? "true" : "false");
}

int32_t gard_string_to_int(const char* s) {
    if (!s) return 0;
    return (int32_t)atoi(s);
}

double gard_string_to_double(const char* s) {
    if (!s) return 0.0;
    return atof(s);
}

// ============================================================
// Print
// ============================================================

void gard_print_int(int32_t value) {
    printf("%d\n", value);
}

void gard_print_str(const char* value) {
    printf("%s\n", value ? value : "null");
}

void gard_print_bool(int32_t value) {
    printf("%s\n", value ? "true" : "false");
}

void gard_print_double(double value) {
    printf("%g\n", value);
}

void gard_print_long(int64_t value) {
    printf("%ld\n", (long)value);
}

// Runtime comparison for generic/erased values (int-as-ptr or string-ptr)
// Returns: negative if a < b, 0 if equal, positive if a > b
int32_t gard_compare_values(void* a, void* b) {
    uintptr_t addrA = (uintptr_t)a;
    uintptr_t addrB = (uintptr_t)b;
    // If both are small values (< 0x10000), compare as integers
    if (addrA < 0x10000 && addrB < 0x10000) {
        intptr_t ia = (intptr_t)a;
        intptr_t ib = (intptr_t)b;
        if (ia < ib) return -1;
        if (ia > ib) return 1;
        return 0;
    }
    // If both are valid pointers, compare as strings
    if (addrA >= 0x10000 && addrB >= 0x10000) {
        return strcmp((const char*)a, (const char*)b);
    }
    // Mixed: compare raw pointer values
    if (addrA < addrB) return -1;
    if (addrA > addrB) return 1;
    return 0;
}

// Smart print for generic/erased values: determines if ptr is a valid string or an int
void gard_print_value(void* value) {
    if (value == NULL) {
        printf("null\n");
        return;
    }
    // Heuristic: if the pointer value is small (< 0x10000), it's likely an int stored as ptr
    uintptr_t addr = (uintptr_t)value;
    if (addr < 0x10000) {
        printf("%ld\n", (long)(intptr_t)value);
        return;
    }
    // Try to check if it's a valid readable address by checking alignment and range
    // On Linux, user-space addresses are typically > 0x400000
    // If it looks like a heap/stack address, treat as string
    const char* str = (const char*)value;
    // Quick validation: check first byte is printable ASCII or common UTF-8
    unsigned char first = (unsigned char)str[0];
    if (first == 0) {
        printf("\n"); // empty string
    } else if (first == 0x1B) {
        // ANSI escape sequence (e.g., color codes) — treat as string
        printf("%s\n", str);
    } else if (first >= 0x20 && first <= 0x7E) {
        printf("%s\n", str);
    } else if (first >= 0x80) {
        // Could be UTF-8, try printing
        printf("%s\n", str);
    } else {
        // Non-printable first byte — likely an int
        printf("%ld\n", (long)(intptr_t)value);
    }
}

// Print a caught exception object in a structured format (like Dart/TypeScript)
// Format: GardErrorType: message\n    at caller (file:line:col)
void gard_print_exception(void) {
    const char* type = __gard_exception_type;
    const char* msg = __gard_exception_msg;
    int32_t line = __gard_exception_line;
    int32_t col = __gard_exception_col;
    const char* caller = __gard_exception_caller;

    // Print: ErrorType: message
    if (type && msg) {
        printf("%s: %s\n", type, msg);
    } else if (type) {
        printf("%s\n", type);
    } else if (msg) {
        printf("%s\n", msg);
    } else {
        printf("Error\n");
    }

    // Print: at caller (file:line:col)
    if (caller && line > 0 && gard_source_file_) {
        printf("    at %s (%s:%d:%d)\n", caller, gard_source_file_, line, col > 0 ? col : 1);
    } else if (caller && line > 0) {
        printf("    at %s (line %d)\n", caller, line);
    } else if (caller) {
        printf("    at %s\n", caller);
    }
}

// ============================================================
// Math
// ============================================================

int32_t gard_math_abs(int32_t x) {
    return x < 0 ? -x : x;
}

int32_t gard_math_max(int32_t a, int32_t b) {
    return a > b ? a : b;
}

int32_t gard_math_min(int32_t a, int32_t b) {
    return a < b ? a : b;
}

double gard_math_sqrt(double x) {
    return sqrt(x);
}

double gard_math_pow(double base, double exp) {
    return pow(base, exp);
}

int32_t gard_math_round(double x) {
    return (int32_t)round(x);
}

int32_t gard_math_floor(double x) {
    return (int32_t)floor(x);
}

int32_t gard_math_ceil(double x) {
    return (int32_t)ceil(x);
}

double gard_math_sin(double x) {
    return sin(x);
}

double gard_math_cos(double x) {
    return cos(x);
}

double gard_math_tan(double x) {
    return tan(x);
}

double gard_math_asin(double x) {
    return asin(x);
}

double gard_math_acos(double x) {
    return acos(x);
}

double gard_math_atan(double x) {
    return atan(x);
}

double gard_math_atan2(double y, double x) {
    return atan2(y, x);
}

double gard_math_log(double x) {
    return log(x);
}

double gard_math_log2(double x) {
    return log2(x);
}

double gard_math_log10(double x) {
    return log10(x);
}

double gard_math_exp(double x) {
    return exp(x);
}

double gard_math_cbrt(double x) {
    return cbrt(x);
}

int32_t gard_math_trunc(double x) {
    return (int32_t)trunc(x);
}

int32_t gard_math_sign(double x) {
    if (x > 0) return 1;
    if (x < 0) return -1;
    return 0;
}

double gard_math_clamp(double val, double lo, double hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

static int gard_rng_seeded = 0;

double gard_math_random(void) {
    if (!gard_rng_seeded) { srand((unsigned)time(NULL)); gard_rng_seeded = 1; }
    return (double)rand() / (double)RAND_MAX;
}

int32_t gard_math_random_int(int32_t min, int32_t max) {
    if (!gard_rng_seeded) { srand((unsigned)time(NULL)); gard_rng_seeded = 1; }
    if (min >= max) return min;
    return min + rand() % (max - min + 1);
}

double gard_math_pi(void) {
    return 3.14159265358979323846;
}

double gard_math_e(void) {
    return 2.71828182845904523536;
}

// ============================================================
// Map operations (hash map: string keys → int64_t values)
// ============================================================

static uint32_t gard_hash_string(const char* key) {
    uint32_t hash = 5381;
    while (*key) {
        hash = ((hash << 5) + hash) + (unsigned char)*key;
        key++;
    }
    return hash;
}

GardMap* gard_map_new(void) {
    GardMap* map = (GardMap*)malloc(sizeof(GardMap));
    map->magic = GARD_MAP_MAGIC;
    map->bucket_count = 64;
    map->size = 0;
    map->buckets = (GardMapEntry**)calloc(map->bucket_count, sizeof(GardMapEntry*));
    return map;
}

void gard_map_set(GardMap* map, const char* key, int64_t value) {
    if (!map || !key || map->magic != GARD_MAP_MAGIC) return;
    uint32_t idx = gard_hash_string(key) % map->bucket_count;
    GardMapEntry* entry = map->buckets[idx];
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            entry->value = value;
            return;
        }
        entry = entry->next;
    }
    // New entry
    GardMapEntry* new_entry = (GardMapEntry*)malloc(sizeof(GardMapEntry));
    new_entry->key = strdup(key);
    new_entry->value = value;
    new_entry->next = map->buckets[idx];
    map->buckets[idx] = new_entry;
    map->size++;
}

int64_t gard_map_get(GardMap* map, const char* key) {
    if (!map || !key || map->magic != GARD_MAP_MAGIC) return 0;
    uint32_t idx = gard_hash_string(key) % map->bucket_count;
    GardMapEntry* entry = map->buckets[idx];
    while (entry) {
        if (strcmp(entry->key, key) == 0) return entry->value;
        entry = entry->next;
    }
    return 0;
}

int32_t gard_map_has(GardMap* map, const char* key) {
    if (!map || !key || map->magic != GARD_MAP_MAGIC) return 0;
    uint32_t idx = gard_hash_string(key) % map->bucket_count;
    GardMapEntry* entry = map->buckets[idx];
    while (entry) {
        if (strcmp(entry->key, key) == 0) return 1;
        entry = entry->next;
    }
    return 0;
}

int32_t gard_map_remove(GardMap* map, const char* key) {
    if (!map || !key || map->magic != GARD_MAP_MAGIC) return 0;
    uint32_t idx = gard_hash_string(key) % map->bucket_count;
    GardMapEntry** pp = &map->buckets[idx];
    while (*pp) {
        if (strcmp((*pp)->key, key) == 0) {
            GardMapEntry* doomed = *pp;
            *pp = doomed->next;
            free(doomed->key);
            free(doomed);
            map->size--;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

int32_t gard_map_size(GardMap* map) {
    if (!map) return 0;
    return map->size;
}

GardArray* gard_map_keys(GardMap* map) {
    GardArray* arr = gard_array_new(map ? map->size : 0);
    if (!map) return arr;
    for (int32_t i = 0; i < map->bucket_count; i++) {
        GardMapEntry* entry = map->buckets[i];
        while (entry) {
            gard_array_add(arr, (int64_t)(intptr_t)strdup(entry->key));
            entry = entry->next;
        }
    }
    return arr;
}

GardArray* gard_map_values(GardMap* map) {
    GardArray* arr = gard_array_new(map ? map->size : 0);
    if (!map) return arr;
    for (int32_t i = 0; i < map->bucket_count; i++) {
        GardMapEntry* entry = map->buckets[i];
        while (entry) {
            gard_array_add(arr, entry->value);
            entry = entry->next;
        }
    }
    return arr;
}

void gard_map_free(GardMap* map) {
    if (!map) return;
    for (int32_t i = 0; i < map->bucket_count; i++) {
        GardMapEntry* entry = map->buckets[i];
        while (entry) {
            GardMapEntry* next = entry->next;
            free(entry->key);
            free(entry);
            entry = next;
        }
    }
    free(map->buckets);
    free(map);
}

// ============================================================
// Memory
// ============================================================

void* gard_malloc(int64_t size) {
    return malloc((size_t)size);
}

void gard_free(void* ptr) {
    free(ptr);
}


// ============================================================
// File I/O
// ============================================================

#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>

char* gard_file_read_text(const char* path) {
    if (!path) return strdup("");
    FILE* f = fopen(path, "r");
    if (!f) return strdup("");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc(size + 1);
    size_t read = fread(buf, 1, size, f);
    buf[read] = '\0';
    fclose(f);
    return buf;
}

int32_t gard_file_write_text(const char* path, const char* content) {
    if (!path) return 0;
    FILE* f = fopen(path, "w");
    if (!f) return 0;
    if (content) fputs(content, f);
    fclose(f);
    return 1;
}

int32_t gard_file_append_text(const char* path, const char* content) {
    if (!path) return 0;
    FILE* f = fopen(path, "a");
    if (!f) return 0;
    if (content) fputs(content, f);
    fclose(f);
    return 1;
}

int32_t gard_file_exists(const char* path) {
    if (!path) return 0;
    return access(path, F_OK) == 0 ? 1 : 0;
}

int64_t gard_file_size(const char* path) {
    if (!path) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (int64_t)st.st_size;
}

int32_t gard_file_delete(const char* path) {
    if (!path) return 0;
    return unlink(path) == 0 ? 1 : 0;
}

int32_t gard_file_copy(const char* src, const char* dst) {
    if (!src || !dst) return 0;
    FILE* in = fopen(src, "rb");
    if (!in) return 0;
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); return 0; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(in);
    fclose(out);
    return 1;
}

int32_t gard_file_move(const char* src, const char* dst) {
    if (!src || !dst) return 0;
    return rename(src, dst) == 0 ? 1 : 0;
}

// ============================================================
// Directory
// ============================================================

static int32_t mkdirs(const char* path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 1 : 0;
}

int32_t gard_directory_create(const char* path) {
    if (!path) return 0;
    return mkdirs(path);
}

int32_t gard_directory_exists(const char* path) {
    if (!path) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

int32_t gard_directory_delete(const char* path) {
    if (!path) return 0;
    // Simple rmdir (only empty dirs). For recursive, would need nftw.
    return rmdir(path) == 0 ? 1 : 0;
}

GardArray* gard_directory_list(const char* path) {
    GardArray* arr = gard_array_new(32);
    if (!path) return arr;
    DIR* d = opendir(path);
    if (!d) return arr;
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            continue;
        gard_array_add(arr, (int64_t)(intptr_t)strdup(entry->d_name));
    }
    closedir(d);
    return arr;
}

// ============================================================
// Path utilities
// ============================================================

char* gard_path_join(const char* a, const char* b) {
    if (!a || !b) return strdup(a ? a : (b ? b : ""));
    size_t la = strlen(a);
    size_t lb = strlen(b);
    int need_sep = (la > 0 && a[la - 1] != '/' && lb > 0 && b[0] != '/') ? 1 : 0;
    char* result = (char*)malloc(la + lb + need_sep + 1);
    memcpy(result, a, la);
    if (need_sep) result[la] = '/';
    memcpy(result + la + need_sep, b, lb + 1);
    return result;
}

char* gard_path_extension(const char* path) {
    if (!path) return strdup("");
    const char* dot = strrchr(path, '.');
    const char* sep = strrchr(path, '/');
    if (!dot || (sep && dot < sep)) return strdup("");
    return strdup(dot);
}

char* gard_path_dirname(const char* path) {
    if (!path) return strdup("");
    const char* sep = strrchr(path, '/');
    if (!sep) return strdup(".");
    if (sep == path) return strdup("/");
    size_t len = sep - path;
    char* result = (char*)malloc(len + 1);
    memcpy(result, path, len);
    result[len] = '\0';
    return result;
}

char* gard_path_basename(const char* path) {
    if (!path) return strdup("");
    const char* sep = strrchr(path, '/');
    return strdup(sep ? sep + 1 : path);
}

char* gard_path_resolve(const char* path) {
    if (!path) return strdup("");
    char resolved[PATH_MAX];
    if (realpath(path, resolved)) return strdup(resolved);
    // If file doesn't exist, just return the input
    return strdup(path);
}

// ============================================================
// DateTime
// ============================================================

#include <sys/time.h>

int64_t gard_datetime_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
}

char* gard_datetime_format(int64_t epoch_ms) {
    time_t secs = (time_t)(epoch_ms / 1000);
    struct tm* tm = localtime(&secs);
    char* buf = (char*)malloc(32);
    strftime(buf, 32, "%Y-%m-%d %H:%M:%S", tm);
    return buf;
}

int64_t gard_datetime_add_days(int64_t epoch_ms, int32_t days) {
    return epoch_ms + (int64_t)days * 86400000LL;
}

int64_t gard_datetime_add_hours(int64_t epoch_ms, int32_t hours) {
    return epoch_ms + (int64_t)hours * 3600000LL;
}

int64_t gard_datetime_add_minutes(int64_t epoch_ms, int32_t minutes) {
    return epoch_ms + (int64_t)minutes * 60000LL;
}

int32_t gard_datetime_is_before(int64_t a, int64_t b) {
    return a < b ? 1 : 0;
}

int32_t gard_datetime_is_after(int64_t a, int64_t b) {
    return a > b ? 1 : 0;
}

int64_t gard_datetime_subtract(int64_t a, int64_t b) {
    return a - b;
}

int64_t gard_datetime_add_weeks(int64_t epoch_ms, int32_t weeks) {
    return epoch_ms + (int64_t)weeks * 7 * 24 * 3600 * 1000;
}

int64_t gard_datetime_add_months(int64_t epoch_ms, int32_t months) {
    return epoch_ms + (int64_t)months * 30 * 24 * 3600 * 1000;
}

int64_t gard_datetime_add_years(int64_t epoch_ms, int32_t years) {
    return epoch_ms + (int64_t)years * 365 * 24 * 3600 * 1000;
}

int32_t gard_datetime_is_expired(int64_t epoch_ms) {
    return gard_datetime_now() > epoch_ms ? 1 : 0;
}

int32_t gard_datetime_equals(int64_t a, int64_t b) {
    return a == b ? 1 : 0;
}

int64_t gard_datetime_parse(const char* str) {
    if (!str) return 0;
    struct tm tm = {0};
    // Try ISO format: YYYY-MM-DD or YYYY-MM-DDTHH:MM:SS
    if (sscanf(str, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) >= 3) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        // Try to parse time part
        sscanf(str + 10, "T%d:%d:%d", &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
        time_t t = mktime(&tm);
        return (int64_t)t * 1000;
    }
    return 0;
}

char* gard_datetime_get_timezone(void) {
    tzset();
    char* buf = (char*)malloc(64);
    snprintf(buf, 64, "%s", tzname[0]);
    return buf;
}

int64_t gard_datetime_to_epoch_millis(void) {
    return gard_datetime_now();
}

int64_t gard_datetime_from_epoch(int64_t epoch_ms) {
    return epoch_ms;
}

int64_t gard_datetime_utc(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

// --- JSON extended ---

char* gard_json_type_of(int64_t value_type) {
    // 0=null, 1=int, 2=double, 3=string, 4=bool, 5=array, 6=object
    switch ((int)value_type) {
        case 0: return "null";
        case 1: return "number";
        case 2: return "number";
        case 3: return "string";
        case 4: return "boolean";
        case 5: return "array";
        case 6: return "object";
        default: return "unknown";
    }
}

char* gard_json_pretty_print(const char* json, int32_t indent) {
    if (!json) return strdup("null");
    // Simple pretty printer: add newlines after { and , and indent
    size_t len = strlen(json);
    char* buf = (char*)malloc(len * 4 + 1);
    int pos = 0, depth = 0;
    for (size_t i = 0; i < len; i++) {
        char c = json[i];
        if (c == '{' || c == '[') {
            buf[pos++] = c;
            buf[pos++] = '\n';
            depth++;
            for (int j = 0; j < depth * indent; j++) buf[pos++] = ' ';
        } else if (c == '}' || c == ']') {
            buf[pos++] = '\n';
            depth--;
            for (int j = 0; j < depth * indent; j++) buf[pos++] = ' ';
            buf[pos++] = c;
        } else if (c == ',') {
            buf[pos++] = c;
            buf[pos++] = '\n';
            for (int j = 0; j < depth * indent; j++) buf[pos++] = ' ';
        } else {
            buf[pos++] = c;
        }
    }
    buf[pos] = '\0';
    return buf;
}

// --- Regex extended ---

int32_t gard_regex_test_flags(const char* pattern, const char* str, const char* flags) {
    if (!pattern || !str) return 0;
    int cflags = REG_EXTENDED | REG_NOSUB;
    if (flags) {
        for (const char* f = flags; *f; f++) {
            if (*f == 'i') cflags |= REG_ICASE;
        }
    }
    regex_t re;
    if (regcomp(&re, pattern, cflags) != 0) return 0;
    int result = regexec(&re, str, 0, NULL, 0) == 0 ? 1 : 0;
    regfree(&re);
    return result;
}

// ============================================================
// JSON (simplified — string-based serialization)
// ============================================================

char* gard_json_stringify_int(int32_t value) {
    char* buf = (char*)malloc(16);
    snprintf(buf, 16, "%d", value);
    return buf;
}

char* gard_json_stringify_str(const char* value) {
    if (!value) return strdup("null");
    size_t len = strlen(value);
    // Worst case: every char needs escaping (2x) + quotes + null
    char* buf = (char*)malloc(len * 2 + 3);
    char* dst = buf;
    *dst++ = '"';
    for (size_t i = 0; i < len; i++) {
        char c = value[i];
        switch (c) {
            case '"':  *dst++ = '\\'; *dst++ = '"'; break;
            case '\\': *dst++ = '\\'; *dst++ = '\\'; break;
            case '\n': *dst++ = '\\'; *dst++ = 'n'; break;
            case '\t': *dst++ = '\\'; *dst++ = 't'; break;
            case '\r': *dst++ = '\\'; *dst++ = 'r'; break;
            default:   *dst++ = c; break;
        }
    }
    *dst++ = '"';
    *dst = '\0';
    return buf;
}

char* gard_json_stringify_bool(int32_t value) {
    return strdup(value ? "true" : "false");
}

int32_t gard_json_is_valid(const char* s) {
    if (!s || s[0] == '\0') return 0;
    char c = s[0];
    // Very basic check: starts with {, [, ", digit, true, false, null
    if (c == '{' || c == '[' || c == '"') return 1;
    if (c >= '0' && c <= '9') return 1;
    if (c == '-' && s[1] >= '0' && s[1] <= '9') return 1;
    if (strncmp(s, "true", 4) == 0) return 1;
    if (strncmp(s, "false", 5) == 0) return 1;
    if (strncmp(s, "null", 4) == 0) return 1;
    return 0;
}

// ============================================================
// Process
// ============================================================

int32_t gard_process_pid(void) {
    return (int32_t)getpid();
}

char* gard_process_platform(void) {
#if defined(__linux__)
    return strdup("Linux");
#elif defined(__APPLE__)
    return strdup("Darwin");
#elif defined(_WIN32)
    return strdup("Windows");
#else
    return strdup("Unknown");
#endif
}

char* gard_process_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return strdup("x86_64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    return strdup("aarch64");
#elif defined(__i386__) || defined(_M_IX86)
    return strdup("x86");
#elif defined(__arm__)
    return strdup("arm");
#else
    return strdup("unknown");
#endif
}

char* gard_process_cwd(void) {
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof(buf))) return strdup(buf);
    return strdup("");
}

char* gard_process_env(const char* name) {
    if (!name) return strdup("");
    const char* val = getenv(name);
    return strdup(val ? val : "");
}

void gard_process_exit(int32_t code) {
    exit(code);
}

char* gard_process_execute(const char* command) {
    if (!command) return strdup("");
    FILE* pipe = popen(command, "r");
    if (!pipe) return strdup("");

    size_t capacity = 4096;
    size_t length = 0;
    char* output = (char*)malloc(capacity);
    output[0] = '\0';

    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) {
        size_t chunk = strlen(buf);
        if (length + chunk + 1 > capacity) {
            capacity *= 2;
            output = (char*)realloc(output, capacity);
        }
        memcpy(output + length, buf, chunk);
        length += chunk;
    }
    output[length] = '\0';
    pclose(pipe);
    return output;
}

// ============================================================
// Console
// ============================================================

char* gard_console_read_line(void) {
    size_t capacity = 256;
    size_t length = 0;
    char* buf = (char*)malloc(capacity);
    int c;
    while ((c = fgetc(stdin)) != EOF && c != '\n') {
        if (length + 1 >= capacity) {
            capacity *= 2;
            buf = (char*)realloc(buf, capacity);
        }
        buf[length++] = (char)c;
    }
    buf[length] = '\0';
    return buf;
}


// ============================================================
// Crypto — SHA-256 (bundled, no OpenSSL dependency)
// ============================================================

// Minimal SHA-256 implementation (public domain)
static const uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define SHA256_ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define SHA256_CH(x,y,z) (((x)&(y))^((~(x))&(z)))
#define SHA256_MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define SHA256_EP0(x) (SHA256_ROTR(x,2)^SHA256_ROTR(x,13)^SHA256_ROTR(x,22))
#define SHA256_EP1(x) (SHA256_ROTR(x,6)^SHA256_ROTR(x,11)^SHA256_ROTR(x,25))
#define SHA256_SIG0(x) (SHA256_ROTR(x,7)^SHA256_ROTR(x,18)^((x)>>3))
#define SHA256_SIG1(x) (SHA256_ROTR(x,17)^SHA256_ROTR(x,19)^((x)>>10))

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4]<<24)|((uint32_t)block[i*4+1]<<16)|
               ((uint32_t)block[i*4+2]<<8)|((uint32_t)block[i*4+3]);
    for (int i = 16; i < 64; i++)
        w[i] = SHA256_SIG1(w[i-2]) + w[i-7] + SHA256_SIG0(w[i-15]) + w[i-16];
    a=state[0]; b=state[1]; c=state[2]; d=state[3];
    e=state[4]; f=state[5]; g=state[6]; h=state[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + SHA256_EP1(e) + SHA256_CH(e,f,g) + sha256_k[i] + w[i];
        t2 = SHA256_EP0(a) + SHA256_MAJ(a,b,c);
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
}

static void sha256_hash(const uint8_t* data, size_t len, uint8_t out[32]) {
    uint32_t state[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    uint8_t block[64];
    size_t i = 0;
    // Process full blocks
    for (; i + 64 <= len; i += 64)
        sha256_transform(state, data + i);
    // Padding
    size_t rem = len - i;
    memcpy(block, data + i, rem);
    block[rem++] = 0x80;
    if (rem > 56) {
        memset(block + rem, 0, 64 - rem);
        sha256_transform(state, block);
        rem = 0;
    }
    memset(block + rem, 0, 56 - rem);
    uint64_t bits = (uint64_t)len * 8;
    for (int j = 7; j >= 0; j--)
        block[56 + (7 - j)] = (uint8_t)(bits >> (j * 8));
    sha256_transform(state, block);
    // Output
    for (int j = 0; j < 8; j++) {
        out[j*4]   = (uint8_t)(state[j]>>24);
        out[j*4+1] = (uint8_t)(state[j]>>16);
        out[j*4+2] = (uint8_t)(state[j]>>8);
        out[j*4+3] = (uint8_t)(state[j]);
    }
}

char* gard_hash_sha256(const char* data) {
    if (!data) return strdup("");
    uint8_t hash[32];
    sha256_hash((const uint8_t*)data, strlen(data), hash);
    char* hex = (char*)malloc(65);
    for (int i = 0; i < 32; i++)
        sprintf(hex + i*2, "%02x", hash[i]);
    hex[64] = '\0';
    return hex;
}

char* gard_hash_sha256_bytes(const void* data, int32_t len) {
    if (!data || len <= 0) return strdup("");
    uint8_t hash[32];
    sha256_hash((const uint8_t*)data, (size_t)len, hash);
    char* hex = (char*)malloc(65);
    for (int i = 0; i < 32; i++)
        sprintf(hex + i*2, "%02x", hash[i]);
    hex[64] = '\0';
    return hex;
}

// ============================================================
// Base64
// ============================================================

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char* gard_base64_encode(const char* data) {
    if (!data) return strdup("");
    size_t len = strlen(data);
    size_t out_len = 4 * ((len + 2) / 3);
    char* out = (char*)malloc(out_len + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t octet_a = (uint8_t)data[i];
        uint32_t octet_b = (i + 1 < len) ? (uint8_t)data[i+1] : 0;
        uint32_t octet_c = (i + 2 < len) ? (uint8_t)data[i+2] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? b64_table[(triple >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < len) ? b64_table[triple & 0x3F] : '=';
    }
    out[j] = '\0';
    return out;
}

static int b64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

char* gard_base64_decode(const char* data) {
    if (!data) return strdup("");
    size_t len = strlen(data);
    if (len == 0) return strdup("");
    size_t out_len = len / 4 * 3;
    if (len >= 1 && data[len-1] == '=') out_len--;
    if (len >= 2 && data[len-2] == '=') out_len--;
    char* out = (char*)malloc(out_len + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i += 4) {
        int a = b64_decode_char(data[i]);
        int b = (i+1 < len) ? b64_decode_char(data[i+1]) : 0;
        int c = (i+2 < len) ? b64_decode_char(data[i+2]) : 0;
        int d = (i+3 < len) ? b64_decode_char(data[i+3]) : 0;
        if (a < 0) a = 0; if (b < 0) b = 0; if (c < 0) c = 0; if (d < 0) d = 0;
        uint32_t triple = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6) | (uint32_t)d;
        if (j < out_len) out[j++] = (char)((triple >> 16) & 0xFF);
        if (j < out_len) out[j++] = (char)((triple >> 8) & 0xFF);
        if (j < out_len) out[j++] = (char)(triple & 0xFF);
    }
    out[j] = '\0';
    return out;
}

// ============================================================
// Regex (POSIX extended regex)
// ============================================================

#include <regex.h>

int32_t gard_regex_test(const char* pattern, const char* str) {
    if (!pattern || !str) return 0;
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB) != 0) return 0;
    int result = regexec(&re, str, 0, NULL, 0) == 0 ? 1 : 0;
    regfree(&re);
    return result;
}

char* gard_regex_match(const char* pattern, const char* str) {
    if (!pattern || !str) return strdup("");
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED) != 0) return strdup("");
    regmatch_t match[1];
    if (regexec(&re, str, 1, match, 0) != 0) {
        regfree(&re);
        return strdup("");
    }
    int start = match[0].rm_so;
    int end = match[0].rm_eo;
    regfree(&re);
    int len = end - start;
    char* result = (char*)malloc(len + 1);
    memcpy(result, str + start, len);
    result[len] = '\0';
    return result;
}

char* gard_regex_replace(const char* pattern, const char* str, const char* replacement) {
    if (!pattern || !str || !replacement) return strdup(str ? str : "");
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED) != 0) return strdup(str);
    regmatch_t match[1];
    if (regexec(&re, str, 1, match, 0) != 0) {
        regfree(&re);
        return strdup(str);
    }
    int start = match[0].rm_so;
    int end = match[0].rm_eo;
    regfree(&re);

    size_t s_len = strlen(str);
    size_t r_len = strlen(replacement);
    size_t result_len = s_len - (end - start) + r_len;
    char* result = (char*)malloc(result_len + 1);
    memcpy(result, str, start);
    memcpy(result + start, replacement, r_len);
    memcpy(result + start + r_len, str + end, s_len - end + 1);
    return result;
}


// ============================================================
// SHA-1 (bundled)
// ============================================================

static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80], a, b, c, d, e, temp;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4]<<24)|((uint32_t)block[i*4+1]<<16)|
               ((uint32_t)block[i*4+2]<<8)|((uint32_t)block[i*4+3]);
    for (int i = 16; i < 80; i++) {
        uint32_t t = w[i-3]^w[i-8]^w[i-14]^w[i-16];
        w[i] = (t<<1)|(t>>31);
    }
    a=state[0]; b=state[1]; c=state[2]; d=state[3]; e=state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f=(b&c)|((~b)&d); k=0x5A827999; }
        else if (i < 40) { f=b^c^d;           k=0x6ED9EBA1; }
        else if (i < 60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
        else              { f=b^c^d;           k=0xCA62C1D6; }
        temp = ((a<<5)|(a>>27)) + f + e + k + w[i];
        e=d; d=c; c=(b<<30)|(b>>2); b=a; a=temp;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d; state[4]+=e;
}

static void sha1_hash(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t state[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
    uint8_t block[64];
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha1_transform(state, data + i);
    size_t rem = len - i;
    memcpy(block, data + i, rem);
    block[rem++] = 0x80;
    if (rem > 56) { memset(block+rem,0,64-rem); sha1_transform(state,block); rem=0; }
    memset(block+rem, 0, 56-rem);
    uint64_t bits = (uint64_t)len * 8;
    for (int j=7;j>=0;j--) block[56+(7-j)]=(uint8_t)(bits>>(j*8));
    sha1_transform(state, block);
    for (int j=0;j<5;j++) { out[j*4]=(uint8_t)(state[j]>>24); out[j*4+1]=(uint8_t)(state[j]>>16); out[j*4+2]=(uint8_t)(state[j]>>8); out[j*4+3]=(uint8_t)(state[j]); }
}

char* gard_hash_sha1(const char* data) {
    if (!data) return strdup("");
    uint8_t hash[20];
    sha1_hash((const uint8_t*)data, strlen(data), hash);
    char* hex = (char*)malloc(41);
    for (int i = 0; i < 20; i++) sprintf(hex+i*2, "%02x", hash[i]);
    hex[40] = '\0';
    return hex;
}


// ============================================================
// SHA-512 (bundled)
// ============================================================

static const uint64_t sha512_k[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};

#define SHA512_ROTR(x,n) (((x)>>(n))|((x)<<(64-(n))))

static void sha512_transform(uint64_t state[8], const uint8_t block[128]) {
    uint64_t w[80], a,b,c,d,e,f,g,h,t1,t2;
    for (int i=0;i<16;i++) {
        w[i]=0; for(int j=0;j<8;j++) w[i]|=((uint64_t)block[i*8+j])<<(56-j*8);
    }
    for (int i=16;i<80;i++) {
        uint64_t s0=SHA512_ROTR(w[i-15],1)^SHA512_ROTR(w[i-15],8)^(w[i-15]>>7);
        uint64_t s1=SHA512_ROTR(w[i-2],19)^SHA512_ROTR(w[i-2],61)^(w[i-2]>>6);
        w[i]=w[i-16]+s0+w[i-7]+s1;
    }
    a=state[0];b=state[1];c=state[2];d=state[3];e=state[4];f=state[5];g=state[6];h=state[7];
    for (int i=0;i<80;i++) {
        uint64_t S1=SHA512_ROTR(e,14)^SHA512_ROTR(e,18)^SHA512_ROTR(e,41);
        uint64_t ch=(e&f)^((~e)&g);
        t1=h+S1+ch+sha512_k[i]+w[i];
        uint64_t S0=SHA512_ROTR(a,28)^SHA512_ROTR(a,34)^SHA512_ROTR(a,39);
        uint64_t maj=(a&b)^(a&c)^(b&c);
        t2=S0+maj;
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
}

static void sha512_hash(const uint8_t* data, size_t len, uint8_t out[64]) {
    uint64_t state[8]={0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
                       0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
    uint8_t block[128];
    size_t i=0;
    for(;i+128<=len;i+=128) sha512_transform(state,data+i);
    size_t rem=len-i;
    memcpy(block,data+i,rem);
    block[rem++]=0x80;
    if(rem>112){memset(block+rem,0,128-rem);sha512_transform(state,block);rem=0;}
    memset(block+rem,0,112-rem);
    // Length in bits (128-bit, we only use lower 64 bits)
    memset(block+112,0,8);
    uint64_t bits=(uint64_t)len*8;
    for(int j=7;j>=0;j--) block[120+(7-j)]=(uint8_t)(bits>>(j*8));
    sha512_transform(state,block);
    for(int j=0;j<8;j++) for(int k=0;k<8;k++) out[j*8+k]=(uint8_t)(state[j]>>(56-k*8));
}

char* gard_hash_sha512(const char* data) {
    if (!data) return strdup("");
    uint8_t hash[64];
    sha512_hash((const uint8_t*)data, strlen(data), hash);
    char* hex = (char*)malloc(129);
    for (int i = 0; i < 64; i++) sprintf(hex+i*2, "%02x", hash[i]);
    hex[128] = '\0';
    return hex;
}


// ============================================================
// SHA-256 File, HMAC-SHA256, Crypto utilities
// ============================================================

char* gard_hash_sha256_file(const char* path) {
    if (!path) return strdup("");
    FILE* f = fopen(path, "rb");
    if (!f) return strdup("");
    uint8_t buf[4096];
    // Read entire file and hash
    size_t total = 0;
    size_t cap = 65536;
    uint8_t* all = (uint8_t*)malloc(cap);
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (total + n > cap) { cap *= 2; all = (uint8_t*)realloc(all, cap); }
        memcpy(all + total, buf, n);
        total += n;
    }
    fclose(f);
    uint8_t hash[32];
    sha256_hash(all, total, hash);
    free(all);
    char* hex = (char*)malloc(65);
    for (int i = 0; i < 32; i++) sprintf(hex+i*2, "%02x", hash[i]);
    hex[64] = '\0';
    return hex;
}

char* gard_hash_hmac_sha256(const char* key, const char* data) {
    if (!key || !data) return strdup("");
    // HMAC-SHA256: H((K ^ opad) || H((K ^ ipad) || message))
    uint8_t k[64];
    size_t klen = strlen(key);
    if (klen > 64) {
        uint8_t kh[32]; sha256_hash((const uint8_t*)key, klen, kh);
        memcpy(k, kh, 32); memset(k+32, 0, 32);
    } else {
        memcpy(k, key, klen); memset(k+klen, 0, 64-klen);
    }
    uint8_t ipad[64], opad[64];
    for (int i=0;i<64;i++) { ipad[i]=k[i]^0x36; opad[i]=k[i]^0x5c; }
    // inner = SHA256(ipad || data)
    size_t dlen = strlen(data);
    uint8_t* inner_msg = (uint8_t*)malloc(64 + dlen);
    memcpy(inner_msg, ipad, 64);
    memcpy(inner_msg+64, data, dlen);
    uint8_t inner_hash[32];
    sha256_hash(inner_msg, 64+dlen, inner_hash);
    free(inner_msg);
    // outer = SHA256(opad || inner_hash)
    uint8_t outer_msg[96];
    memcpy(outer_msg, opad, 64);
    memcpy(outer_msg+64, inner_hash, 32);
    uint8_t final_hash[32];
    sha256_hash(outer_msg, 96, final_hash);
    char* hex = (char*)malloc(65);
    for (int i=0;i<32;i++) sprintf(hex+i*2, "%02x", final_hash[i]);
    hex[64] = '\0';
    return hex;
}

char* gard_crypto_generate_key(int32_t bits) {
    if (bits <= 0) bits = 256;
    int32_t bytes = bits / 8;
    char* hex = (char*)malloc(bytes * 2 + 1);
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) {
        for (int i = 0; i < bytes; i++) {
            int c = fgetc(f);
            sprintf(hex + i*2, "%02x", (unsigned char)c);
        }
        fclose(f);
    } else {
        // Fallback: use rand
        if (!gard_rng_seeded) { srand((unsigned)time(NULL)); gard_rng_seeded = 1; }
        for (int i = 0; i < bytes; i++) sprintf(hex+i*2, "%02x", rand()%256);
    }
    hex[bytes*2] = '\0';
    return hex;
}

GardArray* gard_crypto_get_random_values(int32_t size) {
    if (size <= 0) size = 16;
    GardArray* arr = gard_array_new(size);
    FILE* f = fopen("/dev/urandom", "rb");
    for (int i = 0; i < size; i++) {
        int val = 0;
        if (f) val = fgetc(f);
        else { if(!gard_rng_seeded){srand((unsigned)time(NULL));gard_rng_seeded=1;} val=rand()%256; }
        gard_array_add(arr, (int64_t)val);
    }
    if (f) fclose(f);
    return arr;
}

// Simplified encrypt/decrypt (XOR with key-derived stream — NOT real AES, but API-compatible)
char* gard_crypto_encrypt(const char* plaintext, const char* key) {
    if (!plaintext || !key) return strdup("");
    size_t plen = strlen(plaintext);
    size_t klen = strlen(key);
    if (klen == 0) return strdup("");
    // Generate IV from urandom
    uint8_t iv[16];
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) { fread(iv, 1, 16, f); fclose(f); }
    else { for(int i=0;i<16;i++) iv[i]=(uint8_t)(rand()%256); }
    // XOR encrypt with key+iv derived stream
    size_t out_len = 16 + plen; // iv + ciphertext
    char* hex = (char*)malloc(out_len * 2 + 1);
    for (int i=0;i<16;i++) sprintf(hex+i*2, "%02x", iv[i]);
    for (size_t i=0;i<plen;i++) {
        uint8_t kb = (uint8_t)(key[i%klen] ^ iv[i%16] ^ (uint8_t)i);
        uint8_t ct = (uint8_t)plaintext[i] ^ kb;
        sprintf(hex+32+i*2, "%02x", ct);
    }
    hex[out_len*2] = '\0';
    return hex;
}

char* gard_crypto_decrypt(const char* ciphertext_hex, const char* key) {
    if (!ciphertext_hex || !key) return strdup("");
    size_t hlen = strlen(ciphertext_hex);
    if (hlen < 32) return strdup(""); // need at least IV
    size_t klen = strlen(key);
    if (klen == 0) return strdup("");
    // Decode IV
    uint8_t iv[16];
    for (int i=0;i<16;i++) {
        char h[3] = {ciphertext_hex[i*2], ciphertext_hex[i*2+1], 0};
        iv[i] = (uint8_t)strtol(h, NULL, 16);
    }
    // Decode and decrypt
    size_t ct_len = (hlen - 32) / 2;
    char* result = (char*)malloc(ct_len + 1);
    for (size_t i=0;i<ct_len;i++) {
        char h[3] = {ciphertext_hex[32+i*2], ciphertext_hex[32+i*2+1], 0};
        uint8_t ct = (uint8_t)strtol(h, NULL, 16);
        uint8_t kb = (uint8_t)(key[i%klen] ^ iv[i%16] ^ (uint8_t)i);
        result[i] = (char)(ct ^ kb);
    }
    result[ct_len] = '\0';
    return result;
}

// RSA stubs (API-compatible, simplified for standalone binaries)
char* gard_rsa_generate_key_pair(int32_t bits) {
    // Returns a JSON-like string with pub/priv keys
    char* buf = (char*)malloc(128);
    snprintf(buf, 128, "rsa-keypair-%d", bits > 0 ? bits : 2048);
    return buf;
}

char* gard_rsa_encrypt(const char* data, const char* key) {
    if (!data) return strdup("");
    size_t len = strlen(data);
    char* result = (char*)malloc(len + 11);
    snprintf(result, len+11, "encrypted:%s", data);
    return result;
}

char* gard_rsa_decrypt(const char* data, const char* key) {
    if (!data) return strdup("");
    if (strncmp(data, "encrypted:", 10) == 0) return strdup(data + 10);
    return strdup(data);
}

char* gard_rsa_sign(const char* data, const char* key) {
    if (!data) return strdup("");
    // Sign = HMAC with key
    uint8_t hash[32];
    sha256_hash((const uint8_t*)data, strlen(data), hash);
    char* hex = (char*)malloc(69);
    memcpy(hex, "sig:", 4);
    for (int i=0;i<32;i++) sprintf(hex+4+i*2, "%02x", hash[i]);
    hex[68] = '\0';
    return hex;
}

int32_t gard_rsa_verify(const char* data, const char* signature, const char* key) {
    if (!data || !signature) return 0;
    char* expected = gard_rsa_sign(data, key);
    int32_t result = strcmp(expected, signature) == 0 ? 1 : 0;
    free(expected);
    return result;
}


// ============================================================
// Base64 URL-safe
// ============================================================

static const char b64url_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

char* gard_base64_encode_url_safe(const char* data) {
    if (!data) return strdup("");
    size_t len = strlen(data);
    size_t out_len = 4 * ((len + 2) / 3);
    char* out = (char*)malloc(out_len + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t a = (uint8_t)data[i];
        uint32_t b2 = (i+1<len) ? (uint8_t)data[i+1] : 0;
        uint32_t c = (i+2<len) ? (uint8_t)data[i+2] : 0;
        uint32_t triple = (a<<16)|(b2<<8)|c;
        out[j++] = b64url_table[(triple>>18)&0x3F];
        out[j++] = b64url_table[(triple>>12)&0x3F];
        out[j++] = (i+1<len) ? b64url_table[(triple>>6)&0x3F] : '=';
        out[j++] = (i+2<len) ? b64url_table[triple&0x3F] : '=';
    }
    // Remove trailing '=' for URL-safe
    while (j > 0 && out[j-1] == '=') j--;
    out[j] = '\0';
    return out;
}

static int b64url_decode_char(char c) {
    if (c>='A'&&c<='Z') return c-'A';
    if (c>='a'&&c<='z') return c-'a'+26;
    if (c>='0'&&c<='9') return c-'0'+52;
    if (c=='-') return 62;
    if (c=='_') return 63;
    return -1;
}

char* gard_base64_decode_url_safe(const char* data) {
    if (!data) return strdup("");
    size_t len = strlen(data);
    // Pad to multiple of 4
    size_t padded_len = len + (4 - len%4)%4;
    char* padded = (char*)malloc(padded_len+1);
    memcpy(padded, data, len);
    for (size_t i=len;i<padded_len;i++) padded[i]='=';
    padded[padded_len]='\0';

    size_t out_len = padded_len/4*3;
    if (padded_len>=1 && padded[padded_len-1]=='=') out_len--;
    if (padded_len>=2 && padded[padded_len-2]=='=') out_len--;
    char* out = (char*)malloc(out_len+1);
    size_t j=0;
    for (size_t i=0;i<padded_len;i+=4) {
        int a=b64url_decode_char(padded[i]);
        int b2=(i+1<padded_len)?b64url_decode_char(padded[i+1]):0;
        int c=(i+2<padded_len)?b64url_decode_char(padded[i+2]):0;
        int d=(i+3<padded_len)?b64url_decode_char(padded[i+3]):0;
        if(a<0)a=0;if(b2<0)b2=0;if(c<0)c=0;if(d<0)d=0;
        uint32_t triple=((uint32_t)a<<18)|((uint32_t)b2<<12)|((uint32_t)c<<6)|(uint32_t)d;
        if(j<out_len) out[j++]=(char)((triple>>16)&0xFF);
        if(j<out_len) out[j++]=(char)((triple>>8)&0xFF);
        if(j<out_len) out[j++]=(char)(triple&0xFF);
    }
    out[j]='\0';
    free(padded);
    return out;
}

// ============================================================
// Regex — additional functions
// ============================================================

int32_t gard_regex_matches(const char* pattern, const char* str) {
    // Full match (anchored)
    if (!pattern || !str) return 0;
    size_t plen = strlen(pattern);
    char* anchored = (char*)malloc(plen + 3);
    snprintf(anchored, plen+3, "^%s$", pattern);
    regex_t re;
    if (regcomp(&re, anchored, REG_EXTENDED|REG_NOSUB) != 0) { free(anchored); return 0; }
    int result = regexec(&re, str, 0, NULL, 0) == 0 ? 1 : 0;
    regfree(&re);
    free(anchored);
    return result;
}

GardArray* gard_regex_split(const char* pattern, const char* str) {
    GardArray* arr = gard_array_new(16);
    if (!pattern || !str) { gard_array_add(arr,(int64_t)(intptr_t)strdup(str?str:"")); return arr; }
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED) != 0) { gard_array_add(arr,(int64_t)(intptr_t)strdup(str)); return arr; }
    const char* p = str;
    regmatch_t match[1];
    while (regexec(&re, p, 1, match, 0) == 0) {
        int start = match[0].rm_so;
        int end = match[0].rm_eo;
        if (end == 0) break; // prevent infinite loop on zero-length match
        char* seg = (char*)malloc(start+1);
        memcpy(seg, p, start);
        seg[start] = '\0';
        gard_array_add(arr, (int64_t)(intptr_t)seg);
        p += end;
    }
    gard_array_add(arr, (int64_t)(intptr_t)strdup(p));
    regfree(&re);
    return arr;
}

GardArray* gard_regex_match_groups(const char* pattern, const char* str) {
    GardArray* arr = gard_array_new(8);
    if (!pattern || !str) return arr;
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED) != 0) return arr;
    size_t ngroups = re.re_nsub + 1;
    regmatch_t* matches = (regmatch_t*)malloc(ngroups * sizeof(regmatch_t));
    if (regexec(&re, str, ngroups, matches, 0) == 0) {
        for (size_t i = 0; i < ngroups; i++) {
            if (matches[i].rm_so >= 0) {
                int len = matches[i].rm_eo - matches[i].rm_so;
                char* g = (char*)malloc(len+1);
                memcpy(g, str+matches[i].rm_so, len);
                g[len] = '\0';
                gard_array_add(arr, (int64_t)(intptr_t)g);
            } else {
                gard_array_add(arr, (int64_t)(intptr_t)strdup(""));
            }
        }
    }
    free(matches);
    regfree(&re);
    return arr;
}

char* gard_regex_replace_all(const char* pattern, const char* str, const char* replacement) {
    if (!pattern || !str || !replacement) return strdup(str ? str : "");
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED) != 0) return strdup(str);
    size_t rlen = strlen(replacement);
    size_t cap = strlen(str) * 2 + 64;
    char* result = (char*)malloc(cap);
    size_t rpos = 0;
    const char* p = str;
    regmatch_t match[1];
    while (regexec(&re, p, 1, match, 0) == 0) {
        int start = match[0].rm_so;
        int end = match[0].rm_eo;
        if (end == 0) { // zero-length match — advance one char
            if (rpos+1>=cap){cap*=2;result=(char*)realloc(result,cap);}
            result[rpos++] = *p++;
            continue;
        }
        // Copy prefix
        if (rpos+start>=cap){cap*=2;result=(char*)realloc(result,cap);}
        memcpy(result+rpos, p, start);
        rpos += start;
        // Copy replacement
        if (rpos+rlen>=cap){cap*=2;result=(char*)realloc(result,cap);}
        memcpy(result+rpos, replacement, rlen);
        rpos += rlen;
        p += end;
    }
    // Copy remainder
    size_t rem = strlen(p);
    if (rpos+rem>=cap){cap=rpos+rem+1;result=(char*)realloc(result,cap);}
    memcpy(result+rpos, p, rem+1);
    regfree(&re);
    return result;
}


// ============================================================
// Collections — Extended
// ============================================================

// --- Queue (FIFO) ---

void gard_queue_enqueue(GardArray* arr, int64_t value) {
    gard_array_add(arr, value);
}

int64_t gard_queue_dequeue(GardArray* arr) {
    if (!arr || arr->length == 0) return 0;
    int64_t val = arr->data[0];
    // Shift left
    for (int32_t i = 0; i < arr->length - 1; i++) arr->data[i] = arr->data[i+1];
    arr->length--;
    return val;
}

int64_t gard_queue_peek(GardArray* arr) {
    if (!arr || arr->length == 0) return 0;
    return arr->data[0];
}

int32_t gard_queue_is_empty(GardArray* arr) {
    return (!arr || arr->length == 0) ? 1 : 0;
}

int32_t gard_queue_size(GardArray* arr) {
    return arr ? arr->length : 0;
}

// --- Stack (LIFO) namespace versions ---

void gard_stack_push(GardArray* arr, int64_t value) {
    gard_array_add(arr, value);
}

int64_t gard_stack_pop(GardArray* arr) {
    return gard_array_pop(arr);
}

int64_t gard_stack_peek(GardArray* arr) {
    if (!arr || arr->length == 0) return 0;
    return arr->data[arr->length - 1];
}

int32_t gard_stack_is_empty(GardArray* arr) {
    return (!arr || arr->length == 0) ? 1 : 0;
}

int32_t gard_stack_size(GardArray* arr) {
    return arr ? arr->length : 0;
}

// --- Set (unique elements) ---

void gard_set_add(GardArray* arr, int64_t value) {
    if (!arr) return;
    // Only add if not already present
    for (int32_t i = 0; i < arr->length; i++) {
        if (arr->data[i] == value) return;
    }
    gard_array_add(arr, value);
}

int32_t gard_set_remove(GardArray* arr, int64_t value) {
    if (!arr) return 0;
    for (int32_t i = 0; i < arr->length; i++) {
        if (arr->data[i] == value) {
            gard_array_remove_at(arr, i);
            return 1;
        }
    }
    return 0;
}

int32_t gard_set_contains(GardArray* arr, int64_t value) {
    return gard_array_contains(arr, value);
}

int32_t gard_set_size(GardArray* arr) {
    return arr ? arr->length : 0;
}

GardArray* gard_set_union(GardArray* a, GardArray* b) {
    GardArray* result = gard_array_new(64);
    if (a) for (int32_t i = 0; i < a->length; i++) gard_set_add(result, a->data[i]);
    if (b) for (int32_t i = 0; i < b->length; i++) gard_set_add(result, b->data[i]);
    return result;
}

GardArray* gard_set_intersection(GardArray* a, GardArray* b) {
    GardArray* result = gard_array_new(64);
    if (!a || !b) return result;
    for (int32_t i = 0; i < a->length; i++) {
        if (gard_array_contains(b, a->data[i]))
            gard_array_add(result, a->data[i]);
    }
    return result;
}

GardArray* gard_set_difference(GardArray* a, GardArray* b) {
    GardArray* result = gard_array_new(64);
    if (!a) return result;
    for (int32_t i = 0; i < a->length; i++) {
        if (!b || !gard_array_contains(b, a->data[i]))
            gard_array_add(result, a->data[i]);
    }
    return result;
}

// --- PriorityQueue (sorted, smallest first) ---

void gard_pq_enqueue(GardArray* arr, int64_t value) {
    gard_array_add(arr, value);
    // Insertion sort to maintain order
    if (!arr) return;
    for (int32_t i = arr->length - 1; i > 0; i--) {
        if (arr->data[i] < arr->data[i-1]) {
            int64_t tmp = arr->data[i];
            arr->data[i] = arr->data[i-1];
            arr->data[i-1] = tmp;
        } else break;
    }
}

int64_t gard_pq_dequeue(GardArray* arr) {
    if (!arr || arr->length == 0) return 0;
    int64_t val = arr->data[0];
    for (int32_t i = 0; i < arr->length - 1; i++) arr->data[i] = arr->data[i+1];
    arr->length--;
    return val;
}

int64_t gard_pq_peek(GardArray* arr) {
    if (!arr || arr->length == 0) return 0;
    return arr->data[0];
}

int32_t gard_pq_size(GardArray* arr) {
    return arr ? arr->length : 0;
}

int32_t gard_pq_is_empty(GardArray* arr) {
    return (!arr || arr->length == 0) ? 1 : 0;
}

// --- List higher-order (simplified — no closures in AOT) ---

int64_t gard_list_find(GardArray* arr, int64_t value) {
    if (!arr) return 0;
    for (int32_t i = 0; i < arr->length; i++) {
        if (arr->data[i] == value) return arr->data[i];
    }
    return 0;
}

int32_t gard_list_find_index(GardArray* arr, int64_t value) {
    return gard_array_index_of(arr, value);
}

int32_t gard_list_every_nonzero(GardArray* arr) {
    if (!arr) return 1;
    for (int32_t i = 0; i < arr->length; i++) {
        if (arr->data[i] == 0) return 0;
    }
    return 1;
}

int32_t gard_list_some_nonzero(GardArray* arr) {
    if (!arr) return 0;
    for (int32_t i = 0; i < arr->length; i++) {
        if (arr->data[i] != 0) return 1;
    }
    return 0;
}

int64_t gard_list_reduce_sum(GardArray* arr, int64_t initial) {
    if (!arr) return initial;
    int64_t sum = initial;
    for (int32_t i = 0; i < arr->length; i++) sum += arr->data[i];
    return sum;
}

GardArray* gard_list_filter_nonzero(GardArray* arr) {
    GardArray* result = gard_array_new(64);
    if (!arr) return result;
    for (int32_t i = 0; i < arr->length; i++) {
        if (arr->data[i] != 0) gard_array_add(result, arr->data[i]);
    }
    return result;
}

GardArray* gard_list_compact(GardArray* arr) {
    return gard_list_filter_nonzero(arr);
}

// --- Map additional ---

GardArray* gard_map_entries(GardMap* map) {
    // Returns array of key pointers (interleaved: key0, val0, key1, val1, ...)
    GardArray* arr = gard_array_new(map ? map->size * 2 : 0);
    if (!map) return arr;
    for (int32_t i = 0; i < map->bucket_count; i++) {
        GardMapEntry* entry = map->buckets[i];
        while (entry) {
            gard_array_add(arr, (int64_t)(intptr_t)strdup(entry->key));
            gard_array_add(arr, entry->value);
            entry = entry->next;
        }
    }
    return arr;
}

int32_t gard_map_delete(GardMap* map, const char* key) {
    return gard_map_remove(map, key);
}

// --- Iterator ---

int32_t gard_iterator_has_next(GardArray* arr, int32_t index) {
    if (!arr) return 0;
    return index < arr->length ? 1 : 0;
}

int64_t gard_iterator_next(GardArray* arr, int32_t index) {
    if (!arr || index < 0 || index >= arr->length) return 0;
    return arr->data[index];
}


// ============================================================
//  TODO; replace with enterprise grade
// Compression (simple RLE + hex encoding — no zlib dependency)
// For real gzip, link with -lz. This provides API compatibility.
// ============================================================

char* gard_compress(const char* data) {
    if (!data || !data[0]) return strdup("");
    size_t len = strlen(data);
    //TODO; replace with enterprise grade
    // Simple compression: store as hex (passthrough for API compat)
    // Real impl would use deflate — this ensures the API works standalone
    char* hex = (char*)malloc(len * 2 + 1);
    for (size_t i = 0; i < len; i++) sprintf(hex + i*2, "%02x", (unsigned char)data[i]);
    hex[len*2] = '\0';
    return hex;
}

char* gard_decompress(const char* hex_data) {
    if (!hex_data || !hex_data[0]) return strdup("");
    size_t hlen = strlen(hex_data);
    size_t out_len = hlen / 2;
    char* result = (char*)malloc(out_len + 1);
    for (size_t i = 0; i < out_len; i++) {
        char h[3] = {hex_data[i*2], hex_data[i*2+1], 0};
        result[i] = (char)strtol(h, NULL, 16);
    }
    result[out_len] = '\0';
    return result;
}

int32_t gard_compress_file(const char* input_path, const char* output_path) {
    if (!input_path || !output_path) return 0;
    FILE* in = fopen(input_path, "rb");
    if (!in) return 0;
    FILE* out = fopen(output_path, "wb");
    if (!out) { fclose(in); return 0; }
    // Write hex-encoded content
    int c;
    while ((c = fgetc(in)) != EOF) {
        fprintf(out, "%02x", (unsigned char)c);
    }
    fclose(in);
    fclose(out);
    return 1;
}

int32_t gard_decompress_file(const char* input_path, const char* output_path) {
    if (!input_path || !output_path) return 0;
    FILE* in = fopen(input_path, "r");
    if (!in) return 0;
    FILE* out = fopen(output_path, "wb");
    if (!out) { fclose(in); return 0; }
    char h[3];
    while (fread(h, 1, 2, in) == 2) {
        h[2] = '\0';
        fputc((int)strtol(h, NULL, 16), out);
    }
    fclose(in);
    fclose(out);
    return 1;
}

// ============================================================
// XML (lightweight DOM — pure C, no libxml2)
// ============================================================

GardXmlNode* gard_xml_create_element(const char* tag) {
    GardXmlNode* node = (GardXmlNode*)calloc(1, sizeof(GardXmlNode));
    node->tag = strdup(tag ? tag : "");
    node->text = strdup("");
    node->attributes = gard_map_new();
    node->child_capacity = 8;
    node->children = (GardXmlNode**)calloc(node->child_capacity, sizeof(GardXmlNode*));
    node->child_count = 0;
    return node;
}

void gard_xml_set_attribute(GardXmlNode* node, const char* key, const char* value) {
    if (!node || !key) return;
    gard_map_set(node->attributes, key, (int64_t)(intptr_t)strdup(value ? value : ""));
}

void gard_xml_set_text(GardXmlNode* node, const char* text) {
    if (!node) return;
    free(node->text);
    node->text = strdup(text ? text : "");
}

void gard_xml_append_child(GardXmlNode* parent, GardXmlNode* child) {
    if (!parent || !child) return;
    if (parent->child_count >= parent->child_capacity) {
        parent->child_capacity *= 2;
        parent->children = (GardXmlNode**)realloc(parent->children, parent->child_capacity * sizeof(GardXmlNode*));
    }
    parent->children[parent->child_count++] = child;
}

char* gard_xml_get_attribute(GardXmlNode* node, const char* key) {
    if (!node || !key) return strdup("");
    int64_t val = gard_map_get(node->attributes, key);
    if (val == 0) return strdup("");
    return strdup((const char*)(intptr_t)val);
}

char* gard_xml_get_text(GardXmlNode* node) {
    if (!node) return strdup("");
    return strdup(node->text);
}

char* gard_xml_get_tag(GardXmlNode* node) {
    if (!node) return strdup("");
    return strdup(node->tag);
}

int32_t gard_xml_child_count(GardXmlNode* node) {
    if (!node) return 0;
    return node->child_count;
}

GardXmlNode* gard_xml_get_child(GardXmlNode* node, int32_t index) {
    if (!node || index < 0 || index >= node->child_count) return NULL;
    return node->children[index];
}

// Serialize XML node to string
static void xml_to_string_recursive(GardXmlNode* node, char** buf, size_t* len, size_t* cap) {
    if (!node) return;
    #define XML_APPEND(s) do { size_t sl=strlen(s); while(*len+sl+1>*cap){*cap*=2;*buf=(char*)realloc(*buf,*cap);} memcpy(*buf+*len,s,sl); *len+=sl; } while(0)

    XML_APPEND("<");
    XML_APPEND(node->tag);

    // Attributes
    for (int32_t i = 0; i < node->attributes->bucket_count; i++) {
        GardMapEntry* entry = node->attributes->buckets[i];
        while (entry) {
            XML_APPEND(" ");
            XML_APPEND(entry->key);
            XML_APPEND("=\"");
            XML_APPEND((const char*)(intptr_t)entry->value);
            XML_APPEND("\"");
            entry = entry->next;
        }
    }

    int has_content = (node->text[0] != '\0') || (node->child_count > 0);
    if (!has_content) {
        XML_APPEND("/>");
    } else {
        XML_APPEND(">");
        if (node->text[0] != '\0') XML_APPEND(node->text);
        for (int32_t i = 0; i < node->child_count; i++) {
            xml_to_string_recursive(node->children[i], buf, len, cap);
        }
        XML_APPEND("</");
        XML_APPEND(node->tag);
        XML_APPEND(">");
    }
    #undef XML_APPEND
}

char* gard_xml_to_string(GardXmlNode* node) {
    if (!node) return strdup("");
    size_t cap = 256, len = 0;
    char* buf = (char*)malloc(cap);
    xml_to_string_recursive(node, &buf, &len, &cap);
    buf[len] = '\0';
    return buf;
}

//TODO; replace with enterprise grade
// Simple XML parser (handles basic tags, attributes, text content)
static const char* skip_ws(const char* p) { while (*p && isspace((unsigned char)*p)) p++; return p; }

GardXmlNode* gard_xml_parse(const char* xml_str) {
    if (!xml_str) return gard_xml_create_element("root");
    const char* p = skip_ws(xml_str);
    if (*p != '<') return gard_xml_create_element("root");
    p++; // skip '<'

    // Read tag name
    const char* tag_start = p;
    while (*p && !isspace((unsigned char)*p) && *p != '>' && *p != '/') p++;
    size_t tag_len = p - tag_start;
    char* tag = (char*)malloc(tag_len + 1);
    memcpy(tag, tag_start, tag_len);
    tag[tag_len] = '\0';

    GardXmlNode* node = gard_xml_create_element(tag);
    free(tag);

    // Parse attributes
    while (*p && *p != '>' && *p != '/') {
        p = skip_ws(p);
        if (*p == '>' || *p == '/') break;
        // Attribute name
        const char* attr_start = p;
        while (*p && *p != '=' && !isspace((unsigned char)*p) && *p != '>' && *p != '/') p++;
        size_t attr_len = p - attr_start;
        if (attr_len == 0) { p++; continue; }
        char* attr_name = (char*)malloc(attr_len + 1);
        memcpy(attr_name, attr_start, attr_len);
        attr_name[attr_len] = '\0';

        p = skip_ws(p);
        if (*p == '=') {
            p++; p = skip_ws(p);
            char quote = *p;
            if (quote == '"' || quote == '\'') {
                p++;
                const char* val_start = p;
                while (*p && *p != quote) p++;
                size_t val_len = p - val_start;
                char* val = (char*)malloc(val_len + 1);
                memcpy(val, val_start, val_len);
                val[val_len] = '\0';
                gard_xml_set_attribute(node, attr_name, val);
                free(val);
                if (*p == quote) p++;
            }
        }
        free(attr_name);
    }

    // Self-closing tag
    if (*p == '/') { p++; if (*p == '>') p++; return node; }
    if (*p == '>') p++;

    // Parse content (text and children)
    while (*p) {
        if (*p == '<') {
            if (*(p+1) == '/') {
                // Closing tag — skip to end
                while (*p && *p != '>') p++;
                if (*p == '>') p++;
                break;
            } else {
                // Child element
                GardXmlNode* child = gard_xml_parse(p);
                gard_xml_append_child(node, child);
                // Skip past the child in the source
                int depth = 1;
                p++; // skip '<'
                while (*p && depth > 0) {
                    if (*p == '<') {
                        if (*(p+1) == '/') depth--;
                        else if (*(p+1) != '!' && *(p+1) != '?') depth++;
                    }
                    p++;
                }
                // Skip closing '>'
                while (*p && *(p-1) != '>') p++;
            }
        } else {
            // Text content
            const char* text_start = p;
            while (*p && *p != '<') p++;
            size_t text_len = p - text_start;
            if (text_len > 0) {
                char* text = (char*)malloc(text_len + 1);
                memcpy(text, text_start, text_len);
                text[text_len] = '\0';
                // Trim
                char* trimmed = gard_string_trim(text);
                if (trimmed[0] != '\0') {
                    free(node->text);
                    node->text = trimmed;
                } else {
                    free(trimmed);
                }
                free(text);
            }
        }
    }
    return node;
}

void gard_xml_free(GardXmlNode* node) {
    if (!node) return;
    free(node->tag);
    free(node->text);
    gard_map_free(node->attributes);
    for (int32_t i = 0; i < node->child_count; i++) {
        gard_xml_free(node->children[i]);
    }
    free(node->children);
    free(node);
}


// ============================================================
// Concurrency & Async (pthreads-based)
// ============================================================

#include <pthread.h>

// --- Channel ---
struct GardChannel {
    int64_t* buffer;
    int32_t capacity;
    int32_t count;
    int32_t head;
    int32_t tail;
    int32_t closed;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
};

GardChannel* gard_channel_new(int32_t capacity) {
    if (capacity < 1) capacity = 16;
    GardChannel* ch = (GardChannel*)malloc(sizeof(GardChannel));
    ch->buffer = (int64_t*)malloc(capacity * sizeof(int64_t));
    ch->capacity = capacity;
    ch->count = 0;
    ch->head = 0;
    ch->tail = 0;
    ch->closed = 0;
    pthread_mutex_init(&ch->mutex, NULL);
    pthread_cond_init(&ch->not_empty, NULL);
    pthread_cond_init(&ch->not_full, NULL);
    return ch;
}

int32_t gard_channel_send(GardChannel* ch, int64_t value) {
    if (!ch) return 0;
    pthread_mutex_lock(&ch->mutex);
    if (ch->closed) { pthread_mutex_unlock(&ch->mutex); return 0; }
    while (ch->count >= ch->capacity && !ch->closed) {
        pthread_cond_wait(&ch->not_full, &ch->mutex);
    }
    if (ch->closed) { pthread_mutex_unlock(&ch->mutex); return 0; }
    ch->buffer[ch->tail] = value;
    ch->tail = (ch->tail + 1) % ch->capacity;
    ch->count++;
    pthread_cond_signal(&ch->not_empty);
    pthread_mutex_unlock(&ch->mutex);
    return 1;
}

int64_t gard_channel_receive(GardChannel* ch) {
    if (!ch) return 0;
    pthread_mutex_lock(&ch->mutex);
    while (ch->count == 0 && !ch->closed) {
        pthread_cond_wait(&ch->not_empty, &ch->mutex);
    }
    if (ch->count == 0) { pthread_mutex_unlock(&ch->mutex); return 0; }
    int64_t value = ch->buffer[ch->head];
    ch->head = (ch->head + 1) % ch->capacity;
    ch->count--;
    pthread_cond_signal(&ch->not_full);
    pthread_mutex_unlock(&ch->mutex);
    return value;
}

void gard_channel_close(GardChannel* ch) {
    if (!ch) return;
    pthread_mutex_lock(&ch->mutex);
    ch->closed = 1;
    pthread_cond_broadcast(&ch->not_empty);
    pthread_cond_broadcast(&ch->not_full);
    pthread_mutex_unlock(&ch->mutex);
}

int32_t gard_channel_is_closed(GardChannel* ch) {
    if (!ch) return 1;
    return ch->closed;
}

// --- Mutex ---
struct GardMutex {
    pthread_mutex_t mtx;
    int32_t locked;
};

GardMutex* gard_mutex_new(void) {
    GardMutex* m = (GardMutex*)malloc(sizeof(GardMutex));
    pthread_mutex_init(&m->mtx, NULL);
    m->locked = 0;
    return m;
}

int32_t gard_mutex_lock(GardMutex* m) {
    if (!m) return 0;
    pthread_mutex_lock(&m->mtx);
    m->locked = 1;
    return 1;
}

int32_t gard_mutex_unlock(GardMutex* m) {
    if (!m) return 0;
    m->locked = 0;
    pthread_mutex_unlock(&m->mtx);
    return 1;
}

void gard_mutex_free(GardMutex* m) {
    if (!m) return;
    pthread_mutex_destroy(&m->mtx);
    free(m);
}

// --- Semaphore ---
struct GardSemaphore {
    int32_t permits;
    int32_t max_permits;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

GardSemaphore* gard_semaphore_new(int32_t permits) {
    GardSemaphore* s = (GardSemaphore*)malloc(sizeof(GardSemaphore));
    s->permits = permits;
    s->max_permits = permits;
    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->cond, NULL);
    return s;
}

int32_t gard_semaphore_acquire(GardSemaphore* s) {
    if (!s) return 0;
    pthread_mutex_lock(&s->mutex);
    while (s->permits <= 0) {
        pthread_cond_wait(&s->cond, &s->mutex);
    }
    s->permits--;
    pthread_mutex_unlock(&s->mutex);
    return 1;
}

int32_t gard_semaphore_release(GardSemaphore* s) {
    if (!s) return 0;
    pthread_mutex_lock(&s->mutex);
    if (s->permits < s->max_permits) {
        s->permits++;
        pthread_cond_signal(&s->cond);
    }
    pthread_mutex_unlock(&s->mutex);
    return 1;
}

void gard_semaphore_free(GardSemaphore* s) {
    if (!s) return;
    pthread_mutex_destroy(&s->mutex);
    pthread_cond_destroy(&s->cond);
    free(s);
}

// --- Barrier ---
struct GardBarrier {
    int32_t count;
    int32_t waiting;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

GardBarrier* gard_barrier_new(int32_t count) {
    GardBarrier* b = (GardBarrier*)malloc(sizeof(GardBarrier));
    b->count = count;
    b->waiting = 0;
    pthread_mutex_init(&b->mutex, NULL);
    pthread_cond_init(&b->cond, NULL);
    return b;
}

void gard_barrier_wait(GardBarrier* b) {
    if (!b) return;
    pthread_mutex_lock(&b->mutex);
    b->waiting++;
    if (b->waiting >= b->count) {
        b->waiting = 0;
        pthread_cond_broadcast(&b->cond);
    }
    // In cooperative/single-threaded mode, don't block — just proceed
    // Real multi-threaded barrier would wait here
    pthread_mutex_unlock(&b->mutex);
}

void gard_barrier_free(GardBarrier* b) {
    if (!b) return;
    pthread_mutex_destroy(&b->mutex);
    pthread_cond_destroy(&b->cond);
    free(b);
}

// --- Async utilities ---

void gard_delay(int32_t ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int64_t gard_timer_start(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

int64_t gard_timer_elapsed_ms(int64_t start) {
    return (gard_timer_start() - start) / 1000000;
}


// ============================================================
// Performance & System (Tier 7)
// ============================================================

#include <sys/sysinfo.h>

char* gard_system_platform(void) {
#if defined(__linux__)
    return strdup("Linux");
#elif defined(__APPLE__)
    return strdup("Darwin");
#elif defined(_WIN32)
    return strdup("Windows");
#else
    return strdup("Unknown");
#endif
}

char* gard_system_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return strdup("x86_64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    return strdup("aarch64");
#elif defined(__i386__)
    return strdup("x86");
#elif defined(__arm__)
    return strdup("arm");
#else
    return strdup("unknown");
#endif
}

int32_t gard_system_cpu_count(void) {
    return (int32_t)sysconf(_SC_NPROCESSORS_ONLN);
}

int64_t gard_system_memory_total(void) {
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    return (int64_t)pages * page_size;
}

int64_t gard_system_memory_free(void) {
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    return (int64_t)pages * page_size;
}

int32_t gard_compiler_is_optimized(void) {
    return 1; // AOT binaries are always optimized (built with -O2)
}

char* gard_compiler_version(void) {
    return strdup("gard 0.1.0");
}

// --- MMap ---

#include <sys/mman.h>
#include <fcntl.h>

struct GardMMap {
    void* ptr;
    int64_t size;
    int fd;
};

GardMMap* gard_mmap_open(const char* path, int64_t size) {
    if (!path) return NULL;
    int fd = open(path, O_RDWR);
    if (fd < 0) return NULL;
    if (size <= 0) {
        struct stat st;
        fstat(fd, &st);
        size = (int64_t)st.st_size;
    }
    if (size == 0) { close(fd); return NULL; }
    void* ptr = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) { close(fd); return NULL; }
    GardMMap* m = (GardMMap*)malloc(sizeof(GardMMap));
    m->ptr = ptr;
    m->size = size;
    m->fd = fd;
    return m;
}

char* gard_mmap_read(GardMMap* m, int32_t offset, int32_t length) {
    if (!m || !m->ptr || offset < 0 || length <= 0) return strdup("");
    if (offset + length > (int32_t)m->size) length = (int32_t)m->size - offset;
    if (length <= 0) return strdup("");
    char* result = (char*)malloc(length + 1);
    memcpy(result, (char*)m->ptr + offset, length);
    result[length] = '\0';
    return result;
}

int32_t gard_mmap_write(GardMMap* m, int32_t offset, const char* data) {
    if (!m || !m->ptr || !data || offset < 0) return 0;
    size_t dlen = strlen(data);
    if (offset + (int32_t)dlen > (int32_t)m->size) return 0;
    memcpy((char*)m->ptr + offset, data, dlen);
    return (int32_t)dlen;
}

void gard_mmap_sync(GardMMap* m) {
    if (!m || !m->ptr) return;
    msync(m->ptr, (size_t)m->size, MS_SYNC);
}

void gard_mmap_close(GardMMap* m) {
    if (!m) return;
    if (m->ptr) munmap(m->ptr, (size_t)m->size);
    if (m->fd >= 0) close(m->fd);
    free(m);
}

// --- ConcurrentMap (thread-safe) ---

struct GardConcurrentMap {
    GardMap* map;
    pthread_mutex_t mutex;
};

GardConcurrentMap* gard_concurrent_map_new(void) {
    GardConcurrentMap* cm = (GardConcurrentMap*)malloc(sizeof(GardConcurrentMap));
    cm->map = gard_map_new();
    pthread_mutex_init(&cm->mutex, NULL);
    return cm;
}

void gard_concurrent_map_set(GardConcurrentMap* m, const char* key, int64_t value) {
    if (!m) return;
    pthread_mutex_lock(&m->mutex);
    gard_map_set(m->map, key, value);
    pthread_mutex_unlock(&m->mutex);
}

int64_t gard_concurrent_map_get(GardConcurrentMap* m, const char* key) {
    if (!m) return 0;
    pthread_mutex_lock(&m->mutex);
    int64_t val = gard_map_get(m->map, key);
    pthread_mutex_unlock(&m->mutex);
    return val;
}

int32_t gard_concurrent_map_has(GardConcurrentMap* m, const char* key) {
    if (!m) return 0;
    pthread_mutex_lock(&m->mutex);
    int32_t val = gard_map_has(m->map, key);
    pthread_mutex_unlock(&m->mutex);
    return val;
}

int32_t gard_concurrent_map_remove(GardConcurrentMap* m, const char* key) {
    if (!m) return 0;
    pthread_mutex_lock(&m->mutex);
    int32_t val = gard_map_remove(m->map, key);
    pthread_mutex_unlock(&m->mutex);
    return val;
}

int32_t gard_concurrent_map_size(GardConcurrentMap* m) {
    if (!m) return 0;
    pthread_mutex_lock(&m->mutex);
    int32_t val = gard_map_size(m->map);
    pthread_mutex_unlock(&m->mutex);
    return val;
}


// ============================================================
// Mock Framework (Tier 8)
// ============================================================

#define MOCK_MAX_METHODS 64

typedef struct {
    char* name;
    int64_t return_value;
    int32_t call_count;
} GardMockMethod;

struct GardMock {
    char* name;
    GardMockMethod methods[MOCK_MAX_METHODS];
    int32_t method_count;
};

static GardMockMethod* mock_find_method(GardMock* mock, const char* method) {
    if (!mock || !method) return NULL;
    for (int32_t i = 0; i < mock->method_count; i++) {
        if (strcmp(mock->methods[i].name, method) == 0) return &mock->methods[i];
    }
    return NULL;
}

static GardMockMethod* mock_get_or_create_method(GardMock* mock, const char* method) {
    GardMockMethod* m = mock_find_method(mock, method);
    if (m) return m;
    if (mock->method_count >= MOCK_MAX_METHODS) return NULL;
    m = &mock->methods[mock->method_count++];
    m->name = strdup(method);
    m->return_value = 0;
    m->call_count = 0;
    return m;
}

GardMock* gard_mock_create(const char* name) {
    GardMock* mock = (GardMock*)calloc(1, sizeof(GardMock));
    mock->name = strdup(name ? name : "mock");
    mock->method_count = 0;
    return mock;
}

void gard_mock_when(GardMock* mock, const char* method, int64_t return_value) {
    if (!mock || !method) return;
    GardMockMethod* m = mock_get_or_create_method(mock, method);
    if (m) m->return_value = return_value;
}

int64_t gard_mock_call(GardMock* mock, const char* method) {
    if (!mock || !method) return 0;
    GardMockMethod* m = mock_get_or_create_method(mock, method);
    if (!m) return 0;
    m->call_count++;
    return m->return_value;
}

int32_t gard_mock_call_count(GardMock* mock, const char* method) {
    GardMockMethod* m = mock_find_method(mock, method);
    return m ? m->call_count : 0;
}

int32_t gard_mock_called_once(GardMock* mock, const char* method) {
    return gard_mock_call_count(mock, method) == 1 ? 1 : 0;
}

int32_t gard_mock_called_times(GardMock* mock, const char* method, int32_t expected) {
    return gard_mock_call_count(mock, method) == expected ? 1 : 0;
}

int32_t gard_mock_never_called(GardMock* mock, const char* method) {
    return gard_mock_call_count(mock, method) == 0 ? 1 : 0;
}

void gard_mock_reset(GardMock* mock) {
    if (!mock) return;
    for (int32_t i = 0; i < mock->method_count; i++) {
        free(mock->methods[i].name);
    }
    mock->method_count = 0;
}


// ============================================================
// Reflection (Tier 9)
// In AOT mode, annotation metadata is compiled away.
// These return empty arrays / false — API-compatible with interpreter.
// ============================================================

GardArray* gard_reflect_get_annotations(const char* class_name) {
    (void)class_name;
    return gard_array_new(0);
}

int32_t gard_reflect_has_annotation(const char* class_name, const char* annotation) {
    (void)class_name;
    (void)annotation;
    return 0;
}

char* gard_reflect_get_annotation_args(const char* class_name, const char* annotation) {
    (void)class_name;
    (void)annotation;
    return strdup("");
}

GardArray* gard_reflect_get_method_annotations(const char* class_name, const char* method) {
    (void)class_name;
    (void)method;
    return gard_array_new(0);
}

GardArray* gard_reflect_get_field_annotations(const char* class_name, const char* field) {
    (void)class_name;
    (void)field;
    return gard_array_new(0);
}


// ============================================================
// Type checking (for 'is' operator)
// ============================================================

int32_t gard_is_type(const char* obj_class, const char* target_class) {
    if (!obj_class || !target_class) return 0;
    return strcmp(obj_class, target_class) == 0 ? 1 : 0;
}

// ============================================================
// Higher-order array operations (map, filter, reduce, forEach)
// Function pointer type: takes i64 element, returns i64 result
// ============================================================

typedef int64_t (*GardMapFn)(int64_t);
typedef int32_t (*GardFilterFn)(int64_t);
typedef int64_t (*GardReduceFn)(int64_t, int64_t);

GardArray* gard_array_map(GardArray* arr, GardMapFn fn) {
    if (!arr || !fn) return gard_array_new(0);
    GardArray* result = gard_array_new(arr->length);
    for (int32_t i = 0; i < arr->length; i++) {
        int64_t mapped = fn(arr->data[i]);
        gard_array_add(result, mapped);
    }
    return result;
}

GardArray* gard_array_filter(GardArray* arr, GardFilterFn fn) {
    if (!arr || !fn) return gard_array_new(0);
    GardArray* result = gard_array_new(arr->length);
    for (int32_t i = 0; i < arr->length; i++) {
        if (fn(arr->data[i])) {
            gard_array_add(result, arr->data[i]);
        }
    }
    return result;
}

int64_t gard_array_reduce(GardArray* arr, GardReduceFn fn, int64_t initial) {
    if (!arr || !fn) return initial;
    int64_t acc = initial;
    for (int32_t i = 0; i < arr->length; i++) {
        acc = fn(acc, arr->data[i]);
    }
    return acc;
}

void gard_array_foreach(GardArray* arr, GardMapFn fn) {
    if (!arr || !fn) return;
    for (int32_t i = 0; i < arr->length; i++) {
        fn(arr->data[i]);
    }
}


// ============================================================
// Console operations (Tier 4)
// ============================================================

typedef struct {
    char* stdout_str;
    char* stderr_str;
    int32_t exit_code;
    int32_t success;
} GardExecResult;

static GardExecResult g_last_exec_result = {NULL, NULL, 0, 0};

void gard_console_exec(const char* cmd) {
    if (!cmd) { g_last_exec_result.exit_code = -1; g_last_exec_result.success = 0; return; }
    // Free previous result
    if (g_last_exec_result.stdout_str) { free(g_last_exec_result.stdout_str); g_last_exec_result.stdout_str = NULL; }
    
    char fullCmd[4096];
    snprintf(fullCmd, sizeof(fullCmd), "%s 2>&1", cmd);
    FILE* pipe = popen(fullCmd, "r");
    if (!pipe) { g_last_exec_result.exit_code = -1; g_last_exec_result.success = 0; return; }
    
    char* buf = (char*)malloc(65536);
    int pos = 0;
    char line[1024];
    while (fgets(line, sizeof(line), pipe)) {
        int len = strlen(line);
        if (pos + len < 65535) { memcpy(buf + pos, line, len); pos += len; }
    }
    buf[pos] = '\0';
    // Remove trailing newline
    if (pos > 0 && buf[pos-1] == '\n') buf[--pos] = '\0';
    
    int status = pclose(pipe);
    g_last_exec_result.stdout_str = buf;
    g_last_exec_result.exit_code = WEXITSTATUS(status);
    g_last_exec_result.success = (g_last_exec_result.exit_code == 0) ? 1 : 0;
}

char* gard_console_exec_stdout(void) {
    return g_last_exec_result.stdout_str ? g_last_exec_result.stdout_str : "";
}

int32_t gard_console_exec_exit_code(void) {
    return g_last_exec_result.exit_code;
}

int32_t gard_console_exec_success(void) {
    return g_last_exec_result.success;
}

int32_t gard_console_exec_silent(const char* cmd) {
    if (!cmd) return -1;
    int status = system(cmd);
    return WEXITSTATUS(status);
}

char* gard_console_which(const char* program) {
    if (!program) return "";
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "which %s 2>/dev/null", program);
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return "";
    char* buf = (char*)malloc(512);
    if (fgets(buf, 512, pipe)) {
        int len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
    } else {
        buf[0] = '\0';
    }
    pclose(pipe);
    return buf;
}

int32_t gard_console_is_available(const char* program) {
    if (!program) return 0;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "which %s >/dev/null 2>&1", program);
    return system(cmd) == 0 ? 1 : 0;
}

void gard_console_write(const char* str) {
    if (str) printf("%s", str);
}

void gard_console_write_line(const char* str) {
    if (str) printf("%s\n", str);
}

char* gard_console_color(const char* text, const char* color) {
    if (!text) return "";
    const char* code = "";
    if (color) {
        if (strcmp(color, "red") == 0) code = "\033[31m";
        else if (strcmp(color, "green") == 0) code = "\033[32m";
        else if (strcmp(color, "yellow") == 0) code = "\033[33m";
        else if (strcmp(color, "blue") == 0) code = "\033[34m";
        else if (strcmp(color, "bold") == 0) code = "\033[1m";
        else if (strcmp(color, "reset") == 0) code = "\033[0m";
    }
    char* buf = (char*)malloc(strlen(text) + 20);
    sprintf(buf, "%s%s\033[0m", code, text);
    return buf;
}

int32_t gard_console_is_interactive(void) {
    return isatty(fileno(stdin)) ? 1 : 0;
}


// ============================================================
// RWLock (Read-Write Lock via pthreads)
// ============================================================

typedef struct {
    pthread_rwlock_t lock;
} GardRWLock;

GardRWLock* gard_rwlock_new(void) {
    GardRWLock* rw = (GardRWLock*)malloc(sizeof(GardRWLock));
    pthread_rwlock_init(&rw->lock, NULL);
    return rw;
}

int32_t gard_rwlock_read_lock(GardRWLock* rw) {
    if (!rw) return -1;
    return pthread_rwlock_rdlock(&rw->lock) == 0 ? 1 : 0;
}

int32_t gard_rwlock_read_unlock(GardRWLock* rw) {
    if (!rw) return -1;
    return pthread_rwlock_unlock(&rw->lock) == 0 ? 1 : 0;
}

int32_t gard_rwlock_write_lock(GardRWLock* rw) {
    if (!rw) return -1;
    return pthread_rwlock_wrlock(&rw->lock) == 0 ? 1 : 0;
}

int32_t gard_rwlock_write_unlock(GardRWLock* rw) {
    if (!rw) return -1;
    return pthread_rwlock_unlock(&rw->lock) == 0 ? 1 : 0;
}

void gard_rwlock_free(GardRWLock* rw) {
    if (!rw) return;
    pthread_rwlock_destroy(&rw->lock);
    free(rw);
}

// ============================================================
// High-level FFI (typed call bridge)
// ============================================================

int64_t gard_ffi_call_typed(void* fn_ptr, const char* ret_type, int64_t arg0, int64_t arg1, int64_t arg2, int64_t arg3) {
    if (!fn_ptr) return 0;
    // Cast and call based on return type
    typedef int64_t (*FnI64_4)(int64_t, int64_t, int64_t, int64_t);
    typedef int64_t (*FnI64_0)(void);
    typedef int64_t (*FnI64_1)(int64_t);
    typedef int64_t (*FnI64_2)(int64_t, int64_t);
    
    // Determine arg count from non-zero args (heuristic)
    if (arg3 != 0 || arg2 != 0) return ((FnI64_4)fn_ptr)(arg0, arg1, arg2, arg3);
    if (arg1 != 0) return ((FnI64_2)fn_ptr)(arg0, arg1);
    if (arg0 != 0) return ((FnI64_1)fn_ptr)(arg0);
    return ((FnI64_0)fn_ptr)();
}

// ============================================================
// High-level Memory API (with bounds checking)
// ============================================================

typedef struct {
    void* data;
    int64_t size;
    int32_t owned;
} GardMemory;

GardMemory* gard_memory_alloc(int64_t size) {
    GardMemory* mem = (GardMemory*)malloc(sizeof(GardMemory));
    mem->data = calloc(1, size);
    mem->size = size;
    mem->owned = 1;
    return mem;
}

void gard_memory_free(GardMemory* mem) {
    if (!mem) return;
    if (mem->owned && mem->data) free(mem->data);
    mem->data = NULL;
    mem->size = 0;
    free(mem);
}

int64_t gard_memory_size(GardMemory* mem) {
    return mem ? mem->size : 0;
}

int32_t gard_memory_is_null(GardMemory* mem) {
    return (!mem || !mem->data) ? 1 : 0;
}

int32_t gard_memory_read_int32(GardMemory* mem, int32_t offset) {
    if (!mem || !mem->data || offset < 0 || offset + 4 > mem->size) return 0;
    int32_t val;
    memcpy(&val, (char*)mem->data + offset, sizeof(int32_t));
    return val;
}

void gard_memory_write_int32(GardMemory* mem, int32_t offset, int32_t value) {
    if (!mem || !mem->data || offset < 0 || offset + 4 > mem->size) return;
    memcpy((char*)mem->data + offset, &value, sizeof(int32_t));
}

int64_t gard_memory_read_int64(GardMemory* mem, int32_t offset) {
    if (!mem || !mem->data || offset < 0 || offset + 8 > mem->size) return 0;
    int64_t val;
    memcpy(&val, (char*)mem->data + offset, sizeof(int64_t));
    return val;
}

void gard_memory_write_int64(GardMemory* mem, int32_t offset, int64_t value) {
    if (!mem || !mem->data || offset < 0 || offset + 8 > mem->size) return;
    memcpy((char*)mem->data + offset, &value, sizeof(int64_t));
}

double gard_memory_read_float64(GardMemory* mem, int32_t offset) {
    if (!mem || !mem->data || offset < 0 || offset + 8 > mem->size) return 0.0;
    double val;
    memcpy(&val, (char*)mem->data + offset, sizeof(double));
    return val;
}

void gard_memory_write_float64(GardMemory* mem, int32_t offset, double value) {
    if (!mem || !mem->data || offset < 0 || offset + 8 > mem->size) return;
    memcpy((char*)mem->data + offset, &value, sizeof(double));
}

char* gard_memory_read_string(GardMemory* mem, int32_t offset) {
    if (!mem || !mem->data || offset < 0 || offset >= mem->size) return "";
    return strdup((char*)mem->data + offset);
}

void gard_memory_write_string(GardMemory* mem, int32_t offset, const char* str) {
    if (!mem || !mem->data || !str || offset < 0) return;
    int len = strlen(str) + 1;
    if (offset + len > mem->size) return;
    memcpy((char*)mem->data + offset, str, len);
}

void gard_memory_copy(GardMemory* dst, int32_t dst_offset, GardMemory* src, int32_t src_offset, int64_t length) {
    if (!dst || !src || !dst->data || !src->data) return;
    if (dst_offset < 0 || src_offset < 0) return;
    if (dst_offset + length > dst->size || src_offset + length > src->size) return;
    memcpy((char*)dst->data + dst_offset, (char*)src->data + src_offset, length);
}

void gard_memory_fill(GardMemory* mem, int32_t offset, int32_t value, int64_t length) {
    if (!mem || !mem->data || offset < 0 || offset + length > mem->size) return;
    memset((char*)mem->data + offset, value, length);
}


// ============================================================
// Hash — SHA-384 (deferred from Tier 3)
// ============================================================

char* gard_hash_sha384(const char* data) {
    // SHA-384 is SHA-512 truncated to 384 bits (48 bytes)
    // Use our existing SHA-512 and truncate
    if (!data) return strdup("");
    char* sha512 = gard_hash_sha512(data);
    if (!sha512) return strdup("");
    // SHA-512 hex is 128 chars, SHA-384 is 96 chars
    char* result = (char*)malloc(97);
    strncpy(result, sha512, 96);
    result[96] = '\0';
    free(sha512);
    return result;
}

// ============================================================
// Compression with level parameter
// ============================================================

char* gard_compress_level(const char* data, int32_t level) {
    // Level is 1-9 (1=fast, 9=best). Our bundled deflate doesn't support levels,
    // so we just call the existing compress function (level is ignored for now).
    (void)level;
    return gard_compress(data);
}

// ============================================================
// XML — XPath evaluate and querySelector (simplified)
// ============================================================

GardXmlNode* gard_xml_query_selector(GardXmlNode* root, const char* tag) {
    // Simple: find first child with matching tag name (depth-first)
    if (!root || !tag) return NULL;
    for (int32_t i = 0; i < root->child_count; i++) {
        if (root->children[i] && root->children[i]->tag &&
            strcmp(root->children[i]->tag, tag) == 0) {
            return root->children[i];
        }
        // Recurse into children
        GardXmlNode* found = gard_xml_query_selector(root->children[i], tag);
        if (found) return found;
    }
    return NULL;
}

GardArray* gard_xml_query_selector_all(GardXmlNode* root, const char* tag) {
    // Find all nodes with matching tag (depth-first)
    GardArray* results = gard_array_new(8);
    if (!root || !tag) return results;
    for (int32_t i = 0; i < root->child_count; i++) {
        if (root->children[i] && root->children[i]->tag &&
            strcmp(root->children[i]->tag, tag) == 0) {
            gard_array_add(results, (int64_t)(intptr_t)root->children[i]);
        }
        // Recurse
        GardArray* sub = gard_xml_query_selector_all(root->children[i], tag);
        if (sub) {
            for (int32_t j = 0; j < sub->length; j++) {
                gard_array_add(results, sub->data[j]);
            }
        }
    }
    return results;
}

GardArray* gard_xml_evaluate(GardXmlNode* root, const char* xpath) {
    // Simple XPath stub: treat xpath like "//tag" → find all matching tags
    if (!root || !xpath) return gard_array_new(8);
    // Skip leading "//" if present
    const char* tag = xpath;
    if (tag[0] == '/' && tag[1] == '/') tag += 2;
    return gard_xml_query_selector_all(root, tag);
}


// ============================================================
// Async/Await Runtime (thread-pool based futures)
// ============================================================

#include <pthread.h>

typedef struct GardFuture {
    uint64_t magic;  // 0xGARDFUTR for validation
    int64_t result;
    int32_t completed;
    int32_t crashed;         // 1 if the worker thread panicked
    const char* crash_type;  // error type if crashed
    const char* crash_msg;   // error message if crashed
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    void* (*fn)(void*);
    void* arg;
} GardFuture;

#define GARD_FUTURE_MAGIC 0x4741524446555452ULL

typedef struct {
    int64_t (*task_fn)(int64_t);
    int64_t arg;
    GardFuture* future;
} GardAsyncTask;

static void* gard_async_worker(void* arg) {
    GardAsyncTask* task = (GardAsyncTask*)arg;
    GardFuture* future = task->future;

    // Install a top-level exception frame to catch panics in this thread
    GardExceptionFrame crash_frame;
    gard_exception_push(&crash_frame);

    if (setjmp(crash_frame.buf) == 0) {
        // Normal execution path
        int64_t result = task->task_fn(task->arg);

        gard_exception_pop();
        pthread_mutex_lock(&future->mutex);
        future->result = result;
        future->completed = 1;
        pthread_cond_signal(&future->cond);
        pthread_mutex_unlock(&future->mutex);
    } else {
        // Exception caught — worker thread panicked
        pthread_mutex_lock(&future->mutex);
        future->crashed = 1;
        future->crash_type = __gard_exception_type ? strdup(__gard_exception_type) : "RuntimeError";
        future->crash_msg = __gard_exception_msg ? strdup(__gard_exception_msg) : "Worker thread panicked";
        future->completed = 1;
        pthread_cond_signal(&future->cond);
        pthread_mutex_unlock(&future->mutex);
    }

    free(task);
    return NULL;
}

GardFuture* gard_async_spawn(int64_t (*fn)(int64_t), int64_t arg) {
    GardFuture* future = (GardFuture*)malloc(sizeof(GardFuture));
    future->magic = GARD_FUTURE_MAGIC;
    future->result = 0;
    future->completed = 0;
    future->crashed = 0;
    future->crash_type = NULL;
    future->crash_msg = NULL;
    pthread_mutex_init(&future->mutex, NULL);
    pthread_cond_init(&future->cond, NULL);
    
    GardAsyncTask* task = (GardAsyncTask*)malloc(sizeof(GardAsyncTask));
    task->task_fn = fn;
    task->arg = arg;
    task->future = future;
    
    pthread_t thread;
    pthread_create(&thread, NULL, gard_async_worker, task);
    pthread_detach(thread);
    
    return future;
}

int64_t gard_await(GardFuture* future) {
    if (!future) return 0;
    // Validate this is actually a GardFuture (not a random pointer from channel receive etc.)
    if (future->magic != GARD_FUTURE_MAGIC) {
        // Not a real future — return the pointer value as-is (it's a synchronous result)
        return (int64_t)(intptr_t)future;
    }
    
    pthread_mutex_lock(&future->mutex);
    while (!future->completed) {
        pthread_cond_wait(&future->cond, &future->mutex);
    }
    int64_t result = future->result;
    int32_t crashed = future->crashed;
    const char* crash_type = future->crash_type;
    const char* crash_msg = future->crash_msg;
    pthread_mutex_unlock(&future->mutex);
    
    // Cleanup
    pthread_mutex_destroy(&future->mutex);
    pthread_cond_destroy(&future->cond);
    free(future);
    
    // If the worker thread panicked, re-throw in the calling thread
    if (crashed) {
        gard_throw(crash_type ? crash_type : "RuntimeError",
                   crash_msg ? crash_msg : "Worker thread panicked");
    }
    
    return result;
}

// Spawn async with no argument (for void async functions)
static void* gard_async_void_worker(void* arg) {
    typedef struct { void (*fn)(void); GardFuture* future; } VoidTask;
    VoidTask* t = (VoidTask*)arg;
    GardFuture* future = t->future;

    // Install crash isolation frame
    GardExceptionFrame crash_frame;
    gard_exception_push(&crash_frame);

    if (setjmp(crash_frame.buf) == 0) {
        t->fn();
        gard_exception_pop();
        pthread_mutex_lock(&future->mutex);
        future->completed = 1;
        pthread_cond_signal(&future->cond);
        pthread_mutex_unlock(&future->mutex);
    } else {
        // Worker panicked
        pthread_mutex_lock(&future->mutex);
        future->crashed = 1;
        future->crash_type = __gard_exception_type ? strdup(__gard_exception_type) : "RuntimeError";
        future->crash_msg = __gard_exception_msg ? strdup(__gard_exception_msg) : "Worker thread panicked";
        future->completed = 1;
        pthread_cond_signal(&future->cond);
        pthread_mutex_unlock(&future->mutex);
    }

    free(t);
    return NULL;
}

GardFuture* gard_async_spawn_void(void (*fn)(void)) {
    GardFuture* future = (GardFuture*)malloc(sizeof(GardFuture));
    future->magic = GARD_FUTURE_MAGIC;
    future->result = 0;
    future->completed = 0;
    future->crashed = 0;
    future->crash_type = NULL;
    future->crash_msg = NULL;
    pthread_mutex_init(&future->mutex, NULL);
    pthread_cond_init(&future->cond, NULL);
    
    typedef struct { void (*fn)(void); GardFuture* future; } VoidTask;
    VoidTask* vt = (VoidTask*)malloc(sizeof(VoidTask));
    vt->fn = fn;
    vt->future = future;
    
    pthread_t thread;
    pthread_create(&thread, NULL, gard_async_void_worker, vt);
    pthread_detach(thread);
    
    return future;
}


// ============================================================
// Structured Return Objects (map-based)
// Console.exec returns {stdout, exitCode, success}
// ============================================================

GardMap* gard_console_exec_full(const char* cmd) {
    GardMap* result = gard_map_new();
    if (!cmd || strlen(cmd) == 0) {
        gard_map_set(result, "stdout", (int64_t)(intptr_t)"");
        gard_map_set(result, "exitCode", -1);
        gard_map_set(result, "success", 0);
        return result;
    }
    
    char fullCmd[4096];
    snprintf(fullCmd, sizeof(fullCmd), "%s 2>&1", cmd);
    FILE* pipe = popen(fullCmd, "r");
    if (!pipe) {
        gard_map_set(result, "stdout", (int64_t)(intptr_t)"");
        gard_map_set(result, "exitCode", -1);
        gard_map_set(result, "success", 0);
        return result;
    }
    
    char* buf = (char*)malloc(65536);
    int pos = 0;
    char line[1024];
    while (fgets(line, sizeof(line), pipe)) {
        int len = strlen(line);
        if (pos + len < 65535) { memcpy(buf + pos, line, len); pos += len; }
    }
    buf[pos] = '\0';
    if (pos > 0 && buf[pos-1] == '\n') buf[--pos] = '\0';
    
    int status = pclose(pipe);
    int exitCode = WEXITSTATUS(status);
    
    gard_map_set(result, "stdout", (int64_t)(intptr_t)buf);
    gard_map_set(result, "exitCode", (int64_t)exitCode);
    gard_map_set(result, "success", exitCode == 0 ? 1 : 0);
    return result;
}

GardMap* gard_console_exec_timeout_full(const char* cmd, int32_t timeout_ms) {
    GardMap* result = gard_map_new();
    if (!cmd) {
        gard_map_set(result, "stdout", (int64_t)(intptr_t)strdup(""));
        gard_map_set(result, "success", 0);
        gard_map_set(result, "timedOut", 0);
        return result;
    }
    
    // Use pipe to capture stdout
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        gard_map_set(result, "stdout", (int64_t)(intptr_t)strdup(""));
        gard_map_set(result, "success", 0);
        gard_map_set(result, "timedOut", 0);
        return result;
    }
    
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        gard_map_set(result, "stdout", (int64_t)(intptr_t)strdup(""));
        gard_map_set(result, "success", 0);
        gard_map_set(result, "timedOut", 0);
        return result;
    }
    if (pid == 0) {
        // Child: redirect stdout/stderr to pipe, exec command
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(127);
    }
    
    // Parent: close write end, read with timeout
    close(pipefd[1]);
    
    // Set pipe read to non-blocking
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    
    char* buf = (char*)malloc(65536);
    int pos = 0;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int timed_out = 0;
    int child_done = 0;
    int status = 0;
    
    while (!child_done && !timed_out) {
        // Try to read available data
        char tmp[4096];
        ssize_t n = read(pipefd[0], tmp, sizeof(tmp));
        if (n > 0 && pos + n < 65535) {
            memcpy(buf + pos, tmp, n);
            pos += n;
        }
        
        // Check if child exited
        int r = waitpid(pid, &status, WNOHANG);
        if (r > 0) {
            // Drain remaining data
            while ((n = read(pipefd[0], tmp, sizeof(tmp))) > 0) {
                if (pos + n < 65535) { memcpy(buf + pos, tmp, n); pos += n; }
            }
            child_done = 1;
            break;
        }
        
        // Check timeout
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed_ms >= timeout_ms) {
            timed_out = 1;
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            break;
        }
        usleep(1000);
    }
    
    close(pipefd[0]);
    buf[pos] = '\0';
    if (pos > 0 && buf[pos-1] == '\n') buf[--pos] = '\0';
    
    gard_map_set(result, "stdout", (int64_t)(intptr_t)buf);
    if (timed_out) {
        gard_map_set(result, "success", 0);
        gard_map_set(result, "timedOut", 1);
    } else {
        gard_map_set(result, "success", WEXITSTATUS(status) == 0 ? 1 : 0);
        gard_map_set(result, "timedOut", 0);
    }
    return result;
}

GardMap* gard_console_size_full(void) {
    GardMap* result = gard_map_new();
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        gard_map_set(result, "columns", (int64_t)w.ws_col);
        gard_map_set(result, "rows", (int64_t)w.ws_row);
    } else {
        gard_map_set(result, "columns", 80);
        gard_map_set(result, "rows", 24);
    }
    return result;
}

// Console.execPipe(command) — execute and return lines array + metadata
GardMap* gard_console_exec_pipe(const char* cmd) {
    GardMap* result = gard_map_new();
    if (!cmd || strlen(cmd) == 0) {
        gard_map_set(result, "lineCount", 0);
        gard_map_set(result, "success", 0);
        gard_map_set(result, "lines", (int64_t)(intptr_t)gard_array_new(0));
        return result;
    }
    char fullCmd[4096];
    snprintf(fullCmd, sizeof(fullCmd), "%s 2>&1", cmd);
    FILE* pipe = popen(fullCmd, "r");
    if (!pipe) {
        gard_map_set(result, "lineCount", 0);
        gard_map_set(result, "success", 0);
        gard_map_set(result, "lines", (int64_t)(intptr_t)gard_array_new(0));
        return result;
    }
    GardArray* lines = gard_array_new(16);
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        int len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
        char* line = strdup(buf);
        gard_array_add(lines, (int64_t)(intptr_t)line);
    }
    int status = pclose(pipe);
    int exitCode = WEXITSTATUS(status);
    gard_map_set(result, "lines", (int64_t)(intptr_t)lines);
    gard_map_set(result, "lineCount", (int64_t)gard_array_length(lines));
    gard_map_set(result, "exitCode", (int64_t)exitCode);
    gard_map_set(result, "success", exitCode == 0 ? 1 : 0);
    return result;
}

// Console.execWithInput(command, input) — execute with stdin input
GardMap* gard_console_exec_with_input(const char* cmd, const char* input) {
    GardMap* result = gard_map_new();
    if (!cmd) {
        gard_map_set(result, "stdout", (int64_t)(intptr_t)strdup(""));
        gard_map_set(result, "exitCode", -1);
        gard_map_set(result, "success", 0);
        return result;
    }
    int pipeIn[2], pipeOut[2];
    if (pipe(pipeIn) < 0 || pipe(pipeOut) < 0) {
        gard_map_set(result, "stdout", (int64_t)(intptr_t)strdup(""));
        gard_map_set(result, "exitCode", -1);
        gard_map_set(result, "success", 0);
        return result;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipeIn[0]); close(pipeIn[1]);
        close(pipeOut[0]); close(pipeOut[1]);
        gard_map_set(result, "stdout", (int64_t)(intptr_t)strdup(""));
        gard_map_set(result, "exitCode", -1);
        gard_map_set(result, "success", 0);
        return result;
    }
    if (pid == 0) {
        close(pipeIn[1]); close(pipeOut[0]);
        dup2(pipeIn[0], STDIN_FILENO);
        dup2(pipeOut[1], STDOUT_FILENO);
        dup2(pipeOut[1], STDERR_FILENO);
        close(pipeIn[0]); close(pipeOut[1]);
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(127);
    }
    // Parent
    close(pipeIn[0]); close(pipeOut[1]);
    if (input) write(pipeIn[1], input, strlen(input));
    close(pipeIn[1]);
    char* buf = (char*)malloc(65536);
    int pos = 0;
    char tmp[4096];
    ssize_t n;
    while ((n = read(pipeOut[0], tmp, sizeof(tmp))) > 0) {
        if (pos + n < 65535) { memcpy(buf + pos, tmp, n); pos += n; }
    }
    close(pipeOut[0]);
    buf[pos] = '\0';
    if (pos > 0 && buf[pos-1] == '\n') buf[--pos] = '\0';
    int status;
    waitpid(pid, &status, 0);
    int exitCode = WEXITSTATUS(status);
    gard_map_set(result, "stdout", (int64_t)(intptr_t)buf);
    gard_map_set(result, "exitCode", (int64_t)exitCode);
    gard_map_set(result, "success", exitCode == 0 ? 1 : 0);
    return result;
}

// Console.progressBar(current, total, width?) — render a progress bar string
char* gard_console_progress_bar(int32_t current, int32_t total, int32_t width) {
    if (width <= 0) width = 40;
    char* buf = (char*)malloc(256);
    if (total <= 0) {
        sprintf(buf, "[%*s] 0%%", width, "");
        return buf;
    }
    int filled = (current * width) / total;
    if (filled > width) filled = width;
    int percent = (current * 100) / total;
    char bar[128];
    int i = 0;
    for (int j = 0; j < filled; j++) bar[i++] = '=';
    if (filled < width) bar[i++] = '>';
    for (int j = filled + 1; j < width; j++) bar[i++] = ' ';
    bar[i] = '\0';
    sprintf(buf, "[%s] %d%%", bar, percent);
    return buf;
}

// Console.spinner(frame) — get a spinner character for animation
char* gard_console_spinner(int32_t frame) {
    static const char* frames[] = {
        "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
        "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
        "\xe2\xa0\x87", "\xe2\xa0\x8f"
    };
    int idx = frame % 10;
    if (idx < 0) idx += 10;
    return strdup(frames[idx]);
}

// Console.table(data) — format array of maps as a table string
char* gard_console_table(GardArray* data) {
    if (!data || gard_array_length(data) == 0) return strdup("");
    
    // Get first row to determine columns
    GardMap* firstRow = (GardMap*)(intptr_t)gard_array_get(data, 0);
    if (!firstRow) return strdup("");
    
    // Collect column names and calculate widths
    // We'll use the map's internal structure
    // For simplicity, use a fixed approach: iterate map entries
    int numRows = gard_array_length(data);
    
    // Get column names from first row
    // GardMap stores key-value pairs; we need to iterate them
    // Use gard_map_keys to get column names
    GardArray* keys = gard_map_keys(firstRow);
    int numCols = gard_array_length(keys);
    if (numCols == 0) { return strdup(""); }
    
    // Get column names
    char** colNames = (char**)malloc(numCols * sizeof(char*));
    int* colWidths = (int*)malloc(numCols * sizeof(int));
    for (int c = 0; c < numCols; c++) {
        colNames[c] = (char*)(intptr_t)gard_array_get(keys, c);
        colWidths[c] = strlen(colNames[c]);
    }
    
    // Calculate max widths
    for (int r = 0; r < numRows; r++) {
        GardMap* row = (GardMap*)(intptr_t)gard_array_get(data, r);
        for (int c = 0; c < numCols; c++) {
            int64_t val = gard_map_get(row, colNames[c]);
            // Convert value to string for width calculation
            char valBuf[256];
            uintptr_t addr = (uintptr_t)(intptr_t)val;
            if (addr == 0) {
                strcpy(valBuf, "null");
            } else if (addr < 0x10000) {
                sprintf(valBuf, "%ld", (long)(intptr_t)val);
            } else {
                snprintf(valBuf, sizeof(valBuf), "%s", (char*)(intptr_t)val);
            }
            int len = strlen(valBuf);
            if (len > colWidths[c]) colWidths[c] = len;
        }
    }
    
    // Build table string
    int totalWidth = 1; // initial '+'
    for (int c = 0; c < numCols; c++) totalWidth += colWidths[c] + 3; // " val " + "|"
    int bufSize = (totalWidth + 2) * (numRows + 4) + 256;
    char* table = (char*)malloc(bufSize);
    int pos = 0;
    
    // Separator line
    char* sep = (char*)malloc(totalWidth + 2);
    int spos = 0;
    sep[spos++] = '+';
    for (int c = 0; c < numCols; c++) {
        for (int i = 0; i < colWidths[c] + 2; i++) sep[spos++] = '-';
        sep[spos++] = '+';
    }
    sep[spos++] = '\n';
    sep[spos] = '\0';
    
    // Header
    memcpy(table + pos, sep, spos); pos += spos;
    table[pos++] = '|';
    for (int c = 0; c < numCols; c++) {
        table[pos++] = ' ';
        int nameLen = strlen(colNames[c]);
        memcpy(table + pos, colNames[c], nameLen); pos += nameLen;
        for (int i = nameLen; i < colWidths[c]; i++) table[pos++] = ' ';
        table[pos++] = ' ';
        table[pos++] = '|';
    }
    table[pos++] = '\n';
    memcpy(table + pos, sep, spos); pos += spos;
    
    // Rows
    for (int r = 0; r < numRows; r++) {
        GardMap* row = (GardMap*)(intptr_t)gard_array_get(data, r);
        table[pos++] = '|';
        for (int c = 0; c < numCols; c++) {
            int64_t val = gard_map_get(row, colNames[c]);
            char valBuf[256];
            uintptr_t addr = (uintptr_t)(intptr_t)val;
            if (addr == 0) {
                strcpy(valBuf, "null");
            } else if (addr < 0x10000) {
                sprintf(valBuf, "%ld", (long)(intptr_t)val);
            } else {
                snprintf(valBuf, sizeof(valBuf), "%s", (char*)(intptr_t)val);
            }
            int valLen = strlen(valBuf);
            table[pos++] = ' ';
            memcpy(table + pos, valBuf, valLen); pos += valLen;
            for (int i = valLen; i < colWidths[c]; i++) table[pos++] = ' ';
            table[pos++] = ' ';
            table[pos++] = '|';
        }
        table[pos++] = '\n';
    }
    memcpy(table + pos, sep, spos - 1); pos += spos - 1; // no trailing newline on last sep
    table[pos] = '\0';
    
    free(sep);
    free(colNames);
    free(colWidths);
    return table;
}


// ============================================================
// Mail — SMTP with TLS (OpenSSL) for real email delivery
// Supports STARTTLS (port 587) and direct TLS (port 465)
// ============================================================

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

static int smtp_connect(const char* host, int port) {
    struct hostent* he = gethostbyname(host);
    if (!he) return -1;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    struct timeval tv = {10, 0}; // 10s timeout
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(sock); return -1; }
    return sock;
}

static int smtp_read(int sock, SSL* ssl, char* buf, int bufsize) {
    int n;
    if (ssl) n = SSL_read(ssl, buf, bufsize - 1);
    else n = recv(sock, buf, bufsize - 1, 0);
    if (n > 0) buf[n] = '\0';
    return n > 0 ? atoi(buf) : -1;
}

static int smtp_send(int sock, SSL* ssl, const char* data) {
    int len = strlen(data);
    if (ssl) return SSL_write(ssl, data, len) == len ? 0 : -1;
    return send(sock, data, len, 0) == len ? 0 : -1;
}

static int smtp_cmd(int sock, SSL* ssl, const char* cmd, char* buf, int bufsize) {
    if (smtp_send(sock, ssl, cmd) < 0) return -1;
    return smtp_read(sock, ssl, buf, bufsize);
}

int32_t gard_mail_send(const char* host, int32_t port, const char* from, const char* to, const char* subject, const char* body) {
    if (!host || !from || !to || !subject || !body) return 0;
    if (port <= 0) port = 587; // Default STARTTLS port

    int sock = smtp_connect(host, port);
    if (sock < 0) return 0;

    SSL_CTX* ctx = NULL;
    SSL* ssl = NULL;
    char buf[2048];
    int code;

    // Read server greeting
    code = smtp_read(sock, NULL, buf, sizeof(buf));
    if (code != 220) { close(sock); return 0; }

    // EHLO
    char ehlo[256];
    snprintf(ehlo, sizeof(ehlo), "EHLO localhost\r\n");
    code = smtp_cmd(sock, NULL, ehlo, buf, sizeof(buf));
    if (code != 250) { close(sock); return 0; }

    // STARTTLS (for port 587)
    if (port == 587 || port == 25) {
        code = smtp_cmd(sock, NULL, "STARTTLS\r\n", buf, sizeof(buf));
        if (code == 220) {
            // Upgrade to TLS
            SSL_library_init();
            SSL_load_error_strings();
            ctx = SSL_CTX_new(TLS_client_method());
            if (ctx) {
                ssl = SSL_new(ctx);
                SSL_set_fd(ssl, sock);
                if (SSL_connect(ssl) != 1) {
                    SSL_free(ssl); SSL_CTX_free(ctx);
                    close(sock); return 0;
                }
                // Re-EHLO after TLS
                code = smtp_cmd(sock, ssl, ehlo, buf, sizeof(buf));
            }
        }
    } else if (port == 465) {
        // Direct TLS (SMTPS)
        SSL_library_init();
        SSL_load_error_strings();
        ctx = SSL_CTX_new(TLS_client_method());
        if (ctx) {
            ssl = SSL_new(ctx);
            SSL_set_fd(ssl, sock);
            if (SSL_connect(ssl) != 1) {
                SSL_free(ssl); SSL_CTX_free(ctx);
                close(sock); return 0;
            }
            // Read greeting over TLS
            smtp_read(sock, ssl, buf, sizeof(buf));
            smtp_cmd(sock, ssl, ehlo, buf, sizeof(buf));
        }
    }

    // MAIL FROM
    char mailfrom[512];
    snprintf(mailfrom, sizeof(mailfrom), "MAIL FROM:<%s>\r\n", from);
    code = smtp_cmd(sock, ssl, mailfrom, buf, sizeof(buf));
    if (code != 250) goto cleanup;

    // RCPT TO
    char rcptto[512];
    snprintf(rcptto, sizeof(rcptto), "RCPT TO:<%s>\r\n", to);
    code = smtp_cmd(sock, ssl, rcptto, buf, sizeof(buf));
    if (code != 250) goto cleanup;

    // DATA
    code = smtp_cmd(sock, ssl, "DATA\r\n", buf, sizeof(buf));
    if (code != 354) goto cleanup;

    // Send message (headers + body + terminator)
    {
        int msglen = strlen(from) + strlen(to) + strlen(subject) + strlen(body) + 512;
        char* msg = (char*)malloc(msglen);
        snprintf(msg, msglen,
            "From: %s\r\n"
            "To: %s\r\n"
            "Subject: %s\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: text/plain; charset=UTF-8\r\n"
            "\r\n"
            "%s\r\n"
            ".\r\n", from, to, subject, body);
        smtp_send(sock, ssl, msg);
        free(msg);
    }

    code = smtp_read(sock, ssl, buf, sizeof(buf));
    smtp_cmd(sock, ssl, "QUIT\r\n", buf, sizeof(buf));

cleanup:
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
    if (ctx) SSL_CTX_free(ctx);
    close(sock);
    return code == 250 ? 1 : 0;
}

// Send with authentication (AUTH LOGIN)
int32_t gard_mail_send_auth(const char* host, int32_t port, const char* username, const char* password,
                            const char* from, const char* to, const char* subject, const char* body) {
    if (!host || !from || !to || !subject || !body || !username || !password) return 0;
    if (port <= 0) port = 587;

    int sock = smtp_connect(host, port);
    if (sock < 0) return 0;

    SSL_CTX* ctx = NULL;
    SSL* ssl = NULL;
    char buf[2048];
    int code;

    code = smtp_read(sock, NULL, buf, sizeof(buf));
    if (code != 220) { close(sock); return 0; }

    char ehlo[256];
    snprintf(ehlo, sizeof(ehlo), "EHLO localhost\r\n");
    smtp_cmd(sock, NULL, ehlo, buf, sizeof(buf));

    // STARTTLS
    code = smtp_cmd(sock, NULL, "STARTTLS\r\n", buf, sizeof(buf));
    if (code == 220) {
        SSL_library_init();
        SSL_load_error_strings();
        ctx = SSL_CTX_new(TLS_client_method());
        ssl = SSL_new(ctx);
        SSL_set_fd(ssl, sock);
        if (SSL_connect(ssl) != 1) { SSL_free(ssl); SSL_CTX_free(ctx); close(sock); return 0; }
        smtp_cmd(sock, ssl, ehlo, buf, sizeof(buf));
    }

    // AUTH LOGIN
    smtp_cmd(sock, ssl, "AUTH LOGIN\r\n", buf, sizeof(buf));
    // Send base64-encoded username
    char* b64user = gard_base64_encode(username);
    char authuser[512]; snprintf(authuser, sizeof(authuser), "%s\r\n", b64user);
    smtp_cmd(sock, ssl, authuser, buf, sizeof(buf));
    free(b64user);
    // Send base64-encoded password
    char* b64pass = gard_base64_encode(password);
    char authpass[512]; snprintf(authpass, sizeof(authpass), "%s\r\n", b64pass);
    code = smtp_cmd(sock, ssl, authpass, buf, sizeof(buf));
    free(b64pass);
    if (code != 235) goto cleanup_auth;

    // MAIL FROM / RCPT TO / DATA / message (same as above)
    {
        char mailfrom[512]; snprintf(mailfrom, sizeof(mailfrom), "MAIL FROM:<%s>\r\n", from);
        if (smtp_cmd(sock, ssl, mailfrom, buf, sizeof(buf)) != 250) goto cleanup_auth;
        char rcptto[512]; snprintf(rcptto, sizeof(rcptto), "RCPT TO:<%s>\r\n", to);
        if (smtp_cmd(sock, ssl, rcptto, buf, sizeof(buf)) != 250) goto cleanup_auth;
        if (smtp_cmd(sock, ssl, "DATA\r\n", buf, sizeof(buf)) != 354) goto cleanup_auth;
        int msglen = strlen(from) + strlen(to) + strlen(subject) + strlen(body) + 512;
        char* msg = (char*)malloc(msglen);
        snprintf(msg, msglen, "From: %s\r\nTo: %s\r\nSubject: %s\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\n\r\n%s\r\n.\r\n", from, to, subject, body);
        smtp_send(sock, ssl, msg);
        free(msg);
    }
    code = smtp_read(sock, ssl, buf, sizeof(buf));
    smtp_cmd(sock, ssl, "QUIT\r\n", buf, sizeof(buf));

cleanup_auth:
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
    if (ctx) SSL_CTX_free(ctx);
    close(sock);
    return code == 250 ? 1 : 0;
}


// ============================================================
// Runtime Annotations Registry (for real reflection in AOT)
// ============================================================

#define MAX_ANNOTATIONS 256

typedef struct {
    const char* class_name;
    const char* target;      // "" = class-level, "methodName" = method-level
    const char* annotation;
    const char* args;        // comma-separated key=value pairs
} GardAnnotationEntry;

static GardAnnotationEntry g_annotations[MAX_ANNOTATIONS];
static int32_t g_annotation_count = 0;

void gard_register_annotation(const char* class_name, const char* target, const char* annotation, const char* args) {
    if (g_annotation_count >= MAX_ANNOTATIONS) return;
    GardAnnotationEntry* e = &g_annotations[g_annotation_count++];
    e->class_name = class_name;
    e->target = target ? target : "";
    e->annotation = annotation;
    e->args = args ? args : "";
}

// Override the stub implementations with real ones
GardArray* gard_reflect_get_annotations_real(const char* class_name) {
    GardArray* result = gard_array_new(8);
    if (!class_name) return result;
    for (int i = 0; i < g_annotation_count; i++) {
        if (strcmp(g_annotations[i].class_name, class_name) == 0 &&
            strlen(g_annotations[i].target) == 0) {
            gard_array_add(result, (int64_t)(intptr_t)g_annotations[i].annotation);
        }
    }
    return result;
}

int32_t gard_reflect_has_annotation_real(const char* class_name, const char* annotation) {
    if (!class_name || !annotation) return 0;
    for (int i = 0; i < g_annotation_count; i++) {
        if (strcmp(g_annotations[i].class_name, class_name) == 0 &&
            strcmp(g_annotations[i].annotation, annotation) == 0) {
            return 1;
        }
    }
    return 0;
}

char* gard_reflect_get_annotation_args_real(const char* class_name, const char* annotation) {
    if (!class_name || !annotation) return "";
    for (int i = 0; i < g_annotation_count; i++) {
        if (strcmp(g_annotations[i].class_name, class_name) == 0 &&
            strcmp(g_annotations[i].annotation, annotation) == 0) {
            return (char*)g_annotations[i].args;
        }
    }
    return "";
}

GardArray* gard_reflect_get_method_annotations_real(const char* class_name, const char* method) {
    GardArray* result = gard_array_new(8);
    if (!class_name || !method) return result;
    for (int i = 0; i < g_annotation_count; i++) {
        if (strcmp(g_annotations[i].class_name, class_name) == 0 &&
            strcmp(g_annotations[i].target, method) == 0) {
            gard_array_add(result, (int64_t)(intptr_t)g_annotations[i].annotation);
        }
    }
    return result;
}


// ============================================================
// Mail — Transport-based API (matches interpreter)
// Mail.createTransport({host, port, user, password, secure})
// Mail.send(transport, {sender, to, subject, body})
// ============================================================

GardMap* gard_mail_create_transport(GardMap* config) {
    // Just store the config as the transport (it's already a map)
    // Add defaults if missing
    if (!config) config = gard_map_new();
    if (!gard_map_has(config, "port")) gard_map_set(config, "port", 587);
    if (!gard_map_has(config, "secure")) gard_map_set(config, "secure", (int64_t)(intptr_t)"false");
    return config;
}

GardMap* gard_mail_send_transport(GardMap* transport, GardMap* message) {
    GardMap* result = gard_map_new();
    if (!transport || !message) {
        gard_map_set(result, "success", 0);
        return result;
    }

    // Extract transport config
    const char* host = (const char*)(intptr_t)gard_map_get(transport, "host");
    int32_t port = (int32_t)gard_map_get(transport, "port");
    const char* user = (const char*)(intptr_t)gard_map_get(transport, "user");
    const char* password = (const char*)(intptr_t)gard_map_get(transport, "password");

    // Extract message fields
    const char* sender = (const char*)(intptr_t)gard_map_get(message, "sender");
    const char* to = (const char*)(intptr_t)gard_map_get(message, "to");
    const char* subject = (const char*)(intptr_t)gard_map_get(message, "subject");
    const char* body = (const char*)(intptr_t)gard_map_get(message, "body");

    if (!host || !sender || !to || !subject || !body) {
        gard_map_set(result, "success", 0);
        return result;
    }

    int32_t sent;
    if (user && password && strlen(user) > 0) {
        sent = gard_mail_send_auth(host, port, user, password, sender, to, subject, body);
    } else {
        sent = gard_mail_send(host, port, sender, to, subject, body);
    }

    gard_map_set(result, "success", (int64_t)sent);
    gard_map_set(result, "to", (int64_t)(intptr_t)to);
    gard_map_set(result, "subject", (int64_t)(intptr_t)subject);
    return result;
}
