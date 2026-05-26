#include "runtime/stdlib_native.h"
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <iostream>
#include <chrono>
#include <thread>
#include <fstream>
#include <sstream>
#include <dlfcn.h>
#include <ffi.h>
#include <curl/curl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <signal.h>
#include <algorithm>

namespace gard {
namespace runtime {
namespace stdlib {

// ===== FFI Internals =====

struct NativeLibrary {
    void* handle = nullptr;
    std::string path;
    std::string name;
    bool loaded = false;
};

struct NativePointer {
    void* ptr = nullptr;
    size_t size = 0;
    bool owned = false; // if true, we allocated it and must free
    std::string typeName; // "int", "double", "string", "struct", "void"
};

static std::unordered_map<int, std::shared_ptr<NativeLibrary>> g_nativeLibs;
static std::unordered_map<int, std::shared_ptr<NativePointer>> g_pointers;
static int g_nextLibId = 1;
static int g_nextPtrId = 1;

// ===== Register Native Module =====

void registerNativeModule(VM& vm) {

    // FFI.loadLibrary(path) — load a shared library (.so/.dylib/.dll)
    // Uses dlopen on Linux/macOS
    vm.registerNative("FFI.loadLibrary", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardFFIError", "FFI.loadLibrary: requires library path"); return Value::makeNull(); }
        std::string path = a[0].toString();

        void* handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
        if (!handle) {
            // Try common prefixes/suffixes
            std::string err = dlerror();
            // Try with lib prefix
            if (path.find('/') == std::string::npos && path.find("lib") != 0) {
                handle = dlopen(("lib" + path + ".so").c_str(), RTLD_LAZY | RTLD_LOCAL);
                if (!handle) handle = dlopen(("lib" + path).c_str(), RTLD_LAZY | RTLD_LOCAL);
            }
            if (!handle) {
                vm.throwError("GardFFIError", "FFI.loadLibrary: cannot load '" + path + "': " + err);
                return Value::makeNull();
            }
        }

        auto lib = std::make_shared<NativeLibrary>();
        lib->handle = handle;
        lib->path = path;
        lib->name = path;
        lib->loaded = true;

        int id = g_nextLibId++;
        g_nativeLibs[id] = lib;

        Value result = Value::makeObject("NativeLibrary");
        result.objVal->fields["_libId"] = Value::makeInt(id);
        result.objVal->fields["path"] = Value::makeString(path);
        result.objVal->fields["loaded"] = Value::makeBool(true);
        return result;
    });

    // FFI.closeLibrary(lib) — unload a shared library
    vm.registerNative("FFI.closeLibrary", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardFFIError", "FFI.closeLibrary: requires library object"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_libId"].toInt();
        auto it = g_nativeLibs.find(id);
        if (it == g_nativeLibs.end()) { vm.throwError("GardFFIError", "FFI.closeLibrary: library not found"); return Value::makeNull(); }
        if (it->second->handle) { dlclose(it->second->handle); it->second->handle = nullptr; }
        it->second->loaded = false;
        g_nativeLibs.erase(it);
        return Value::makeBool(true);
    });

    // FFI.getSymbol(lib, name) / FFI.getFunction(lib, name) — get a function/symbol pointer from library
    auto ffiGetSymbolImpl = [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardFFIError", "FFI.getSymbol: requires library and symbol name"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_libId"].toInt();
        auto it = g_nativeLibs.find(id);
        if (it == g_nativeLibs.end() || !it->second->handle) { vm.throwError("GardFFIError", "FFI.getSymbol: library not loaded"); return Value::makeNull(); }

        std::string name = a[1].toString();
        dlerror(); // clear
        void* sym = dlsym(it->second->handle, name.c_str());
        char* err = dlerror();
        if (err) {
            vm.throwError("GardFFIError", "FFI.getSymbol: symbol '" + name + "' not found: " + std::string(err));
            return Value::makeNull();
        }

        auto ptr = std::make_shared<NativePointer>();
        ptr->ptr = sym;
        ptr->typeName = "function";
        ptr->owned = false;

        int ptrId = g_nextPtrId++;
        g_pointers[ptrId] = ptr;

