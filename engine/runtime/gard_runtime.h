// Gard Native Runtime Library
// Provides C implementations of core stdlib functions for AOT-compiled binaries.
// Linked via: clang program.o -lgard_runtime -lm

#ifndef GARD_RUNTIME_H
#define GARD_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Runtime Initialization & Panic System
// ============================================================

// Initialize the Gard runtime (signal handlers, panic system).
// Must be called at the start of every compiled main().
void gard_runtime_init(void);

// Set the source file name for error reporting.
void gard_set_source_file(const char* file);

// Panic — prints a formatted error and aborts.
// type: error class name (e.g. "NullReferenceError")
// message: human-readable description
void gard_panic(const char* type, const char* message);

// Specialized panic functions (called from generated null/bounds guards)
void gard_panic_null(const char* function, int32_t line, int32_t col);
void gard_panic_index(int32_t index, int32_t length, const char* function);
void gard_panic_arithmetic(const char* op, const char* function, int32_t line, int32_t col);

// ============================================================
// Exception Handling (setjmp/longjmp based)
// ============================================================

#include <setjmp.h>

// Exception handler frame — linked list of jmpbufs
typedef struct GardExceptionFrame {
    jmp_buf buf;
    struct GardExceptionFrame* prev;
} GardExceptionFrame;

// Current exception handler (thread-local for multi-threading)
extern __thread GardExceptionFrame* __gard_exception_frame;
extern __thread const char* __gard_exception_msg;
extern __thread const char* __gard_exception_type;
extern __thread int32_t __gard_exception_line;
extern __thread int32_t __gard_exception_col;
extern __thread const char* __gard_exception_caller;

// Push/pop exception frames (called from generated code)
void gard_exception_push(GardExceptionFrame* frame);
void gard_exception_pop(void);

// Throw an exception — longjmps to nearest handler or panics if none
void gard_throw(const char* type, const char* message);

// Check if there's an active exception handler
int32_t gard_has_exception_handler(void);

// ============================================================
// Array (List) operations
// Layout: [capacity:i32][length:i32][elem0:i64][elem1:i64]...
// Each element is 8 bytes (can hold i32, i64, or ptr)
// ============================================================

typedef struct {
    int32_t capacity;
    int32_t length;
    int64_t data[];  // flexible array member
} GardArray;

GardArray* gard_array_new(int32_t initial_capacity);
void gard_array_add(GardArray* arr, int64_t value);
int64_t gard_array_get(GardArray* arr, int32_t index);
void gard_array_set(GardArray* arr, int32_t index, int64_t value);
int32_t gard_array_length(GardArray* arr);
int64_t gard_array_pop(GardArray* arr);
int32_t gard_array_contains(GardArray* arr, int64_t value);
void gard_array_remove_at(GardArray* arr, int32_t index);
int32_t gard_array_index_of(GardArray* arr, int64_t value);
void gard_array_reverse(GardArray* arr);
void gard_array_sort(GardArray* arr);
GardArray* gard_array_slice(GardArray* arr, int32_t start, int32_t end);
char* gard_array_join(GardArray* arr, const char* separator);
void gard_array_add_all(GardArray* dest, GardArray* src);
int64_t gard_array_remove(GardArray* arr, int64_t value);

// ============================================================
// String operations
// ============================================================

char* gard_string_concat(const char* a, const char* b);
int32_t gard_string_length(const char* s);
int32_t gard_string_equals(const char* a, const char* b);
char* gard_string_substring(const char* s, int32_t start, int32_t end);
int32_t gard_string_index_of(const char* s, const char* sub);
int32_t gard_string_last_index_of(const char* s, const char* sub);
char* gard_string_to_upper(const char* s);
char* gard_string_to_lower(const char* s);
char* gard_string_replace(const char* s, const char* old_str, const char* new_str);
char* gard_string_replace_all(const char* s, const char* old_str, const char* new_str);
char* gard_string_trim(const char* s);
char* gard_string_trim_start(const char* s);
char* gard_string_trim_end(const char* s);
int32_t gard_string_starts_with(const char* s, const char* prefix);
int32_t gard_string_ends_with(const char* s, const char* suffix);
int32_t gard_string_contains(const char* s, const char* sub);
char* gard_string_repeat(const char* s, int32_t count);
char* gard_string_pad_start(const char* s, int32_t target_len, const char* pad);
char* gard_string_pad_end(const char* s, int32_t target_len, const char* pad);
char* gard_string_slice(const char* s, int32_t start, int32_t end);
char* gard_string_char_at(const char* s, int32_t index);
int32_t gard_string_char_code_at(const char* s, int32_t index);
GardArray* gard_string_split(const char* s, const char* delimiter);

