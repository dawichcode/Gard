// Gard FFI Runtime — Foreign Function Interface for AOT
// Allows Gard programs to load shared libraries and call C functions at runtime.
// Uses dlopen/dlsym/dlclose + libffi for type-safe calling conventions.

#ifndef GARD_RUNTIME_FFI_H
#define GARD_RUNTIME_FFI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles
typedef struct GardFFILibrary GardFFILibrary;
typedef struct GardFFIFunction GardFFIFunction;

// FFI.loadLibrary(path) — dlopen a shared library
// path: "libfoo.so", "./mylib.so", or full path
GardFFILibrary* gard_ffi_load_library(const char* path);

// FFI.getFunction(lib, name) — dlsym to get function pointer
GardFFIFunction* gard_ffi_get_function(GardFFILibrary* lib, const char* name);

// FFI.call(fn, ...args) — invoke with int args, returns int
int64_t gard_ffi_call_int(GardFFIFunction* fn, int64_t a0, int64_t a1, int64_t a2, int64_t a3);

// FFI.callString(fn, ...args) — invoke, returns string (char*)
const char* gard_ffi_call_string(GardFFIFunction* fn, int64_t a0, int64_t a1, int64_t a2, int64_t a3);

// FFI.callVoid(fn, ...args) — invoke, no return
void gard_ffi_call_void(GardFFIFunction* fn, int64_t a0, int64_t a1, int64_t a2, int64_t a3);

// FFI.closeLibrary(lib) — dlclose
void gard_ffi_close_library(GardFFILibrary* lib);

// FFI.getLastError() — dlerror
const char* gard_ffi_get_last_error(void);

// === Pointer operations ===

// Pointer.alloc(size) — malloc
void* gard_pointer_alloc(int64_t size);

// Pointer.free(ptr) — free
void gard_pointer_free(void* ptr);

// Pointer.readInt(ptr, offset) — read int32 at offset
int32_t gard_pointer_read_int(void* ptr, int32_t offset);

// Pointer.writeInt(ptr, offset, value) — write int32 at offset
void gard_pointer_write_int(void* ptr, int32_t offset, int32_t value);

// Pointer.readLong(ptr, offset) — read int64 at offset
int64_t gard_pointer_read_long(void* ptr, int32_t offset);

// Pointer.writeLong(ptr, offset, value) — write int64 at offset
void gard_pointer_write_long(void* ptr, int32_t offset, int64_t value);

// Pointer.readString(ptr) — read null-terminated C string
const char* gard_pointer_read_string(void* ptr);

// Pointer.writeString(ptr, str) — write string to memory (including null terminator)
void gard_pointer_write_string(void* ptr, const char* str);

// Pointer.readByte(ptr, offset) — read single byte
int32_t gard_pointer_read_byte(void* ptr, int32_t offset);

// Pointer.writeByte(ptr, offset, value) — write single byte
void gard_pointer_write_byte(void* ptr, int32_t offset, int32_t value);

// Pointer.copy(dest, src, size) — memcpy
void gard_pointer_copy(void* dest, void* src, int64_t size);

// Pointer.zero(ptr, size) — memset to 0
void gard_pointer_zero(void* ptr, int64_t size);

// Pointer.toInt(ptr) — cast pointer to integer
int64_t gard_pointer_to_int(void* ptr);

// Pointer.fromInt(val) — cast integer to pointer
void* gard_pointer_from_int(int64_t val);

#ifdef __cplusplus
}
#endif

#endif // GARD_RUNTIME_FFI_H