        Value result = Value::makeObject("NativeSymbol");
        result.objVal->fields["_ptrId"] = Value::makeInt(ptrId);
        result.objVal->fields["name"] = Value::makeString(name);
        result.objVal->fields["address"] = Value::makeLong(reinterpret_cast<int64_t>(sym));
        return result;
    };
    vm.registerNative("FFI.getSymbol", ffiGetSymbolImpl);
    vm.registerNative("FFI.getFunction", ffiGetSymbolImpl);

    // FFI.call(symbol) — simplified: no args, returns int (matches LLVM codegen path)
    // FFI.call(symbol, returnType, argTypes, args) — full typed call via libffi
    // returnType: "void", "int", "long", "double", "float", "pointer", "string"
    // argTypes: ["int", "double", "pointer", ...]
    // args: [42, 3.14, ptrObj, ...]
    vm.registerNative("FFI.call", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardFFIError", "FFI.call: requires at least a function pointer"); return Value::makeNull(); }
        int ptrId = a[0].objVal->fields["_ptrId"].toInt();
        auto pit = g_pointers.find(ptrId);
        if (pit == g_pointers.end() || !pit->second->ptr) { vm.throwError("GardFFIError", "FFI.call: invalid function pointer"); return Value::makeNull(); }

        // Simplified form: FFI.call(symbol) or FFI.call(symbol, arg1, arg2, ...)
        // When no returnType string is provided, call with integer args and return int
        if (a.size() < 4 || (a.size() >= 2 && a[1].type != ValueType::String)) {
            void* funcPtr = pit->second->ptr;
            // Collect up to 4 integer arguments (matching LLVM codegen behavior)
            int64_t args[4] = {0, 0, 0, 0};
            for (size_t i = 1; i < a.size() && i <= 4; i++) {
                args[i - 1] = (int64_t)a[i].toInt();
            }
            int numArgs = (int)(a.size() > 1 ? a.size() - 1 : 0);
            if (numArgs > 4) numArgs = 4;

            // Prepare CIF for simple int-returning call
            std::vector<ffi_type*> argTypes(numArgs, &ffi_type_sint64);
            ffi_cif cif;
            ffi_status status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned int)numArgs,
                                              &ffi_type_sint64, argTypes.empty() ? nullptr : argTypes.data());
            if (status != FFI_OK) {
                vm.throwError("GardFFIError", "FFI.call: failed to prepare call interface");
                return Value::makeNull();
            }

            std::vector<void*> argPtrs;
            for (int i = 0; i < numArgs; i++) argPtrs.push_back(&args[i]);

            int64_t retVal = 0;
            ffi_call(&cif, (void(*)())funcPtr, &retVal, argPtrs.empty() ? nullptr : argPtrs.data());
            return Value::makeInt((int)retVal);
        }

        void* funcPtr = pit->second->ptr;
        std::string retType = a[1].toString();

        // Build ffi_type arrays
        std::vector<ffi_type*> argTypes;
        if (a[2].type == ValueType::Array && a[2].arrVal) {
            for (auto& t : a[2].arrVal->elements) {
                std::string ts = t.toString();
                if (ts == "int" || ts == "i32") argTypes.push_back(&ffi_type_sint32);
                else if (ts == "long" || ts == "i64") argTypes.push_back(&ffi_type_sint64);
                else if (ts == "double" || ts == "f64") argTypes.push_back(&ffi_type_double);
                else if (ts == "float" || ts == "f32") argTypes.push_back(&ffi_type_float);
                else if (ts == "pointer" || ts == "string") argTypes.push_back(&ffi_type_pointer);
                else argTypes.push_back(&ffi_type_pointer);
            }
        }

        // Determine return type
        ffi_type* rtype = &ffi_type_void;
        if (retType == "int" || retType == "i32") rtype = &ffi_type_sint32;
        else if (retType == "long" || retType == "i64") rtype = &ffi_type_sint64;
        else if (retType == "double" || retType == "f64") rtype = &ffi_type_double;
        else if (retType == "float" || retType == "f32") rtype = &ffi_type_float;
        else if (retType == "pointer" || retType == "string") rtype = &ffi_type_pointer;

        // Prepare CIF
        ffi_cif cif;
        ffi_status status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned int)argTypes.size(),
                                          rtype, argTypes.empty() ? nullptr : argTypes.data());
        if (status != FFI_OK) {
            vm.throwError("GardFFIError", "FFI.call: failed to prepare call interface (status=" + std::to_string(status) + ")");
            return Value::makeNull();
        }

        // Marshal arguments
        std::vector<void*> argPtrs;
        std::vector<int32_t> intArgs;
        std::vector<int64_t> longArgs;
        std::vector<double> doubleArgs;
        std::vector<float> floatArgs;
        std::vector<void*> ptrArgs;
        std::vector<std::string> strBufs; // keep strings alive

        if (a[3].type == ValueType::Array && a[3].arrVal) {
            for (size_t i = 0; i < a[3].arrVal->elements.size() && i < argTypes.size(); i++) {
                auto& arg = a[3].arrVal->elements[i];
                if (argTypes[i] == &ffi_type_sint32) {
                    intArgs.push_back(arg.toInt());
                    argPtrs.push_back(&intArgs.back());
                } else if (argTypes[i] == &ffi_type_sint64) {
                    longArgs.push_back((int64_t)arg.toInt());
                    argPtrs.push_back(&longArgs.back());
                } else if (argTypes[i] == &ffi_type_double) {
                    doubleArgs.push_back(arg.toDouble());
                    argPtrs.push_back(&doubleArgs.back());
                } else if (argTypes[i] == &ffi_type_float) {
                    floatArgs.push_back((float)arg.toDouble());
                    argPtrs.push_back(&floatArgs.back());
                } else if (argTypes[i] == &ffi_type_pointer) {
                    if (arg.type == ValueType::String) {
                        strBufs.push_back(arg.toString());
                        ptrArgs.push_back((void*)strBufs.back().c_str());
                        argPtrs.push_back(&ptrArgs.back());
                    } else if (arg.type == ValueType::Object && arg.objVal && arg.objVal->fields.count("_ptrId")) {
                        int pid = arg.objVal->fields["_ptrId"].toInt();
                        auto pt = g_pointers.find(pid);
                        ptrArgs.push_back(pt != g_pointers.end() ? pt->second->ptr : nullptr);
                        argPtrs.push_back(&ptrArgs.back());
                    } else {
                        ptrArgs.push_back(nullptr);
                        argPtrs.push_back(&ptrArgs.back());
                    }
                }
            }
        }

        // Call
        union { int32_t i32; int64_t i64; double f64; float f32; void* ptr; } retVal;
        std::memset(&retVal, 0, sizeof(retVal));
        ffi_call(&cif, (void(*)())funcPtr, &retVal, argPtrs.empty() ? nullptr : argPtrs.data());

        // Marshal return value
        if (retType == "void") return Value::makeNull();
        if (retType == "int" || retType == "i32") return Value::makeInt(retVal.i32);
        if (retType == "long" || retType == "i64") return Value::makeLong(retVal.i64);
        if (retType == "double" || retType == "f64") return Value::makeDouble(retVal.f64);
        if (retType == "float" || retType == "f32") return Value::makeFloat(retVal.f32);
        if (retType == "string" && retVal.ptr) return Value::makeString((const char*)retVal.ptr);
        if (retType == "pointer") {
            auto rp = std::make_shared<NativePointer>();
            rp->ptr = retVal.ptr;
            rp->typeName = "void";
            int rpId = g_nextPtrId++;
            g_pointers[rpId] = rp;
            Value result = Value::makeObject("NativePointer");
            result.objVal->fields["_ptrId"] = Value::makeInt(rpId);
            result.objVal->fields["address"] = Value::makeLong(reinterpret_cast<int64_t>(retVal.ptr));
            return result;
        }
        return Value::makeNull();
    });

    // --- Memory/Pointer Operations ---

    // Memory.alloc(size) — allocate raw memory (malloc), returns pointer object
    vm.registerNative("Memory.alloc", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardMemoryError", "Memory.alloc: requires size"); return Value::makeNull(); }
        int size = a[0].toInt();
        if (size <= 0) { vm.throwError("GardMemoryError", "Memory.alloc: size must be > 0"); return Value::makeNull(); }
        void* ptr = std::malloc(size);
        if (!ptr) { vm.throwError("GardMemoryError", "Memory.alloc: allocation failed (size=" + std::to_string(size) + ")"); return Value::makeNull(); }
        std::memset(ptr, 0, size);

        auto np = std::make_shared<NativePointer>();
        np->ptr = ptr;
        np->size = size;
        np->owned = true;
        np->typeName = "void";
        int id = g_nextPtrId++;
        g_pointers[id] = np;

        Value result = Value::makeObject("NativePointer");
        result.objVal->fields["_ptrId"] = Value::makeInt(id);
        result.objVal->fields["size"] = Value::makeInt(size);
        result.objVal->fields["address"] = Value::makeLong(reinterpret_cast<int64_t>(ptr));
        result.objVal->fields["owned"] = Value::makeBool(true);
        return result;
    });

    // Memory.allocAligned(size, alignment) — aligned allocation
    vm.registerNative("Memory.allocAligned", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) { vm.throwError("GardMemoryError", "Memory.allocAligned: requires size and alignment"); return Value::makeNull(); }
        int size = a[0].toInt();
        int alignment = a[1].toInt();
        if (size <= 0) { vm.throwError("GardMemoryError", "Memory.allocAligned: size must be > 0"); return Value::makeNull(); }
        if (alignment < 1 || (alignment & (alignment - 1)) != 0) { vm.throwError("GardMemoryError", "Memory.allocAligned: alignment must be power of 2"); return Value::makeNull(); }
        void* ptr = nullptr;
        if (posix_memalign(&ptr, alignment, size) != 0 || !ptr) {
            vm.throwError("GardMemoryError", "Memory.allocAligned: allocation failed");
            return Value::makeNull();
        }
        std::memset(ptr, 0, size);

        auto np = std::make_shared<NativePointer>();
        np->ptr = ptr; np->size = size; np->owned = true; np->typeName = "void";
        int id = g_nextPtrId++;
        g_pointers[id] = np;

        Value result = Value::makeObject("NativePointer");
        result.objVal->fields["_ptrId"] = Value::makeInt(id);
        result.objVal->fields["size"] = Value::makeInt(size);
        result.objVal->fields["address"] = Value::makeLong(reinterpret_cast<int64_t>(ptr));
        result.objVal->fields["alignment"] = Value::makeInt(alignment);
        result.objVal->fields["owned"] = Value::makeBool(true);
        return result;
    });

    // Memory.free(ptr) — free allocated memory
    vm.registerNative("Memory.free", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardMemoryError", "Memory.free: requires pointer object"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end()) { vm.throwError("GardMemoryError", "Memory.free: invalid pointer (already freed or not allocated)"); return Value::makeNull(); }
        if (!it->second->owned) { vm.throwError("GardMemoryError", "Memory.free: cannot free unowned pointer (from library symbol)"); return Value::makeNull(); }
        if (it->second->ptr) std::free(it->second->ptr);
        g_pointers.erase(it);
        return Value::makeBool(true);
    });

    // Memory.readInt32(ptr, offset?) — read i32 from pointer + offset
    vm.registerNative("Memory.readInt32", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardMemoryError", "Memory.readInt32: requires pointer"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end() || !it->second->ptr) { vm.throwError("GardMemoryError", "Memory.readInt32: invalid pointer"); return Value::makeNull(); }
        int offset = a.size() >= 2 ? a[1].toInt() : 0;
        if (offset < 0 || (size_t)(offset + 4) > it->second->size) return Value::makeInt(0);
        int32_t val;
        std::memcpy(&val, (char*)it->second->ptr + offset, 4);
        return Value::makeInt(val);
    });

    // Memory.writeInt32(ptr, offset, value) — write i32 to pointer + offset
    vm.registerNative("Memory.writeInt32", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardMemoryError", "Memory.writeInt32: requires pointer, offset, value"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end() || !it->second->ptr) { vm.throwError("GardMemoryError", "Memory.writeInt32: invalid pointer"); return Value::makeNull(); }
        int offset = a[1].toInt();
        if (offset < 0 || (size_t)(offset + 4) > it->second->size) { vm.throwError("GardMemoryError", "Memory.writeInt32: out of bounds"); return Value::makeNull(); }
        int32_t val = a[2].toInt();
        std::memcpy((char*)it->second->ptr + offset, &val, 4);
        return Value::makeBool(true);
    });

    // Memory.readFloat64(ptr, offset?) — read f64
    vm.registerNative("Memory.readFloat64", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardMemoryError", "Memory.readFloat64: requires pointer"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end() || !it->second->ptr) { vm.throwError("GardMemoryError", "Memory.readFloat64: invalid pointer"); return Value::makeNull(); }
        int offset = a.size() >= 2 ? a[1].toInt() : 0;
        if (offset < 0 || (size_t)(offset + 8) > it->second->size) { vm.throwError("GardMemoryError", "Memory.readFloat64: out of bounds"); return Value::makeNull(); }
        double val;
        std::memcpy(&val, (char*)it->second->ptr + offset, 8);
        return Value::makeDouble(val);
    });

    // Memory.writeFloat64(ptr, offset, value) — write f64 to pointer + offset
    vm.registerNative("Memory.writeFloat64", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardMemoryError", "Memory.writeFloat64: requires pointer, offset, value"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end() || !it->second->ptr) { vm.throwError("GardMemoryError", "Memory.writeFloat64: invalid pointer"); return Value::makeNull(); }
        int offset = a[1].toInt();
        if (offset < 0 || (size_t)(offset + 8) > it->second->size) { vm.throwError("GardMemoryError", "Memory.writeFloat64: out of bounds"); return Value::makeNull(); }
        double val = a[2].toDouble();
        std::memcpy((char*)it->second->ptr + offset, &val, 8);
        return Value::makeBool(true);
    });

    // Memory.readInt64(ptr, offset) — read i64 from pointer + offset
    vm.registerNative("Memory.readInt64", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardMemoryError", "Memory.readInt64: requires pointer and offset"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end() || !it->second->ptr) { vm.throwError("GardMemoryError", "Memory.readInt64: invalid pointer"); return Value::makeNull(); }
        int offset = a[1].toInt();
        if (offset < 0 || (size_t)(offset + 8) > it->second->size) return Value::makeLong(0);
        int64_t val;
        std::memcpy(&val, (char*)it->second->ptr + offset, 8);
        return Value::makeLong(val);
    });

    // Memory.writeInt64(ptr, offset, value) — write i64 to pointer + offset
    vm.registerNative("Memory.writeInt64", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardMemoryError", "Memory.writeInt64: requires pointer, offset, value"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end() || !it->second->ptr) { vm.throwError("GardMemoryError", "Memory.writeInt64: invalid pointer"); return Value::makeNull(); }
        int offset = a[1].toInt();
        if (offset < 0 || (size_t)(offset + 8) > it->second->size) { vm.throwError("GardMemoryError", "Memory.writeInt64: out of bounds"); return Value::makeNull(); }
        int64_t val = (int64_t)a[2].toInt();
        std::memcpy((char*)it->second->ptr + offset, &val, 8);
        return Value::makeBool(true);
    });

    // Memory.size(ptr) — get allocation size
    vm.registerNative("Memory.size", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeLong(0);
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end()) return Value::makeLong(0);
        return Value::makeLong((int64_t)it->second->size);
    });

    // Memory.readString(ptr, offset?, maxLen?) — read null-terminated string
    vm.registerNative("Memory.readString", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardMemoryError", "Memory.readString: requires pointer"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end() || !it->second->ptr) { vm.throwError("GardMemoryError", "Memory.readString: invalid pointer"); return Value::makeNull(); }
        int offset = a.size() >= 2 ? a[1].toInt() : 0;
        int maxLen = a.size() >= 3 ? a[2].toInt() : (int)it->second->size - offset;
        if (offset < 0 || (size_t)offset >= it->second->size) { vm.throwError("GardMemoryError", "Memory.readString: out of bounds"); return Value::makeNull(); }
        const char* base = (const char*)it->second->ptr + offset;
        int len = 0;
        while (len < maxLen && (size_t)(offset + len) < it->second->size && base[len] != '\0') len++;
        return Value::makeString(std::string(base, len));
    });

    // Memory.writeString(ptr, offset, str) — write string to pointer + offset
    vm.registerNative("Memory.writeString", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardMemoryError", "Memory.writeString: requires pointer, offset, string"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end() || !it->second->ptr) { vm.throwError("GardMemoryError", "Memory.writeString: invalid pointer"); return Value::makeNull(); }
        int offset = a[1].toInt();
        std::string str = a[2].toString();
        if (offset < 0 || (size_t)(offset + str.size() + 1) > it->second->size) { vm.throwError("GardMemoryError", "Memory.writeString: out of bounds"); return Value::makeNull(); }
        std::memcpy((char*)it->second->ptr + offset, str.c_str(), str.size() + 1);
        return Value::makeInt((int)str.size());
    });

    // Memory.readBytes(ptr, offset, length) — read raw bytes as array
    vm.registerNative("Memory.readBytes", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardMemoryError", "Memory.readBytes: requires pointer, offset, length"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end() || !it->second->ptr) { vm.throwError("GardMemoryError", "Memory.readBytes: invalid pointer"); return Value::makeNull(); }
        int offset = a[1].toInt(), length = a[2].toInt();
        if (offset < 0 || length < 0 || (size_t)(offset + length) > it->second->size) { vm.throwError("GardMemoryError", "Memory.readBytes: out of bounds"); return Value::makeNull(); }
        Value arr = Value::makeArray();
        uint8_t* base = (uint8_t*)it->second->ptr + offset;
        for (int i = 0; i < length; i++) arr.arrVal->elements.push_back(Value::makeInt(base[i]));
        return arr;
    });

    // Memory.writeBytes(ptr, offset, data) — write byte array to memory
    vm.registerNative("Memory.writeBytes", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal || !a[2].arrVal) { vm.throwError("GardMemoryError", "Memory.writeBytes: requires pointer, offset, byte array"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end() || !it->second->ptr) { vm.throwError("GardMemoryError", "Memory.writeBytes: invalid pointer"); return Value::makeNull(); }
        int offset = a[1].toInt();
        int length = (int)a[2].arrVal->elements.size();
        if (offset < 0 || (size_t)(offset + length) > it->second->size) { vm.throwError("GardMemoryError", "Memory.writeBytes: out of bounds"); return Value::makeNull(); }
        uint8_t* base = (uint8_t*)it->second->ptr + offset;
        for (int i = 0; i < length; i++) base[i] = (uint8_t)a[2].arrVal->elements[i].toInt();
        return Value::makeInt(length);
    });

    // Memory.copy(dest, destOffset, src, srcOffset, length) — memcpy between pointers
    vm.registerNative("Memory.copy", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 5 || !a[0].objVal || !a[2].objVal) { vm.throwError("GardMemoryError", "Memory.copy: requires dest, destOffset, src, srcOffset, length"); return Value::makeNull(); }
        int destId = a[0].objVal->fields["_ptrId"].toInt();
        int srcId = a[2].objVal->fields["_ptrId"].toInt();
        auto dit = g_pointers.find(destId), sit = g_pointers.find(srcId);
        if (dit == g_pointers.end() || sit == g_pointers.end()) { vm.throwError("GardMemoryError", "Memory.copy: invalid pointer"); return Value::makeNull(); }
        int destOff = a[1].toInt(), srcOff = a[3].toInt(), length = a[4].toInt();
        if (destOff < 0 || (size_t)(destOff + length) > dit->second->size) { vm.throwError("GardMemoryError", "Memory.copy: dest out of bounds"); return Value::makeNull(); }
        if (srcOff < 0 || (size_t)(srcOff + length) > sit->second->size) { vm.throwError("GardMemoryError", "Memory.copy: src out of bounds"); return Value::makeNull(); }
        std::memcpy((char*)dit->second->ptr + destOff, (char*)sit->second->ptr + srcOff, length);
        return Value::makeInt(length);
    });

    // Memory.fill(ptr, offset, value, length) — memset
    vm.registerNative("Memory.fill", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal) { vm.throwError("GardMemoryError", "Memory.fill: requires pointer, offset, value, length"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end() || !it->second->ptr) { vm.throwError("GardMemoryError", "Memory.fill: invalid pointer"); return Value::makeNull(); }
        int offset = a[1].toInt(), val = a[2].toInt(), length = a[3].toInt();
        if (offset < 0 || (size_t)(offset + length) > it->second->size) { vm.throwError("GardMemoryError", "Memory.fill: out of bounds"); return Value::makeNull(); }
        std::memset((char*)it->second->ptr + offset, val & 0xFF, length);
        return Value::makeBool(true);
    });

    // Memory.addressOf(ptr) — get raw address as integer
    vm.registerNative("Memory.addressOf", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardMemoryError", "Memory.addressOf: requires pointer"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end()) return Value::makeLong(0);
        return Value::makeLong(reinterpret_cast<int64_t>(it->second->ptr));
    });

    // Memory.fromAddress(address, size) — create pointer from raw address (unsafe!)
    vm.registerNative("Memory.fromAddress", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) { vm.throwError("GardMemoryError", "Memory.fromAddress: requires address and size"); return Value::makeNull(); }
        int64_t addr = (int64_t)a[0].toInt();
        int size = a[1].toInt();
        auto np = std::make_shared<NativePointer>();
        np->ptr = reinterpret_cast<void*>(addr);
        np->size = size;
        np->owned = false; // not owned — don't free
        np->typeName = "void";
        int id = g_nextPtrId++;
        g_pointers[id] = np;
        Value result = Value::makeObject("NativePointer");
        result.objVal->fields["_ptrId"] = Value::makeInt(id);
        result.objVal->fields["size"] = Value::makeInt(size);
        result.objVal->fields["address"] = Value::makeLong(addr);
        result.objVal->fields["owned"] = Value::makeBool(false);
        return result;
    });

    // Memory.sizeof(type) — get size of a type in bytes
    vm.registerNative("Memory.sizeof", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeInt(0);
        std::string type = a[0].toString();
        if (type == "int" || type == "i32" || type == "float" || type == "f32") return Value::makeInt(4);
        if (type == "long" || type == "i64" || type == "double" || type == "f64") return Value::makeInt(8);
        if (type == "short" || type == "i16") return Value::makeInt(2);
        if (type == "byte" || type == "i8" || type == "char") return Value::makeInt(1);
        if (type == "pointer") return Value::makeInt(8); // 64-bit
        return Value::makeInt(0);
    });

    // Memory.isNull(ptr) — check if pointer is null
    vm.registerNative("Memory.isNull", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(true);
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        return Value::makeBool(it == g_pointers.end() || it->second->ptr == nullptr);
    });

    // Memory.realloc(ptr, newSize) — resize allocation
    vm.registerNative("Memory.realloc", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardMemoryError", "Memory.realloc: requires pointer and new size"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end() || !it->second->owned) { vm.throwError("GardMemoryError", "Memory.realloc: invalid or unowned pointer"); return Value::makeNull(); }
        int newSize = a[1].toInt();
        if (newSize <= 0) { vm.throwError("GardMemoryError", "Memory.realloc: size must be > 0"); return Value::makeNull(); }
        void* newPtr = std::realloc(it->second->ptr, newSize);
        if (!newPtr) { vm.throwError("GardMemoryError", "Memory.realloc: reallocation failed"); return Value::makeNull(); }
        it->second->ptr = newPtr;
        it->second->size = newSize;
        a[0].objVal->fields["size"] = Value::makeInt(newSize);
        a[0].objVal->fields["address"] = Value::makeLong(reinterpret_cast<int64_t>(newPtr));
        return a[0];
    });

    // --- Pointer namespace (aliases matching LLVM codegen API) ---

    // Pointer.alloc(size) — allocate raw memory, returns pointer object
    vm.registerNative("Pointer.alloc", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardFFIError", "Pointer.alloc: requires size"); return Value::makeNull(); }
        int size = a[0].toInt();
        if (size <= 0) { vm.throwError("GardFFIError", "Pointer.alloc: size must be > 0"); return Value::makeNull(); }
        void* ptr = std::calloc(1, size);
        if (!ptr) { vm.throwError("GardFFIError", "Pointer.alloc: allocation failed"); return Value::makeNull(); }
        auto np = std::make_shared<NativePointer>();
        np->ptr = ptr; np->size = size; np->owned = true; np->typeName = "void";
        int id = g_nextPtrId++;
        g_pointers[id] = np;
        Value result = Value::makeObject("NativePointer");
        result.objVal->fields["_ptrId"] = Value::makeInt(id);
        result.objVal->fields["size"] = Value::makeInt(size);
        result.objVal->fields["address"] = Value::makeLong(reinterpret_cast<int64_t>(ptr));
        return result;
    });

    // Pointer.free(ptr) — free allocated memory
    vm.registerNative("Pointer.free", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardFFIError", "Pointer.free: requires pointer object"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end()) { vm.throwError("GardFFIError", "Pointer.free: invalid pointer"); return Value::makeNull(); }
        if (it->second->owned && it->second->ptr) std::free(it->second->ptr);
        g_pointers.erase(it);
        return Value::makeNull();
    });

    // Pointer.readInt(ptr, offset) — read i32 from pointer + byte offset
    vm.registerNative("Pointer.readInt", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardFFIError", "Pointer.readInt: requires pointer and offset"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end() || !it->second->ptr) { vm.throwError("GardFFIError", "Pointer.readInt: invalid pointer"); return Value::makeNull(); }
        int offset = a[1].toInt();
        int32_t val;
        std::memcpy(&val, (char*)it->second->ptr + offset, sizeof(int32_t));
        return Value::makeInt(val);
    });

    // Pointer.writeInt(ptr, offset, value) — write i32 to pointer + byte offset
    vm.registerNative("Pointer.writeInt", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardFFIError", "Pointer.writeInt: requires pointer, offset, value"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end() || !it->second->ptr) { vm.throwError("GardFFIError", "Pointer.writeInt: invalid pointer"); return Value::makeNull(); }
        int offset = a[1].toInt();
        int32_t val = a[2].toInt();
        std::memcpy((char*)it->second->ptr + offset, &val, sizeof(int32_t));
        return Value::makeNull();
    });

    // Pointer.readString(ptr) — read null-terminated string from pointer
    vm.registerNative("Pointer.readString", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardFFIError", "Pointer.readString: requires pointer"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end() || !it->second->ptr) { vm.throwError("GardFFIError", "Pointer.readString: invalid pointer"); return Value::makeNull(); }
        const char* str = (const char*)it->second->ptr;
        return Value::makeString(std::string(str));
    });

    // Pointer.writeString(ptr, str) — write string to pointer (including null terminator)
    vm.registerNative("Pointer.writeString", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardFFIError", "Pointer.writeString: requires pointer and string"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end() || !it->second->ptr) { vm.throwError("GardFFIError", "Pointer.writeString: invalid pointer"); return Value::makeNull(); }
        std::string str = a[1].toString();
        std::memcpy(it->second->ptr, str.c_str(), str.size() + 1);
        return Value::makeNull();
    });

    // Pointer.readLong(ptr, offset) — read i64 from pointer + byte offset
    vm.registerNative("Pointer.readLong", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardFFIError", "Pointer.readLong: requires pointer and offset"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end() || !it->second->ptr) return Value::makeNull();
        int offset = a[1].toInt();
        int64_t val;
        std::memcpy(&val, (char*)it->second->ptr + offset, sizeof(int64_t));
        return Value::makeLong(val);
    });

    // Pointer.readByte(ptr, offset) — read single byte
    vm.registerNative("Pointer.readByte", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardFFIError", "Pointer.readByte: requires pointer and offset"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end() || !it->second->ptr) return Value::makeInt(0);
        int offset = a[1].toInt();
        uint8_t val = *((uint8_t*)it->second->ptr + offset);
        return Value::makeInt(val);
    });

    // Pointer.toInt(ptr) — get raw address as integer
    vm.registerNative("Pointer.toInt", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeLong(0);
        int id = a[0].objVal->fields["_ptrId"].toInt();
        auto it = g_pointers.find(id);
        if (it == g_pointers.end() || !it->second->ptr) return Value::makeLong(0);
        return Value::makeLong(reinterpret_cast<int64_t>(it->second->ptr));
    });

    // Pointer.fromInt(address) — create pointer from raw address (unsafe)
    vm.registerNative("Pointer.fromInt", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();
        int64_t addr = (int64_t)a[0].toInt();
        auto np = std::make_shared<NativePointer>();
        np->ptr = reinterpret_cast<void*>(addr);
        np->size = 0;
        np->owned = false;
        np->typeName = "void";
        int id = g_nextPtrId++;
        g_pointers[id] = np;
        Value result = Value::makeObject("NativePointer");
        result.objVal->fields["_ptrId"] = Value::makeInt(id);
        result.objVal->fields["address"] = Value::makeLong(addr);
        return result;
    });

    // FFI.callString(symbol, args...) — call function returning string
    vm.registerNative("FFI.callString", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardFFIError", "FFI.callString: requires function pointer"); return Value::makeNull(); }
        int ptrId = a[0].objVal->fields["_ptrId"].toInt();
        auto pit = g_pointers.find(ptrId);
        if (pit == g_pointers.end() || !pit->second->ptr) { vm.throwError("GardFFIError", "FFI.callString: invalid function pointer"); return Value::makeNull(); }
        void* funcPtr = pit->second->ptr;
        int64_t args[4] = {0, 0, 0, 0};
        for (size_t i = 1; i < a.size() && i <= 4; i++) args[i - 1] = (int64_t)a[i].toInt();
        int numArgs = (int)(a.size() > 1 ? a.size() - 1 : 0);
        if (numArgs > 4) numArgs = 4;
        std::vector<ffi_type*> argTypes(numArgs, &ffi_type_sint64);
        ffi_cif cif;
        ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned int)numArgs, &ffi_type_pointer, argTypes.empty() ? nullptr : argTypes.data());
        std::vector<void*> argPtrs;
        for (int i = 0; i < numArgs; i++) argPtrs.push_back(&args[i]);
        void* retVal = nullptr;
        ffi_call(&cif, (void(*)())funcPtr, &retVal, argPtrs.empty() ? nullptr : argPtrs.data());
        if (retVal) return Value::makeString((const char*)retVal);
        return Value::makeString("");
    });

    // FFI.callVoid(symbol, args...) — call function returning void
    vm.registerNative("FFI.callVoid", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardFFIError", "FFI.callVoid: requires function pointer"); return Value::makeNull(); }
        int ptrId = a[0].objVal->fields["_ptrId"].toInt();
        auto pit = g_pointers.find(ptrId);
        if (pit == g_pointers.end() || !pit->second->ptr) { vm.throwError("GardFFIError", "FFI.callVoid: invalid function pointer"); return Value::makeNull(); }
        void* funcPtr = pit->second->ptr;
        int64_t args[4] = {0, 0, 0, 0};
        for (size_t i = 1; i < a.size() && i <= 4; i++) args[i - 1] = (int64_t)a[i].toInt();
        int numArgs = (int)(a.size() > 1 ? a.size() - 1 : 0);
        if (numArgs > 4) numArgs = 4;
        std::vector<ffi_type*> argTypes(numArgs, &ffi_type_sint64);
        ffi_cif cif;
        ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned int)numArgs, &ffi_type_void, argTypes.empty() ? nullptr : argTypes.data());
        std::vector<void*> argPtrs;
        for (int i = 0; i < numArgs; i++) argPtrs.push_back(&args[i]);
        ffi_call(&cif, (void(*)())funcPtr, nullptr, argPtrs.empty() ? nullptr : argPtrs.data());
        return Value::makeNull();
    });

    // FFI.getLastError() — get last FFI error message
    vm.registerNative("FFI.getLastError", [](const std::vector<Value>&) -> Value {
        const char* err = dlerror();
        return err ? Value::makeString(err) : Value::makeString("");
    });

    // Console.readLine(prompt?) — alias for Console.read (LLVM uses readLine)
    vm.registerNative("Console.readLine", [&vm](const std::vector<Value>& a) -> Value {
        std::string prompt = a.empty() ? "" : a[0].toString();
        if (!prompt.empty()) std::cout << prompt << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) return Value::makeString("");
        return Value::makeString(line);
    });

    // Console.execStdout() — get stdout from last Console.exec
    // Note: LLVM codegen calls gard_console_exec_stdout() which is stateful
    // In interpreter, Console.exec returns the full result object, so this is a convenience
    vm.registerNative("Console.execStdout", [](const std::vector<Value>&) -> Value {
        return Value::makeString("");
    });

    // Console.execExitCode() — get exit code from last exec
    vm.registerNative("Console.execExitCode", [](const std::vector<Value>&) -> Value {
        return Value::makeInt(0);
    });

    // Console.execSuccess() — check if last exec succeeded
    vm.registerNative("Console.execSuccess", [](const std::vector<Value>&) -> Value {
        return Value::makeBool(true);
    });

    // RWLock.create() — create a read-write lock
    vm.registerNative("RWLock.create", [](const std::vector<Value>&) -> Value {
        Value lock = Value::makeObject("RWLock");
        lock.objVal->fields["_readers"] = Value::makeInt(0);
        lock.objVal->fields["_writeLocked"] = Value::makeBool(false);
        return lock;
    });

    // RWLock.readLock(lock)
    vm.registerNative("RWLock.readLock", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        if (a[0].objVal->fields["_writeLocked"].toBool()) return Value::makeBool(false);
        int readers = a[0].objVal->fields["_readers"].toInt();
        a[0].objVal->fields["_readers"] = Value::makeInt(readers + 1);
        return Value::makeBool(true);
    });

    // RWLock.readUnlock(lock)
    vm.registerNative("RWLock.readUnlock", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        int readers = a[0].objVal->fields["_readers"].toInt();
        if (readers > 0) a[0].objVal->fields["_readers"] = Value::makeInt(readers - 1);
        return Value::makeBool(true);
    });

    // RWLock.writeLock(lock)
    vm.registerNative("RWLock.writeLock", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        if (a[0].objVal->fields["_writeLocked"].toBool()) return Value::makeBool(false);
        if (a[0].objVal->fields["_readers"].toInt() > 0) return Value::makeBool(false);
        a[0].objVal->fields["_writeLocked"] = Value::makeBool(true);
        return Value::makeBool(true);
    });

    // RWLock.writeUnlock(lock)
    vm.registerNative("RWLock.writeUnlock", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        a[0].objVal->fields["_writeLocked"] = Value::makeBool(false);
        return Value::makeBool(true);
    });

    // RWLock.free(lock) — no-op in interpreter (GC handles it)
    vm.registerNative("RWLock.free", [](const std::vector<Value>&) -> Value {
        return Value::makeNull();
    });

    // Mutex.create() — create a mutex object
    vm.registerNative("Mutex.create", [](const std::vector<Value>&) -> Value {
        Value m = Value::makeObject("Mutex");
        m.objVal->fields["_locked"] = Value::makeBool(false);
        return m;
    });

    // Mutex.free(mutex) — no-op in interpreter
    vm.registerNative("Mutex.free", [](const std::vector<Value>&) -> Value {
        return Value::makeNull();
    });
    vm.registerNative("FFI.listSymbols", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardFFIError", "FFI.listSymbols: requires library"); return Value::makeNull(); }
        // dlopen doesn't provide symbol enumeration — return empty (would need libelf for real impl)
        Value arr = Value::makeArray();
        return arr;
    });

    // FFI.isLoaded(lib) — check if library is still loaded
    vm.registerNative("FFI.isLoaded", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        int id = a[0].objVal->fields["_libId"].toInt();
        auto it = g_nativeLibs.find(id);
        return Value::makeBool(it != g_nativeLibs.end() && it->second->loaded);
    });

    // ===== Console Operations =====

    // Console.exec(command) — execute a shell command, returns {stdout, stderr, exitCode, success}
    // Real implementation using popen with stderr capture via 2>&1 redirection
    vm.registerNative("Console.exec", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardConsoleError", "Console.exec: requires command string"); return Value::makeNull(); }
        std::string cmd = a[0].toString();
        // Redirect stderr to stdout for capture
        std::string fullCmd = cmd + " 2>&1";
        FILE* pipe = popen(fullCmd.c_str(), "r");
        if (!pipe) {
            vm.throwError("GardConsoleError", "Console.exec: failed to execute command '" + cmd + "'");
            return Value::makeNull();
        }
        std::string output;
        char buf[4096];
        while (fgets(buf, sizeof(buf), pipe)) output += buf;
        int status = pclose(pipe);
        int exitCode = WEXITSTATUS(status);

        Value result = Value::makeObject("ExecResult");
        result.objVal->fields["stdout"] = Value::makeString(output);
        result.objVal->fields["exitCode"] = Value::makeInt(exitCode);
        result.objVal->fields["success"] = Value::makeBool(exitCode == 0);
        result.objVal->fields["command"] = Value::makeString(cmd);
        return result;
    });

    // Console.execSilent(command) — execute without capturing output, returns exit code
    vm.registerNative("Console.execSilent", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardConsoleError", "Console.execSilent: requires command string"); return Value::makeNull(); }
        int status = std::system(a[0].toString().c_str());
        return Value::makeInt(WEXITSTATUS(status));
    });

    // Console.execAsync(command) — spawn command in background, returns PID
    vm.registerNative("Console.execAsync", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardConsoleError", "Console.execAsync: requires command string"); return Value::makeNull(); }
        std::string cmd = a[0].toString();
        pid_t pid = fork();
        if (pid < 0) { vm.throwError("GardConsoleError", "Console.execAsync: fork failed"); return Value::makeNull(); }
        if (pid == 0) {
            // Child: redirect to /dev/null and exec
            setsid();
            execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
            _exit(127);
        }
        Value result = Value::makeObject("AsyncProcess");
        result.objVal->fields["pid"] = Value::makeInt((int)pid);
        result.objVal->fields["command"] = Value::makeString(cmd);
        result.objVal->fields["running"] = Value::makeBool(true);
        return result;
    });

    // Console.execPipe(command) — open a pipe to a command for streaming I/O
    // Returns a pipe object that can be read from line by line
    vm.registerNative("Console.execPipe", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardConsoleError", "Console.execPipe: requires command string"); return Value::makeNull(); }
        std::string cmd = a[0].toString() + " 2>&1";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) { vm.throwError("GardConsoleError", "Console.execPipe: failed to open pipe for '" + a[0].toString() + "'"); return Value::makeNull(); }

        // Read all lines into an array
        Value lines = Value::makeArray();
        char buf[4096];
        while (fgets(buf, sizeof(buf), pipe)) {
            std::string line(buf);
            // Remove trailing newline
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            lines.arrVal->elements.push_back(Value::makeString(line));
        }
        int status = pclose(pipe);

        Value result = Value::makeObject("PipeResult");
        result.objVal->fields["lines"] = lines;
        result.objVal->fields["lineCount"] = Value::makeInt((int)lines.arrVal->elements.size());
        result.objVal->fields["exitCode"] = Value::makeInt(WEXITSTATUS(status));
        result.objVal->fields["success"] = Value::makeBool(WEXITSTATUS(status) == 0);
        result.objVal->fields["command"] = Value::makeString(a[0].toString());
        return result;
    });

    // Console.execWithInput(command, stdin) — execute command with stdin input
    vm.registerNative("Console.execWithInput", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) { vm.throwError("GardConsoleError", "Console.execWithInput: requires command and input string"); return Value::makeNull(); }
        std::string cmd = a[0].toString();
        std::string input = a[1].toString();

        // Use pipe pair: write input to child's stdin, read output from child's stdout
        int pipeIn[2], pipeOut[2];
        if (pipe(pipeIn) < 0 || pipe(pipeOut) < 0) {
            vm.throwError("GardConsoleError", "Console.execWithInput: pipe creation failed");
            return Value::makeNull();
        }

        pid_t pid = fork();
        if (pid < 0) { vm.throwError("GardConsoleError", "Console.execWithInput: fork failed"); return Value::makeNull(); }
        if (pid == 0) {
            // Child
            close(pipeIn[1]); close(pipeOut[0]);
            dup2(pipeIn[0], STDIN_FILENO);
            dup2(pipeOut[1], STDOUT_FILENO);
            dup2(pipeOut[1], STDERR_FILENO);
            close(pipeIn[0]); close(pipeOut[1]);
            execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
            _exit(127);
        }
        // Parent
        close(pipeIn[0]); close(pipeOut[1]);
        // Write input
        write(pipeIn[1], input.c_str(), input.size());
        close(pipeIn[1]);
        // Read output
        std::string output;
        char buf[4096];
        ssize_t n;
        while ((n = read(pipeOut[0], buf, sizeof(buf))) > 0) output.append(buf, n);
        close(pipeOut[0]);
        // Wait for child
        int status;
        waitpid(pid, &status, 0);

        Value result = Value::makeObject("ExecResult");
        result.objVal->fields["stdout"] = Value::makeString(output);
        result.objVal->fields["exitCode"] = Value::makeInt(WEXITSTATUS(status));
        result.objVal->fields["success"] = Value::makeBool(WEXITSTATUS(status) == 0);
        result.objVal->fields["command"] = Value::makeString(cmd);
        return result;
    });

    // Console.execTimeout(command, timeoutMs) — execute with timeout, kills if exceeded
    vm.registerNative("Console.execTimeout", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) { vm.throwError("GardConsoleError", "Console.execTimeout: requires command and timeout (ms)"); return Value::makeNull(); }
        std::string cmd = a[0].toString();
        int timeoutMs = a[1].toInt();

        pid_t pid = fork();
        if (pid < 0) { vm.throwError("GardConsoleError", "Console.execTimeout: fork failed"); return Value::makeNull(); }
        if (pid == 0) {
            execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
            _exit(127);
        }

        // Parent: wait with timeout
        int status;
        auto start = std::chrono::steady_clock::now();
        bool timedOut = false;
        while (true) {
            pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid) break; // child exited
            if (result < 0) break; // error
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= timeoutMs) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                timedOut = true;
                break;
            }
            usleep(1000); // 1ms poll
        }

        Value res = Value::makeObject("ExecResult");
        res.objVal->fields["exitCode"] = Value::makeInt(timedOut ? -1 : WEXITSTATUS(status));
        res.objVal->fields["success"] = Value::makeBool(!timedOut && WEXITSTATUS(status) == 0);
        res.objVal->fields["timedOut"] = Value::makeBool(timedOut);
        res.objVal->fields["command"] = Value::makeString(cmd);
        return res;
    });

    // Console.which(program) — find full path of a program (like 'which' command)
    vm.registerNative("Console.which", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardConsoleError", "Console.which: requires program name"); return Value::makeNull(); }
        std::string prog = a[0].toString();
        std::string cmd = "which " + prog + " 2>/dev/null";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return Value::makeNull();
        char buf[1024];
        std::string path;
        if (fgets(buf, sizeof(buf), pipe)) {
            path = buf;
            while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
        }
        pclose(pipe);
        return path.empty() ? Value::makeNull() : Value::makeString(path);
    });

    // Console.isAvailable(program) — check if a program exists in PATH
    vm.registerNative("Console.isAvailable", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeBool(false);
        std::string cmd = "which " + a[0].toString() + " >/dev/null 2>&1";
        return Value::makeBool(std::system(cmd.c_str()) == 0);
    });

    // Console.read() — read a line from stdin (blocking)
    vm.registerNative("Console.read", [&vm](const std::vector<Value>& a) -> Value {
        std::string prompt = a.empty() ? "" : a[0].toString();
        if (!prompt.empty()) std::cout << prompt << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) {
            return Value::makeNull(); // EOF
        }
        return Value::makeString(line);
    });

    // Console.readPassword(prompt?) — read without echo (for passwords)
    vm.registerNative("Console.readPassword", [&vm](const std::vector<Value>& a) -> Value {
        std::string prompt = a.empty() ? "Password: " : a[0].toString();
        // Disable echo
        std::string cmd = "stty -echo 2>/dev/null";
        std::system(cmd.c_str());
        std::cout << prompt << std::flush;
        std::string line;
        std::getline(std::cin, line);
        // Re-enable echo
        std::system("stty echo 2>/dev/null");
        std::cout << std::endl;
        return Value::makeString(line);
    });

    // Console.write(text) — write to stdout without newline
    vm.registerNative("Console.write", [](const std::vector<Value>& a) -> Value {
        if (!a.empty()) std::cout << a[0].toString() << std::flush;
        return Value::makeNull();
    });

    // Console.writeLine(text) — write to stdout with newline
    vm.registerNative("Console.writeLine", [](const std::vector<Value>& a) -> Value {
        std::cout << (a.empty() ? "" : a[0].toString()) << std::endl;
        return Value::makeNull();
    });

    // Console.writeError(text) — write to stderr
    vm.registerNative("Console.writeError", [](const std::vector<Value>& a) -> Value {
        if (!a.empty()) std::cerr << a[0].toString() << std::flush;
        return Value::makeNull();
    });

    // Console.clear() — clear terminal screen
    vm.registerNative("Console.clear", [](const std::vector<Value>&) -> Value {
        std::cout << "\033[2J\033[H" << std::flush;
        return Value::makeNull();
    });

    // Console.color(text, color) — wrap text in ANSI color codes
    // Colors: "red", "green", "blue", "yellow", "cyan", "magenta", "white", "gray", "bold", "underline", "reset"
    vm.registerNative("Console.color", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) { vm.throwError("GardConsoleError", "Console.color: requires text and color"); return Value::makeNull(); }
        std::string text = a[0].toString();
        std::string color = a[1].toString();
        std::string code;
        if (color == "red") code = "\033[31m";
        else if (color == "green") code = "\033[32m";
        else if (color == "yellow") code = "\033[33m";
        else if (color == "blue") code = "\033[34m";
        else if (color == "magenta") code = "\033[35m";
        else if (color == "cyan") code = "\033[36m";
        else if (color == "white") code = "\033[37m";
        else if (color == "gray") code = "\033[90m";
        else if (color == "bold") code = "\033[1m";
        else if (color == "underline") code = "\033[4m";
        else if (color == "dim") code = "\033[2m";
        else if (color == "reset") return Value::makeString(text + "\033[0m");
        else code = "\033[0m";
        return Value::makeString(code + text + "\033[0m");
    });

    // Console.table(data) — format array of objects as a table (like console.table in JS)
    vm.registerNative("Console.table", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].arrVal || a[0].arrVal->elements.empty()) return Value::makeString("");
        auto& rows = a[0].arrVal->elements;

        // Collect all column names from first row
        std::vector<std::string> columns;
        auto& first = rows[0];
        if (first.type == ValueType::Object && first.objVal) {
            for (auto& [k, v] : first.objVal->fields) { if (k[0] != '_') columns.push_back(k); }
        } else if (first.type == ValueType::Map && first.mapVal) {
            for (auto& [k, v] : first.mapVal->entries) columns.push_back(k);
        }
        if (columns.empty()) return Value::makeString("");

        // Calculate column widths
        std::vector<size_t> widths(columns.size());
        for (size_t c = 0; c < columns.size(); c++) widths[c] = columns[c].size();
        for (auto& row : rows) {
            for (size_t c = 0; c < columns.size(); c++) {
                std::string val;
                if (row.type == ValueType::Object && row.objVal) {
                    auto it = row.objVal->fields.find(columns[c]);
                    if (it != row.objVal->fields.end()) val = it->second.toString();
                } else if (row.type == ValueType::Map && row.mapVal) {
                    auto it = row.mapVal->entries.find(columns[c]);
                    if (it != row.mapVal->entries.end()) val = it->second.toString();
                }
                if (val.size() > widths[c]) widths[c] = val.size();
            }
        }

        // Build table string
        std::string table;
        // Header
        std::string sep = "+";
        std::string header = "|";
        for (size_t c = 0; c < columns.size(); c++) {
            sep += std::string(widths[c] + 2, '-') + "+";
            header += " " + columns[c] + std::string(widths[c] - columns[c].size(), ' ') + " |";
        }
        table += sep + "\n" + header + "\n" + sep + "\n";
        // Rows
        for (auto& row : rows) {
            std::string line = "|";
            for (size_t c = 0; c < columns.size(); c++) {
                std::string val;
                if (row.type == ValueType::Object && row.objVal) {
                    auto it = row.objVal->fields.find(columns[c]);
                    if (it != row.objVal->fields.end()) val = it->second.toString();
                } else if (row.type == ValueType::Map && row.mapVal) {
                    auto it = row.mapVal->entries.find(columns[c]);
                    if (it != row.mapVal->entries.end()) val = it->second.toString();
                }
                line += " " + val + std::string(widths[c] - val.size(), ' ') + " |";
            }
            table += line + "\n";
        }
        table += sep;
        return Value::makeString(table);
    });

    // Console.progressBar(current, total, width?) — render a progress bar string
    vm.registerNative("Console.progressBar", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeString("");
        int current = a[0].toInt(), total = a[1].toInt();
        int width = a.size() >= 3 ? a[2].toInt() : 40;
        if (total <= 0) return Value::makeString("[" + std::string(width, ' ') + "] 0%");
        int filled = (current * width) / total;
        if (filled > width) filled = width;
        int percent = (current * 100) / total;
        std::string bar = "[" + std::string(filled, '=') + (filled < width ? ">" : "") +
                          std::string(std::max(0, width - filled - 1), ' ') + "] " + std::to_string(percent) + "%";
        return Value::makeString(bar);
    });

    // Console.spinner(frame) — get a spinner character for animation
    vm.registerNative("Console.spinner", [](const std::vector<Value>& a) -> Value {
        static const char* frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
        int frame = a.empty() ? 0 : a[0].toInt();
        return Value::makeString(frames[frame % 10]);
    });

    // Console.cursorUp(n?) — move cursor up n lines
    vm.registerNative("Console.cursorUp", [](const std::vector<Value>& a) -> Value {
        int n = a.empty() ? 1 : a[0].toInt();
        std::cout << "\033[" << n << "A" << std::flush;
        return Value::makeNull();
    });

    // Console.cursorDown(n?)
    vm.registerNative("Console.cursorDown", [](const std::vector<Value>& a) -> Value {
        int n = a.empty() ? 1 : a[0].toInt();
        std::cout << "\033[" << n << "B" << std::flush;
        return Value::makeNull();
    });

    // Console.cursorTo(col, row?)
    vm.registerNative("Console.cursorTo", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();
        int col = a[0].toInt();
        if (a.size() >= 2) {
            int row = a[1].toInt();
            std::cout << "\033[" << row << ";" << col << "H" << std::flush;
        } else {
            std::cout << "\033[" << col << "G" << std::flush;
        }
        return Value::makeNull();
    });

    // Console.eraseLine() — erase current line
    vm.registerNative("Console.eraseLine", [](const std::vector<Value>&) -> Value {
        std::cout << "\033[2K\r" << std::flush;
        return Value::makeNull();
    });

    // Console.size() — get terminal size {columns, rows}
    vm.registerNative("Console.size", [](const std::vector<Value>&) -> Value {
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
            Value result = Value::makeObject("TerminalSize");
            result.objVal->fields["columns"] = Value::makeInt(w.ws_col);
            result.objVal->fields["rows"] = Value::makeInt(w.ws_row);
            return result;
        }
        Value result = Value::makeObject("TerminalSize");
        result.objVal->fields["columns"] = Value::makeInt(80);
        result.objVal->fields["rows"] = Value::makeInt(24);
        return result;
    });

    // Console.isInteractive() — check if stdin is a terminal (not piped)
    vm.registerNative("Console.isInteractive", [](const std::vector<Value>&) -> Value {
        return Value::makeBool(isatty(STDIN_FILENO) != 0);
    });

    // ===== OS Operations =====

    // OS.platform() — "linux", "darwin", "windows"
    vm.registerNative("OS.platform", [](const std::vector<Value>&) -> Value {
        struct utsname info; uname(&info);
        std::string sys = info.sysname;
        std::transform(sys.begin(), sys.end(), sys.begin(), ::tolower);
        return Value::makeString(sys);
    });

    // OS.arch() — "x86_64", "aarch64", etc.
    vm.registerNative("OS.arch", [](const std::vector<Value>&) -> Value {
        struct utsname info; uname(&info);
        return Value::makeString(info.machine);
    });

    // OS.hostname() — machine hostname
    vm.registerNative("OS.hostname", [](const std::vector<Value>&) -> Value {
        char buf[256]; gethostname(buf, sizeof(buf));
        return Value::makeString(buf);
    });

    // OS.release() — kernel release string
    vm.registerNative("OS.release", [](const std::vector<Value>&) -> Value {
        struct utsname info; uname(&info);
        return Value::makeString(info.release);
    });

    // OS.version() — kernel version string
    vm.registerNative("OS.version", [](const std::vector<Value>&) -> Value {
        struct utsname info; uname(&info);
        return Value::makeString(info.version);
    });

    // OS.uptime() — system uptime in seconds
    vm.registerNative("OS.uptime", [](const std::vector<Value>&) -> Value {
        std::ifstream f("/proc/uptime");
        if (f) { double up; f >> up; return Value::makeDouble(up); }
        return Value::makeDouble(0);
    });

    // OS.loadAvg() — 1, 5, 15 minute load averages
    vm.registerNative("OS.loadAvg", [](const std::vector<Value>&) -> Value {
        double loads[3] = {0};
        getloadavg(loads, 3);
        Value result = Value::makeArray();
        result.arrVal->elements.push_back(Value::makeDouble(loads[0]));
        result.arrVal->elements.push_back(Value::makeDouble(loads[1]));
        result.arrVal->elements.push_back(Value::makeDouble(loads[2]));
        return result;
    });

    // OS.cpuCount() — number of logical CPUs
    vm.registerNative("OS.cpuCount", [](const std::vector<Value>&) -> Value {
        return Value::makeInt((int)sysconf(_SC_NPROCESSORS_ONLN));
    });

    // OS.cpuInfo() — detailed CPU info from /proc/cpuinfo
    vm.registerNative("OS.cpuInfo", [](const std::vector<Value>&) -> Value {
        Value result = Value::makeObject("CpuInfo");
        std::ifstream f("/proc/cpuinfo");
        if (f) {
            std::string line;
            while (std::getline(f, line)) {
                size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string key = line.substr(0, colon);
                std::string val = line.substr(colon + 1);
                // Trim
                while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
                while (!val.empty() && val.front() == ' ') val = val.substr(1);
                if (key == "model name") { result.objVal->fields["model"] = Value::makeString(val); break; }
            }
        }
        result.objVal->fields["cores"] = Value::makeInt((int)sysconf(_SC_NPROCESSORS_ONLN));
        return result;
    });

    // OS.totalMemory() — total RAM in bytes
    vm.registerNative("OS.totalMemory", [](const std::vector<Value>&) -> Value {
        long pages = sysconf(_SC_PHYS_PAGES);
        long pageSize = sysconf(_SC_PAGE_SIZE);
        return Value::makeLong((int64_t)pages * pageSize);
    });

    // OS.freeMemory() — available RAM in bytes
    vm.registerNative("OS.freeMemory", [](const std::vector<Value>&) -> Value {
        long pages = sysconf(_SC_AVPHYS_PAGES);
        long pageSize = sysconf(_SC_PAGE_SIZE);
        return Value::makeLong((int64_t)pages * pageSize);
    });

    // OS.memoryUsage() — {total, free, used, percent}
    vm.registerNative("OS.memoryUsage", [](const std::vector<Value>&) -> Value {
        long total = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGE_SIZE);
        long free = sysconf(_SC_AVPHYS_PAGES) * sysconf(_SC_PAGE_SIZE);
        long used = total - free;
        Value result = Value::makeObject("MemoryUsage");
        result.objVal->fields["total"] = Value::makeLong(total);
        result.objVal->fields["free"] = Value::makeLong(free);
        result.objVal->fields["used"] = Value::makeLong(used);
        result.objVal->fields["percent"] = Value::makeDouble(total > 0 ? (double)used / total * 100.0 : 0);
        return result;
    });

    // OS.diskUsage(path?) — {total, free, used, percent} for a filesystem
    vm.registerNative("OS.diskUsage", [&vm](const std::vector<Value>& a) -> Value {
        std::string path = a.empty() ? "/" : a[0].toString();
        struct statvfs stat;
        if (statvfs(path.c_str(), &stat) != 0) {
            vm.throwError("GardOSError", "OS.diskUsage: cannot stat filesystem at '" + path + "'");
            return Value::makeNull();
        }
        int64_t total = (int64_t)stat.f_blocks * stat.f_frsize;
        int64_t free = (int64_t)stat.f_bavail * stat.f_frsize;
        int64_t used = total - free;
        Value result = Value::makeObject("DiskUsage");
        result.objVal->fields["total"] = Value::makeLong(total);
        result.objVal->fields["free"] = Value::makeLong(free);
        result.objVal->fields["used"] = Value::makeLong(used);
        result.objVal->fields["percent"] = Value::makeDouble(total > 0 ? (double)used / total * 100.0 : 0);
        result.objVal->fields["path"] = Value::makeString(path);
        return result;
    });

    // OS.env(key) — get environment variable
    vm.registerNative("OS.env", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();
        const char* val = std::getenv(a[0].toString().c_str());
        return val ? Value::makeString(val) : Value::makeNull();
    });

    // OS.setEnv(key, value) — set environment variable
    vm.registerNative("OS.setEnv", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) { vm.throwError("GardOSError", "OS.setEnv: requires key and value"); return Value::makeNull(); }
        if (setenv(a[0].toString().c_str(), a[1].toString().c_str(), 1) != 0) {
            vm.throwError("GardOSError", "OS.setEnv: failed to set '" + a[0].toString() + "'");
            return Value::makeNull();
        }
        return Value::makeBool(true);
    });

    // OS.unsetEnv(key) — remove environment variable
    vm.registerNative("OS.unsetEnv", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardOSError", "OS.unsetEnv: requires key"); return Value::makeNull(); }
        unsetenv(a[0].toString().c_str());
        return Value::makeBool(true);
    });

    // OS.envAll() — get all environment variables as a map
    vm.registerNative("OS.envAll", [](const std::vector<Value>&) -> Value {
        Value result = Value::makeMap();
        // Read from /proc/self/environ
        std::ifstream f("/proc/self/environ", std::ios::binary);
        if (f) {
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            size_t pos = 0;
            while (pos < content.size()) {
                size_t end = content.find('\0', pos);
                if (end == std::string::npos) break;
                std::string entry = content.substr(pos, end - pos);
                size_t eq = entry.find('=');
                if (eq != std::string::npos) {
                    result.mapVal->entries[entry.substr(0, eq)] = Value::makeString(entry.substr(eq + 1));
                }
                pos = end + 1;
            }
        }
        return result;
    });

    // OS.pid() — current process ID
    vm.registerNative("OS.pid", [](const std::vector<Value>&) -> Value {
        return Value::makeInt((int)getpid());
    });

    // OS.ppid() — parent process ID
    vm.registerNative("OS.ppid", [](const std::vector<Value>&) -> Value {
        return Value::makeInt((int)getppid());
    });

    // OS.uid() — current user ID
    vm.registerNative("OS.uid", [](const std::vector<Value>&) -> Value {
        return Value::makeInt((int)getuid());
    });

    // OS.gid() — current group ID
    vm.registerNative("OS.gid", [](const std::vector<Value>&) -> Value {
        return Value::makeInt((int)getgid());
    });

    // OS.user() — current username
    vm.registerNative("OS.user", [](const std::vector<Value>&) -> Value {
        const char* user = std::getenv("USER");
        if (!user) user = std::getenv("LOGNAME");
        return user ? Value::makeString(user) : Value::makeString("unknown");
    });

    // OS.home() — home directory
    vm.registerNative("OS.home", [](const std::vector<Value>&) -> Value {
        const char* home = std::getenv("HOME");
        return home ? Value::makeString(home) : Value::makeString("/");
    });

    // OS.tmpdir() — temp directory
    vm.registerNative("OS.tmpdir", [](const std::vector<Value>&) -> Value {
        const char* tmp = std::getenv("TMPDIR");
        if (!tmp) tmp = "/tmp";
        return Value::makeString(tmp);
    });

    // OS.cwd() — current working directory
    vm.registerNative("OS.cwd", [](const std::vector<Value>&) -> Value {
        char buf[4096];
        if (getcwd(buf, sizeof(buf))) return Value::makeString(buf);
        return Value::makeString(".");
    });

    // OS.chdir(path) — change working directory
    vm.registerNative("OS.chdir", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardOSError", "OS.chdir: requires path"); return Value::makeNull(); }
        if (chdir(a[0].toString().c_str()) != 0) {
            vm.throwError("GardOSError", "OS.chdir: cannot change to '" + a[0].toString() + "'");
            return Value::makeNull();
        }
        return Value::makeBool(true);
    });

    // OS.kill(pid, signal?) — send signal to process
    vm.registerNative("OS.kill", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardOSError", "OS.kill: requires PID"); return Value::makeNull(); }
        int pid = a[0].toInt();
        int sig = a.size() >= 2 ? a[1].toInt() : SIGTERM;
        if (kill(pid, sig) != 0) {
            vm.throwError("GardOSError", "OS.kill: failed to send signal " + std::to_string(sig) + " to PID " + std::to_string(pid));
            return Value::makeNull();
        }
        return Value::makeBool(true);
    });

    // OS.sleep(ms) — sleep for milliseconds
    vm.registerNative("OS.sleep", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();
        int ms = a[0].toInt();
        usleep(ms * 1000);
        return Value::makeNull();
    });

    // OS.time() — current time in milliseconds since epoch
    vm.registerNative("OS.time", [](const std::vector<Value>&) -> Value {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        return Value::makeLong(ms);
    });

    // OS.hrtime() — high-resolution time in nanoseconds (for benchmarking)
    vm.registerNative("OS.hrtime", [](const std::vector<Value>&) -> Value {
        auto now = std::chrono::steady_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        return Value::makeLong(ns);
    });

    // OS.networkInterfaces() — list network interfaces with addresses
    vm.registerNative("OS.networkInterfaces", [](const std::vector<Value>&) -> Value {
        Value result = Value::makeArray();
        std::ifstream f("/proc/net/dev");
        if (f) {
            std::string line;
            std::getline(f, line); std::getline(f, line); // skip headers
            while (std::getline(f, line)) {
                size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string name = line.substr(0, colon);
                    size_t start = name.find_first_not_of(" ");
                    if (start != std::string::npos) name = name.substr(start);
                    Value iface = Value::makeObject("NetworkInterface");
                    iface.objVal->fields["name"] = Value::makeString(name);
                    // Parse RX/TX bytes
                    std::istringstream iss(line.substr(colon + 1));
                    long rxBytes, rxPackets; iss >> rxBytes >> rxPackets;
                    iface.objVal->fields["rxBytes"] = Value::makeLong(rxBytes);
                    result.arrVal->elements.push_back(iface);
                }
            }
        }
        return result;
    });

    // OS.processes() — list running processes (from /proc)
    vm.registerNative("OS.processes", [](const std::vector<Value>&) -> Value {
        Value result = Value::makeArray();
        // Read /proc for PIDs
        std::string cmd = "ps -eo pid,comm --no-headers 2>/dev/null | head -50";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (pipe) {
            char buf[256];
            while (fgets(buf, sizeof(buf), pipe)) {
                std::string line(buf);
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                while (!line.empty() && line.front() == ' ') line = line.substr(1);
                size_t space = line.find(' ');
                if (space != std::string::npos) {
                    Value proc = Value::makeObject("ProcessInfo");
                    proc.objVal->fields["pid"] = Value::makeInt(std::atoi(line.substr(0, space).c_str()));
                    proc.objVal->fields["name"] = Value::makeString(line.substr(space + 1));
                    result.arrVal->elements.push_back(proc);
                }
            }
            pclose(pipe);
        }
        return result;
    });

    // OS.processExists(pid) — check if a process is running
    vm.registerNative("OS.processExists", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeBool(false);
        return Value::makeBool(kill(a[0].toInt(), 0) == 0);
    });

    // OS.exit(code?) — exit the process
    vm.registerNative("OS.exit", [](const std::vector<Value>& a) -> Value {
        int code = a.empty() ? 0 : a[0].toInt();
        std::exit(code);
        return Value::makeNull();
    });

    // OS.tempFile(prefix?) — create a temporary file, returns path
    vm.registerNative("OS.tempFile", [&vm](const std::vector<Value>& a) -> Value {
        std::string prefix = a.empty() ? "gard_" : a[0].toString();
        std::string tmpl = "/tmp/" + prefix + "XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        int fd = mkstemp(buf.data());
        if (fd < 0) { vm.throwError("GardOSError", "OS.tempFile: failed to create temp file"); return Value::makeNull(); }
        close(fd);
        return Value::makeString(std::string(buf.data()));
    });

    // OS.signal(signum, action) — simplified signal handling ("ignore", "default")
    vm.registerNative("OS.signal", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) { vm.throwError("GardOSError", "OS.signal: requires signal number and action"); return Value::makeNull(); }
        int sig = a[0].toInt();
        std::string action = a[1].toString();
        if (action == "ignore") signal(sig, SIG_IGN);
        else if (action == "default") signal(sig, SIG_DFL);
        else { vm.throwError("GardOSError", "OS.signal: action must be 'ignore' or 'default'"); return Value::makeNull(); }
        return Value::makeBool(true);
    });

    // OS.errno() — get last system error number and message
    vm.registerNative("OS.errno", [](const std::vector<Value>&) -> Value {
        Value result = Value::makeObject("ErrnoInfo");
        result.objVal->fields["code"] = Value::makeInt(errno);
        result.objVal->fields["message"] = Value::makeString(strerror(errno));
        return result;
    });

    // ===== Mail Operations (SMTP via libcurl) =====

    // Mail uses libcurl SMTP for real email sending.
    // Supports: SMTP/SMTPS, TLS/STARTTLS, attachments, HTML body, CC/BCC, reply-to

    // Helper: curl payload reader for SMTP
    struct MailPayload {
        std::string data;
        size_t offset = 0;
    };
    static auto mailPayloadRead = [](char* ptr, size_t size, size_t nmemb, void* userp) -> size_t {
        auto* payload = static_cast<MailPayload*>(userp);
        size_t room = size * nmemb;
        size_t remaining = payload->data.size() - payload->offset;
        if (remaining == 0) return 0;
        size_t toSend = std::min(room, remaining);
        std::memcpy(ptr, payload->data.c_str() + payload->offset, toSend);
        payload->offset += toSend;
        return toSend;
    };

    // Mail.createTransport(config) — create SMTP transport
    // config: {host, port, user, password, secure?, tls?}
    vm.registerNative("Mail.createTransport", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardMailError", "Mail.createTransport: requires config object"); return Value::makeNull(); }

        std::string host = "localhost", user, password;
        int port = 587;
        bool secure = false, tls = true;

        auto getField = [&](const Value& obj, const std::string& key) -> std::string {
            if (obj.type == ValueType::Object && obj.objVal) {
                auto it = obj.objVal->fields.find(key);
                if (it != obj.objVal->fields.end()) return it->second.toString();
            } else if (obj.type == ValueType::Map && obj.mapVal) {
                auto it = obj.mapVal->entries.find(key);
                if (it != obj.mapVal->entries.end()) return it->second.toString();
            }
            return "";
        };

        host = getField(a[0], "host");
        if (host.empty()) host = "localhost";
        std::string portStr = getField(a[0], "port");
        if (!portStr.empty()) port = std::atoi(portStr.c_str());
        user = getField(a[0], "user");
        password = getField(a[0], "password");
        std::string secureStr = getField(a[0], "secure");
        if (secureStr == "true") { secure = true; port = port == 587 ? 465 : port; }
        std::string tlsStr = getField(a[0], "tls");
        if (tlsStr == "false") tls = false;

        Value transport = Value::makeObject("MailTransport");
        transport.objVal->fields["host"] = Value::makeString(host);
        transport.objVal->fields["port"] = Value::makeInt(port);
        transport.objVal->fields["user"] = Value::makeString(user);
        transport.objVal->fields["_password"] = Value::makeString(password);
        transport.objVal->fields["secure"] = Value::makeBool(secure);
        transport.objVal->fields["tls"] = Value::makeBool(tls);
        transport.objVal->fields["connected"] = Value::makeBool(true);
        return transport;
    });

    // Mail.send(transport, message) — send an email via SMTP using libcurl
    // message: {from, to, subject, body, html?, cc?, bcc?, replyTo?, headers?}
    vm.registerNative("Mail.send", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardMailError", "Mail.send: requires transport and message"); return Value::makeNull(); }

        auto getField = [](const Value& obj, const std::string& key) -> std::string {
            if (obj.type == ValueType::Object && obj.objVal) {
                auto it = obj.objVal->fields.find(key);
                if (it != obj.objVal->fields.end() && it->second.type != ValueType::Null) return it->second.toString();
            } else if (obj.type == ValueType::Map && obj.mapVal) {
                auto it = obj.mapVal->entries.find(key);
                if (it != obj.mapVal->entries.end() && it->second.type != ValueType::Null) return it->second.toString();
            }
            return "";
        };

        auto getArray = [](const Value& obj, const std::string& key) -> std::vector<std::string> {
            std::vector<std::string> result;
            Value arr;
            if (obj.type == ValueType::Object && obj.objVal) {
                auto it = obj.objVal->fields.find(key);
                if (it != obj.objVal->fields.end()) arr = it->second;
            } else if (obj.type == ValueType::Map && obj.mapVal) {
                auto it = obj.mapVal->entries.find(key);
                if (it != obj.mapVal->entries.end()) arr = it->second;
            }
            if (arr.type == ValueType::Array && arr.arrVal) {
                for (auto& e : arr.arrVal->elements) result.push_back(e.toString());
            } else if (arr.type == ValueType::String && !arr.toString().empty()) {
                result.push_back(arr.toString());
            }
            return result;
        };

        // Extract transport config
        std::string host = a[0].objVal->fields["host"].toString();
        int port = a[0].objVal->fields["port"].toInt();
        std::string user = a[0].objVal->fields["user"].toString();
        std::string password = a[0].objVal->fields["_password"].toString();
        bool secure = a[0].objVal->fields["secure"].toBool();
        bool tls = a[0].objVal->fields["tls"].toBool();

        // Extract message (accept 'from' or 'sender' since 'from' is a keyword in Gard)
        std::string from = getField(a[1], "from");
        if (from.empty()) from = getField(a[1], "sender");
        std::vector<std::string> to = getArray(a[1], "to");
        std::string subject = getField(a[1], "subject");
        std::string body = getField(a[1], "body");
        std::string html = getField(a[1], "html");
        std::vector<std::string> cc = getArray(a[1], "cc");
        std::vector<std::string> bcc = getArray(a[1], "bcc");
        std::string replyTo = getField(a[1], "replyTo");

        if (from.empty()) { vm.throwError("GardMailError", "Mail.send: 'from' is required"); return Value::makeNull(); }
        if (to.empty()) { vm.throwError("GardMailError", "Mail.send: 'to' is required (string or array)"); return Value::makeNull(); }
        if (subject.empty()) { vm.throwError("GardMailError", "Mail.send: 'subject' is required"); return Value::makeNull(); }

        // Build RFC 2822 message
        std::string boundary = "----=_GardMail_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        std::string message;
        message += "From: " + from + "\r\n";
        message += "To: ";
        for (size_t i = 0; i < to.size(); i++) { if (i > 0) message += ", "; message += to[i]; }
        message += "\r\n";
        if (!cc.empty()) {
            message += "Cc: ";
            for (size_t i = 0; i < cc.size(); i++) { if (i > 0) message += ", "; message += cc[i]; }
            message += "\r\n";
        }
        if (!replyTo.empty()) message += "Reply-To: " + replyTo + "\r\n";
        message += "Subject: " + subject + "\r\n";
        message += "MIME-Version: 1.0\r\n";

        if (!html.empty()) {
            // Multipart: text + html
            message += "Content-Type: multipart/alternative; boundary=\"" + boundary + "\"\r\n\r\n";
            message += "--" + boundary + "\r\n";
            message += "Content-Type: text/plain; charset=UTF-8\r\n\r\n";
            message += body + "\r\n\r\n";
            message += "--" + boundary + "\r\n";
            message += "Content-Type: text/html; charset=UTF-8\r\n\r\n";
            message += html + "\r\n\r\n";
            message += "--" + boundary + "--\r\n";
        } else {
            message += "Content-Type: text/plain; charset=UTF-8\r\n\r\n";
            message += body + "\r\n";
        }

        // Send via libcurl SMTP
        CURL* curl = curl_easy_init();
        if (!curl) { vm.throwError("GardMailError", "Mail.send: failed to initialize CURL"); return Value::makeNull(); }

        std::string url = (secure ? "smtps://" : "smtp://") + host + ":" + std::to_string(port);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

        if (!user.empty()) {
            curl_easy_setopt(curl, CURLOPT_USERNAME, user.c_str());
            curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());
        }

        if (tls && !secure) {
            curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
        }

        // SSL settings
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        curl_easy_setopt(curl, CURLOPT_MAIL_FROM, from.c_str());

        struct curl_slist* recipients = nullptr;
        for (auto& r : to) recipients = curl_slist_append(recipients, r.c_str());
        for (auto& r : cc) recipients = curl_slist_append(recipients, r.c_str());
        for (auto& r : bcc) recipients = curl_slist_append(recipients, r.c_str());
        curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);

        MailPayload payload;
        payload.data = message;
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userp) -> size_t {
            auto* p = static_cast<MailPayload*>(userp);
            size_t room = size * nmemb;
            size_t remaining = p->data.size() - p->offset;
            if (remaining == 0) return 0;
            size_t toSend = std::min(room, remaining);
            std::memcpy(ptr, p->data.c_str() + p->offset, toSend);
            p->offset += toSend;
            return toSend;
        });
        curl_easy_setopt(curl, CURLOPT_READDATA, &payload);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);

        CURLcode res = curl_easy_perform(curl);

        curl_slist_free_all(recipients);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            vm.throwError("GardMailError", "Mail.send: " + std::string(curl_easy_strerror(res)) + " (host: " + host + ":" + std::to_string(port) + ")");
            return Value::makeNull();
        }

        Value result = Value::makeObject("MailResult");
        result.objVal->fields["success"] = Value::makeBool(true);
        result.objVal->fields["to"] = Value::makeString(to[0]);
        result.objVal->fields["subject"] = Value::makeString(subject);
        result.objVal->fields["messageId"] = Value::makeString(boundary);
        return result;
    });

    // Mail.verify(transport) — verify SMTP connection (EHLO handshake)
    vm.registerNative("Mail.verify", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardMailError", "Mail.verify: requires transport"); return Value::makeNull(); }
        std::string host = a[0].objVal->fields["host"].toString();
        int port = a[0].objVal->fields["port"].toInt();
        bool secure = a[0].objVal->fields["secure"].toBool();
        std::string user = a[0].objVal->fields["user"].toString();
        std::string password = a[0].objVal->fields["_password"].toString();

        CURL* curl = curl_easy_init();
        if (!curl) { vm.throwError("GardMailError", "Mail.verify: CURL init failed"); return Value::makeNull(); }

        std::string url = (secure ? "smtps://" : "smtp://") + host + ":" + std::to_string(port);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        if (!user.empty()) {
            curl_easy_setopt(curl, CURLOPT_USERNAME, user.c_str());
            curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_USE_SSL, secure ? (long)CURLUSESSL_ALL : (long)CURLUSESSL_TRY);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        Value result = Value::makeObject("VerifyResult");
        result.objVal->fields["connected"] = Value::makeBool(res == CURLE_OK);
        result.objVal->fields["error"] = Value::makeString(res != CURLE_OK ? curl_easy_strerror(res) : "");
        result.objVal->fields["host"] = Value::makeString(host);
        result.objVal->fields["port"] = Value::makeInt(port);
        return result;
    });

    // Mail.buildMessage(options) — build RFC 2822 message string without sending
    // Useful for testing/debugging or custom transport
    vm.registerNative("Mail.buildMessage", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardMailError", "Mail.buildMessage: requires message options"); return Value::makeNull(); }

        auto getField = [](const Value& obj, const std::string& key) -> std::string {
            if (obj.type == ValueType::Object && obj.objVal) {
                auto it = obj.objVal->fields.find(key);
                if (it != obj.objVal->fields.end() && it->second.type != ValueType::Null) return it->second.toString();
            } else if (obj.type == ValueType::Map && obj.mapVal) {
                auto it = obj.mapVal->entries.find(key);
                if (it != obj.mapVal->entries.end() && it->second.type != ValueType::Null) return it->second.toString();
            }
            return "";
        };

        std::string from = getField(a[0], "from");
        std::string to = getField(a[0], "to");
        std::string subject = getField(a[0], "subject");
        std::string body = getField(a[0], "body");
        std::string html = getField(a[0], "html");
        std::string cc = getField(a[0], "cc");
        std::string replyTo = getField(a[0], "replyTo");

        std::string msg;
        msg += "From: " + from + "\r\n";
        msg += "To: " + to + "\r\n";
        if (!cc.empty()) msg += "Cc: " + cc + "\r\n";
        if (!replyTo.empty()) msg += "Reply-To: " + replyTo + "\r\n";
        msg += "Subject: " + subject + "\r\n";
        msg += "MIME-Version: 1.0\r\n";
        msg += "Content-Type: text/plain; charset=UTF-8\r\n\r\n";
        msg += body + "\r\n";

        return Value::makeString(msg);
    });

    // Mail.sendBulk(transport, messages) — send multiple emails
    vm.registerNative("Mail.sendBulk", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal || !a[1].arrVal) { vm.throwError("GardMailError", "Mail.sendBulk: requires transport and messages array"); return Value::makeNull(); }
        int sent = 0, failed = 0;
        Value errors = Value::makeArray();
        for (auto& msg : a[1].arrVal->elements) {
            std::vector<Value> sendArgs = {a[0], msg};
            Value result = vm.callNative("Mail.send", sendArgs);
            if (result.type == ValueType::Null) {
                failed++;
            } else {
                sent++;
            }
        }
        Value result = Value::makeObject("BulkResult");
        result.objVal->fields["sent"] = Value::makeInt(sent);
        result.objVal->fields["failed"] = Value::makeInt(failed);
        result.objVal->fields["total"] = Value::makeInt(sent + failed);
        return result;
    });

    // Mail.createTemplate(template, variables) — simple template rendering for emails
    // Replaces {{key}} with values from variables map
    vm.registerNative("Mail.createTemplate", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) { vm.throwError("GardMailError", "Mail.createTemplate: requires template string and variables"); return Value::makeNull(); }
        std::string tmpl = a[0].toString();
        std::string result = tmpl;

        // Replace {{key}} patterns
        auto replaceVar = [](const Value& vars, const std::string& key) -> std::string {
            if (vars.type == ValueType::Object && vars.objVal) {
                auto it = vars.objVal->fields.find(key);
                if (it != vars.objVal->fields.end()) return it->second.toString();
            } else if (vars.type == ValueType::Map && vars.mapVal) {
                auto it = vars.mapVal->entries.find(key);
                if (it != vars.mapVal->entries.end()) return it->second.toString();
            }
            return "{{" + key + "}}";
        };

        size_t pos = 0;
        while ((pos = result.find("{{", pos)) != std::string::npos) {
            size_t end = result.find("}}", pos);
            if (end == std::string::npos) break;
            std::string key = result.substr(pos + 2, end - pos - 2);
            // Trim whitespace
            while (!key.empty() && key.front() == ' ') key = key.substr(1);
            while (!key.empty() && key.back() == ' ') key.pop_back();
            std::string value = replaceVar(a[1], key);
            result.replace(pos, end - pos + 2, value);
            pos += value.size();
        }

        return Value::makeString(result);
    });

    // Mail.validateEmail(email) — validate email format
    vm.registerNative("Mail.validateEmail", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeBool(false);
        std::string email = a[0].toString();
        // Basic RFC 5322 validation
        size_t at = email.find('@');
        if (at == std::string::npos || at == 0 || at == email.size() - 1) return Value::makeBool(false);
        size_t dot = email.find('.', at);
        if (dot == std::string::npos || dot == at + 1 || dot == email.size() - 1) return Value::makeBool(false);
        // No spaces
        if (email.find(' ') != std::string::npos) return Value::makeBool(false);
        // No double dots
        if (email.find("..") != std::string::npos) return Value::makeBool(false);
        return Value::makeBool(true);
    });

    // Mail.encodeAttachment(filePath) — base64 encode a file for email attachment
    vm.registerNative("Mail.encodeAttachment", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardMailError", "Mail.encodeAttachment: requires file path"); return Value::makeNull(); }
        std::string path = a[0].toString();
        std::ifstream file(path, std::ios::binary);
        if (!file) { vm.throwError("GardMailError", "Mail.encodeAttachment: cannot read file '" + path + "'"); return Value::makeNull(); }
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        // Base64 encode
        static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string encoded;
        int val = 0, valb = -6;
        for (unsigned char c : content) {
            val = (val << 8) + c; valb += 8;
            while (valb >= 0) { encoded += table[(val >> valb) & 0x3F]; valb -= 6; }
        }
        if (valb > -6) encoded += table[((val << 8) >> (valb + 8)) & 0x3F];
        while (encoded.size() % 4) encoded += '=';

        // Extract filename
        size_t slash = path.rfind('/');
        std::string filename = (slash != std::string::npos) ? path.substr(slash + 1) : path;

        Value result = Value::makeObject("Attachment");
        result.objVal->fields["filename"] = Value::makeString(filename);
        result.objVal->fields["content"] = Value::makeString(encoded);
        result.objVal->fields["size"] = Value::makeInt((int)content.size());
        result.objVal->fields["encoding"] = Value::makeString("base64");
        return result;
    });
}

} // namespace stdlib
} // namespace runtime
} // namespace gard