// ============================================================
// Type conversion
// ============================================================

char* gard_int_to_string(int32_t value);
char* gard_long_to_string(int64_t value);
char* gard_double_to_string(double value);
char* gard_bool_to_string(int32_t value);
int32_t gard_string_to_int(const char* s);
double gard_string_to_double(const char* s);

// ============================================================
// Print
// ============================================================

void gard_print_int(int32_t value);
void gard_print_str(const char* value);
void gard_print_bool(int32_t value);
void gard_print_double(double value);
void gard_print_long(int64_t value);
void gard_print_exception(void);

// ============================================================
// Math
// ============================================================

int32_t gard_math_abs(int32_t x);
int32_t gard_math_max(int32_t a, int32_t b);
int32_t gard_math_min(int32_t a, int32_t b);
double gard_math_sqrt(double x);
double gard_math_pow(double base, double exp);
int32_t gard_math_round(double x);
int32_t gard_math_floor(double x);
int32_t gard_math_ceil(double x);
double gard_math_sin(double x);
double gard_math_cos(double x);
double gard_math_tan(double x);
double gard_math_asin(double x);
double gard_math_acos(double x);
double gard_math_atan(double x);
double gard_math_atan2(double y, double x);
double gard_math_log(double x);
double gard_math_log2(double x);
double gard_math_log10(double x);
double gard_math_exp(double x);
double gard_math_cbrt(double x);
int32_t gard_math_trunc(double x);
int32_t gard_math_sign(double x);
double gard_math_clamp(double val, double lo, double hi);
double gard_math_random(void);
int32_t gard_math_random_int(int32_t min, int32_t max);
double gard_math_pi(void);
double gard_math_e(void);

// ============================================================
// Map operations
// Simple hash map: string keys → int64_t values (can hold int or ptr)
// ============================================================

typedef struct GardMapEntry {
    char* key;
    int64_t value;
    struct GardMapEntry* next;
} GardMapEntry;

typedef struct {
    uint32_t magic;         // 0x4D415047 ("MAPG") for validation
    GardMapEntry** buckets;
    int32_t bucket_count;
    int32_t size;
} GardMap;

#define GARD_MAP_MAGIC 0x4D415047U

GardMap* gard_map_new(void);
void gard_map_set(GardMap* map, const char* key, int64_t value);
int64_t gard_map_get(GardMap* map, const char* key);
int32_t gard_map_has(GardMap* map, const char* key);
int32_t gard_map_remove(GardMap* map, const char* key);
int32_t gard_map_size(GardMap* map);
GardArray* gard_map_keys(GardMap* map);
GardArray* gard_map_values(GardMap* map);
void gard_map_free(GardMap* map);

// ============================================================
// Memory
// ============================================================

void* gard_malloc(int64_t size);
void gard_free(void* ptr);

// ============================================================
// File I/O
// ============================================================

char* gard_file_read_text(const char* path);
int32_t gard_file_write_text(const char* path, const char* content);
int32_t gard_file_append_text(const char* path, const char* content);
int32_t gard_file_exists(const char* path);
int64_t gard_file_size(const char* path);
int32_t gard_file_delete(const char* path);
int32_t gard_file_copy(const char* src, const char* dst);
int32_t gard_file_move(const char* src, const char* dst);

// ============================================================
// Directory
// ============================================================

int32_t gard_directory_create(const char* path);
int32_t gard_directory_exists(const char* path);
int32_t gard_directory_delete(const char* path);
GardArray* gard_directory_list(const char* path);

// ============================================================
// Path utilities
// ============================================================

char* gard_path_join(const char* a, const char* b);
char* gard_path_extension(const char* path);
char* gard_path_dirname(const char* path);
char* gard_path_basename(const char* path);
char* gard_path_resolve(const char* path);

// ============================================================
// DateTime
// ============================================================

int64_t gard_datetime_now(void);
char* gard_datetime_format(int64_t epoch_ms);
int64_t gard_datetime_add_days(int64_t epoch_ms, int32_t days);
int64_t gard_datetime_add_hours(int64_t epoch_ms, int32_t hours);
int64_t gard_datetime_add_minutes(int64_t epoch_ms, int32_t minutes);
int32_t gard_datetime_is_before(int64_t a, int64_t b);
int32_t gard_datetime_is_after(int64_t a, int64_t b);
int64_t gard_datetime_subtract(int64_t a, int64_t b);

