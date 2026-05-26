// Gard FFI Runtime — Foreign Function Interface
// Real dlopen/dlsym/dlclose with direct function pointer invocation.
// Compile: gcc -c -O2 -fPIC gard_runtime_ffi.c -o gard_runtime_ffi.o
// Link: -ldl

#include "gard_runtime_ffi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

// ============================================================
// FFI Library / Function handles
// ============================================================

struct GardFFILibrary {
    void* handle;
    char* path;
};

struct GardFFIFunction {
    void* fn_ptr;
    char* name;
    GardFFILibrary* lib;
};

// Typedef for function pointer types we support
typedef int64_t (*ffi_func_i64_4)(int64_t, int64_t, int64_t, int64_t);
typedef const char* (*ffi_func_str_4)(int64_t, int64_t, int64_t, int64_t);
typedef void (*ffi_func_void_4)(int64_t, int64_t, int64_t, int64_t);

// ============================================================
// FFI.loadLibrary
// ============================================================

GardFFILibrary* gard_ffi_load_library(const char* path) {
    if (!path) return NULL;

    // Try loading as-is first
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);

    // If failed and path doesn't contain '/', try with lib prefix and .so suffix
    if (!handle && !strchr(path, '/')) {
        char buf[512];
        // Try: lib<name>.so
        snprintf(buf, sizeof(buf), "lib%s.so", path);
        handle = dlopen(buf, RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            // Try: ./<name>.so
            snprintf(buf, sizeof(buf), "./%s.so", path);
            handle = dlopen(buf, RTLD_NOW | RTLD_LOCAL);
        }
        if (!handle) {
            // Try: <name> (maybe it's already a full name)
            snprintf(buf, sizeof(buf), "./%s", path);
            handle = dlopen(buf, RTLD_NOW | RTLD_LOCAL);
        }
    }

    if (!handle) return NULL;

    GardFFILibrary* lib = (GardFFILibrary*)calloc(1, sizeof(GardFFILibrary));
    lib->handle = handle;
    lib->path = strdup(path);
    return lib;
}

// ============================================================
// FFI.getFunction
// ============================================================

GardFFIFunction* gard_ffi_get_function(GardFFILibrary* lib, const char* name) {
    if (!lib || !lib->handle || !name) return NULL;

    dlerror(); // Clear previous errors
    void* fn = dlsym(lib->handle, name);
    char* err = dlerror();
    if (err || !fn) return NULL;

    GardFFIFunction* func = (GardFFIFunction*)calloc(1, sizeof(GardFFIFunction));
    func->fn_ptr = fn;
    func->name = strdup(name);
    func->lib = lib;
    return func;
}

// ============================================================
// FFI.call variants — direct function pointer invocation
// Uses the platform's C calling convention (System V AMD64 ABI on x86_64)
// Arguments are passed as int64_t (can hold int, pointer, or float bits)
// ============================================================

int64_t gard_ffi_call_int(GardFFIFunction* fn, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
    if (!fn || !fn->fn_ptr) return 0;
    ffi_func_i64_4 f = (ffi_func_i64_4)fn->fn_ptr;
    return f(a0, a1, a2, a3);
}

const char* gard_ffi_call_string(GardFFIFunction* fn, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
    if (!fn || !fn->fn_ptr) return "";
    ffi_func_str_4 f = (ffi_func_str_4)fn->fn_ptr;
    const char* result = f(a0, a1, a2, a3);
    return result ? result : "";
}

void gard_ffi_call_void(GardFFIFunction* fn, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
    if (!fn || !fn->fn_ptr) return;
    ffi_func_void_4 f = (ffi_func_void_4)fn->fn_ptr;
    f(a0, a1, a2, a3);
}

// ============================================================
// FFI.closeLibrary
// ============================================================

void gard_ffi_close_library(GardFFILibrary* lib) {
    if (!lib) return;
    if (lib->handle) dlclose(lib->handle);
    if (lib->path) free(lib->path);
    free(lib);
}

// ============================================================
// FFI.getLastError
// ============================================================

const char* gard_ffi_get_last_error(void) {
    const char* err = dlerror();
    return err ? err : "";
}

// ============================================================
// Pointer operations — raw memory access
// ============================================================

void* gard_pointer_alloc(int64_t size) {
    if (size <= 0) return NULL;
    return calloc(1, (size_t)size);
}

void gard_pointer_free(void* ptr) {
    if (ptr) free(ptr);
}

int32_t gard_pointer_read_int(void* ptr, int32_t offset) {
    if (!ptr) return 0;
    return *(int32_t*)((char*)ptr + offset);
}

void gard_pointer_write_int(void* ptr, int32_t offset, int32_t value) {
    if (!ptr) return;
    *(int32_t*)((char*)ptr + offset) = value;
}

int64_t gard_pointer_read_long(void* ptr, int32_t offset) {
    if (!ptr) return 0;
    return *(int64_t*)((char*)ptr + offset);
}

void gard_pointer_write_long(void* ptr, int32_t offset, int64_t value) {
    if (!ptr) return;
    *(int64_t*)((char*)ptr + offset) = value;
}

const char* gard_pointer_read_string(void* ptr) {
    if (!ptr) return "";
    return (const char*)ptr;
}

void gard_pointer_write_string(void* ptr, const char* str) {
    if (!ptr || !str) return;
    strcpy((char*)ptr, str);
}

int32_t gard_pointer_read_byte(void* ptr, int32_t offset) {
    if (!ptr) return 0;
    return (int32_t)(unsigned char)((char*)ptr)[offset];
}

void gard_pointer_write_byte(void* ptr, int32_t offset, int32_t value) {
    if (!ptr) return;
    ((char*)ptr)[offset] = (char)value;
}

void gard_pointer_copy(void* dest, void* src, int64_t size) {
    if (!dest || !src || size <= 0) return;
    memcpy(dest, src, (size_t)size);
}

void gard_pointer_zero(void* ptr, int64_t size) {
    if (!ptr || size <= 0) return;
    memset(ptr, 0, (size_t)size);
}

int64_t gard_pointer_to_int(void* ptr) {
    return (int64_t)(intptr_t)ptr;
}

void* gard_pointer_from_int(int64_t val) {
    return (void*)(intptr_t)val;
}