// ============================================================
// JSON (simplified — string-based)
// ============================================================

char* gard_json_stringify_int(int32_t value);
char* gard_json_stringify_str(const char* value);
char* gard_json_stringify_bool(int32_t value);
int32_t gard_json_is_valid(const char* s);

// ============================================================
// Process
// ============================================================

int32_t gard_process_pid(void);
char* gard_process_platform(void);
char* gard_process_arch(void);
char* gard_process_cwd(void);
char* gard_process_env(const char* name);
void gard_process_exit(int32_t code);
char* gard_process_execute(const char* command);

// ============================================================
// Console
// ============================================================

char* gard_console_read_line(void);
void gard_console_exec(const char* cmd);
char* gard_console_exec_stdout(void);
int32_t gard_console_exec_exit_code(void);
int32_t gard_console_exec_success(void);
int32_t gard_console_exec_silent(const char* cmd);
char* gard_console_which(const char* program);
int32_t gard_console_is_available(const char* program);
void gard_console_write(const char* str);
void gard_console_write_line(const char* str);
char* gard_console_color(const char* text, const char* color);
int32_t gard_console_is_interactive(void);
GardMap* gard_console_exec_full(const char* cmd);
GardMap* gard_console_exec_timeout_full(const char* cmd, int32_t timeout_ms);
GardMap* gard_console_size_full(void);
GardMap* gard_console_exec_pipe(const char* cmd);
GardMap* gard_console_exec_with_input(const char* cmd, const char* input);
char* gard_console_progress_bar(int32_t current, int32_t total, int32_t width);
char* gard_console_spinner(int32_t frame);
char* gard_console_table(GardArray* data);

// ============================================================
// Crypto (bundled — no OpenSSL needed)
// ============================================================

char* gard_hash_sha256(const char* data);
char* gard_hash_sha256_bytes(const void* data, int32_t len);
char* gard_hash_sha1(const char* data);
char* gard_hash_sha512(const char* data);
char* gard_hash_sha256_file(const char* path);
char* gard_hash_hmac_sha256(const char* key, const char* data);
char* gard_crypto_generate_key(int32_t bits);
GardArray* gard_crypto_get_random_values(int32_t size);
// AES-256 (simplified XOR-based for standalone binaries — real AES needs OpenSSL)
char* gard_crypto_encrypt(const char* plaintext, const char* key);
char* gard_crypto_decrypt(const char* ciphertext_hex, const char* key);
// RSA stubs (signature-compatible, simplified for standalone)
char* gard_rsa_generate_key_pair(int32_t bits);
char* gard_rsa_encrypt(const char* data, const char* key);
char* gard_rsa_decrypt(const char* data, const char* key);
char* gard_rsa_sign(const char* data, const char* key);
int32_t gard_rsa_verify(const char* data, const char* signature, const char* key);

// ============================================================
// Base64
// ============================================================

char* gard_base64_encode(const char* data);
char* gard_base64_decode(const char* data);
char* gard_base64_encode_url_safe(const char* data);
char* gard_base64_decode_url_safe(const char* data);

// ============================================================
// Regex (POSIX regex)
// ============================================================

int32_t gard_regex_test(const char* pattern, const char* str);
char* gard_regex_match(const char* pattern, const char* str);
char* gard_regex_replace(const char* pattern, const char* str, const char* replacement);
int32_t gard_regex_matches(const char* pattern, const char* str);
GardArray* gard_regex_split(const char* pattern, const char* str);
GardArray* gard_regex_match_groups(const char* pattern, const char* str);
char* gard_regex_replace_all(const char* pattern, const char* str, const char* replacement);

// ============================================================
// Collections — Extended (Queue, Stack, Set, PriorityQueue, Iterator)
// All operate on GardArray as the underlying storage.
// ============================================================

// Queue (FIFO — front=index 0)
void gard_queue_enqueue(GardArray* arr, int64_t value);
int64_t gard_queue_dequeue(GardArray* arr);
int64_t gard_queue_peek(GardArray* arr);
int32_t gard_queue_is_empty(GardArray* arr);
int32_t gard_queue_size(GardArray* arr);

// Stack (LIFO — top=last element) — namespace versions
void gard_stack_push(GardArray* arr, int64_t value);
int64_t gard_stack_pop(GardArray* arr);
int64_t gard_stack_peek(GardArray* arr);
int32_t gard_stack_is_empty(GardArray* arr);
int32_t gard_stack_size(GardArray* arr);

// Set (unique elements, backed by array)
void gard_set_add(GardArray* arr, int64_t value);
int32_t gard_set_remove(GardArray* arr, int64_t value);
int32_t gard_set_contains(GardArray* arr, int64_t value);
int32_t gard_set_size(GardArray* arr);
GardArray* gard_set_union(GardArray* a, GardArray* b);
GardArray* gard_set_intersection(GardArray* a, GardArray* b);
GardArray* gard_set_difference(GardArray* a, GardArray* b);

// PriorityQueue (sorted array, smallest first)
void gard_pq_enqueue(GardArray* arr, int64_t value);
int64_t gard_pq_dequeue(GardArray* arr);
int64_t gard_pq_peek(GardArray* arr);
int32_t gard_pq_size(GardArray* arr);
int32_t gard_pq_is_empty(GardArray* arr);

// List — higher-order operations (simplified for AOT — no closures)
int64_t gard_list_find(GardArray* arr, int64_t value);
int32_t gard_list_find_index(GardArray* arr, int64_t value);
int32_t gard_list_every_nonzero(GardArray* arr);
int32_t gard_list_some_nonzero(GardArray* arr);
int64_t gard_list_reduce_sum(GardArray* arr, int64_t initial);
GardArray* gard_list_filter_nonzero(GardArray* arr);
GardArray* gard_list_compact(GardArray* arr);

// Map — additional
GardArray* gard_map_entries(GardMap* map);
int32_t gard_map_delete(GardMap* map, const char* key);

// Iterator
int32_t gard_iterator_has_next(GardArray* arr, int32_t index);
int64_t gard_iterator_next(GardArray* arr, int32_t index);

// ============================================================
// Compression (bundled deflate/inflate — no zlib dependency)
// ============================================================

char* gard_compress(const char* data);
char* gard_decompress(const char* hex_data);
int32_t gard_compress_file(const char* input_path, const char* output_path);
int32_t gard_decompress_file(const char* input_path, const char* output_path);

// ============================================================
// XML (lightweight DOM — no libxml2 dependency)
// ============================================================

typedef struct GardXmlNode {
    char* tag;
    char* text;
    GardMap* attributes;
    struct GardXmlNode** children;
    int32_t child_count;
    int32_t child_capacity;
} GardXmlNode;

GardXmlNode* gard_xml_parse(const char* xml_str);
GardXmlNode* gard_xml_create_element(const char* tag);
void gard_xml_set_attribute(GardXmlNode* node, const char* key, const char* value);
void gard_xml_set_text(GardXmlNode* node, const char* text);
void gard_xml_append_child(GardXmlNode* parent, GardXmlNode* child);
char* gard_xml_to_string(GardXmlNode* node);
char* gard_xml_get_attribute(GardXmlNode* node, const char* key);
char* gard_xml_get_text(GardXmlNode* node);
char* gard_xml_get_tag(GardXmlNode* node);
int32_t gard_xml_child_count(GardXmlNode* node);
GardXmlNode* gard_xml_get_child(GardXmlNode* node, int32_t index);
void gard_xml_free(GardXmlNode* node);

// ============================================================
// Concurrency & Async (pthreads-based)
// ============================================================

// Channel (thread-safe bounded queue)
typedef struct GardChannel GardChannel;
GardChannel* gard_channel_new(int32_t capacity);
int32_t gard_channel_send(GardChannel* ch, int64_t value);
int64_t gard_channel_receive(GardChannel* ch);
void gard_channel_close(GardChannel* ch);
int32_t gard_channel_is_closed(GardChannel* ch);

// Mutex
typedef struct GardMutex GardMutex;
GardMutex* gard_mutex_new(void);
int32_t gard_mutex_lock(GardMutex* m);
int32_t gard_mutex_unlock(GardMutex* m);
void gard_mutex_free(GardMutex* m);

// Semaphore
typedef struct GardSemaphore GardSemaphore;
GardSemaphore* gard_semaphore_new(int32_t permits);
int32_t gard_semaphore_acquire(GardSemaphore* s);
int32_t gard_semaphore_release(GardSemaphore* s);
void gard_semaphore_free(GardSemaphore* s);

// Barrier
typedef struct GardBarrier GardBarrier;
GardBarrier* gard_barrier_new(int32_t count);
void gard_barrier_wait(GardBarrier* b);
void gard_barrier_free(GardBarrier* b);

// Async utilities
void gard_delay(int32_t ms);
int64_t gard_timer_start(void);
int64_t gard_timer_elapsed_ms(int64_t start);

// ============================================================
// Performance & System (Tier 7)
// ============================================================

// System info
char* gard_system_platform(void);
char* gard_system_arch(void);
int32_t gard_system_cpu_count(void);
int64_t gard_system_memory_total(void);
int64_t gard_system_memory_free(void);

// Compiler introspection
int32_t gard_compiler_is_optimized(void);
char* gard_compiler_version(void);

// MMap (memory-mapped I/O)
typedef struct GardMMap GardMMap;
GardMMap* gard_mmap_open(const char* path, int64_t size);
char* gard_mmap_read(GardMMap* m, int32_t offset, int32_t length);
int32_t gard_mmap_write(GardMMap* m, int32_t offset, const char* data);
void gard_mmap_sync(GardMMap* m);
void gard_mmap_close(GardMMap* m);

// ConcurrentMap (thread-safe map using mutex)
typedef struct GardConcurrentMap GardConcurrentMap;
GardConcurrentMap* gard_concurrent_map_new(void);
void gard_concurrent_map_set(GardConcurrentMap* m, const char* key, int64_t value);
int64_t gard_concurrent_map_get(GardConcurrentMap* m, const char* key);
int32_t gard_concurrent_map_has(GardConcurrentMap* m, const char* key);
int32_t gard_concurrent_map_remove(GardConcurrentMap* m, const char* key);
int32_t gard_concurrent_map_size(GardConcurrentMap* m);

// ============================================================
// Mock Framework (Tier 8)
// Mock objects store method call counts and configured return values.
// ============================================================

typedef struct GardMock GardMock;
GardMock* gard_mock_create(const char* name);
void gard_mock_when(GardMock* mock, const char* method, int64_t return_value);
int64_t gard_mock_call(GardMock* mock, const char* method);
int32_t gard_mock_call_count(GardMock* mock, const char* method);
int32_t gard_mock_called_once(GardMock* mock, const char* method);
int32_t gard_mock_called_times(GardMock* mock, const char* method, int32_t expected);
int32_t gard_mock_never_called(GardMock* mock, const char* method);
void gard_mock_reset(GardMock* mock);

// ============================================================
// Reflection (Tier 9)
// In AOT mode, annotation metadata is not available at runtime.
// These return empty/false as stubs — API-compatible with interpreter.
// ============================================================

GardArray* gard_reflect_get_annotations(const char* class_name);
int32_t gard_reflect_has_annotation(const char* class_name, const char* annotation);
char* gard_reflect_get_annotation_args(const char* class_name, const char* annotation);
GardArray* gard_reflect_get_method_annotations(const char* class_name, const char* method);
GardArray* gard_reflect_get_field_annotations(const char* class_name, const char* field);

// ============================================================
// Type checking (for 'is' operator)
// ============================================================

int32_t gard_is_type(const char* obj_class, const char* target_class);

// ============================================================
// Watchdog — Runtime Recovery & Monitoring (Tier 8)
// Optional: programs work fine without starting the watchdog.
// See gard_watchdog.h for full documentation.
// ============================================================

#include "gard_watchdog.h"

// ============================================================
// Crash Report & Structured Logging (Tier 9)
// See gard_crash_report.h for full documentation.
// ============================================================

#include "gard_crash_report.h"

// ============================================================
// Higher-order array operations (map, filter, reduce, forEach)
// ============================================================

typedef int64_t (*GardMapFn)(int64_t);
typedef int32_t (*GardFilterFn)(int64_t);
typedef int64_t (*GardReduceFn)(int64_t, int64_t);

GardArray* gard_array_map(GardArray* arr, GardMapFn fn);
GardArray* gard_array_filter(GardArray* arr, GardFilterFn fn);
int64_t gard_array_reduce(GardArray* arr, GardReduceFn fn, int64_t initial);
void gard_array_foreach(GardArray* arr, GardMapFn fn);

#ifdef __cplusplus
}
#endif

#endif // GARD_RUNTIME_H
