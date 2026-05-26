#pragma once

#include "runtime/runtime.h"
#include "bytecode/bytecode.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <chrono>
#include <atomic>

namespace gard {
namespace runtime {

// --- Sandboxed Execution ---

struct SandboxLimits {
    size_t maxInstructions = 1000000;   // max instructions before halt
    size_t maxMemoryBytes = 64 * 1024 * 1024; // 64MB
    size_t maxStackDepth = 512;
    size_t maxCallDepth = 256;
    std::chrono::milliseconds timeout{5000}; // 5 second timeout
    bool allowFileIO = false;
    bool allowNetworkIO = false;
    bool allowNativeCall = false;
};

struct SandboxResult {
    int exitCode = 0;
    bool timedOut = false;
    bool memoryExceeded = false;
    bool instructionLimitHit = false;
    size_t instructionsExecuted = 0;
    size_t peakMemory = 0;
    std::chrono::milliseconds elapsed{0};
    std::string error;
};

class SandboxedVM {
public:
    SandboxedVM(const SandboxLimits& limits = SandboxLimits());

    SandboxResult run(const bytecode::BytecodeModule& module);

private:
    SandboxLimits limits_;
};

// --- Hot Reload ---

class HotReloader {
public:
    HotReloader(VM& vm);

    // Replace a function's bytecode at runtime
    bool reloadFunction(const std::string& name, const bytecode::BytecodeModule& newModule);

    // Replace entire module (preserving state where possible)
    bool reloadModule(const bytecode::BytecodeModule& newModule);

    // Check if a reload is pending
    bool hasPendingReload() const { return pendingReload_; }

    // Get reload count
    int reloadCount() const { return reloadCount_; }

private:
    VM& vm_;
    std::atomic<bool> pendingReload_{false};
    int reloadCount_ = 0;
};

// --- JIT Compilation Trigger ---

struct JITProfile {
    std::string functionName;
    uint64_t callCount = 0;
    uint64_t instructionCount = 0;
    bool isHot = false;
    bool isCompiled = false;
};

class JITTrigger {
public:
    JITTrigger(int hotThreshold = 100);

    // Record a function call (returns true if function became hot)
    bool recordCall(const std::string& funcName);

    // Record instructions executed in a function
    void recordInstructions(const std::string& funcName, uint64_t count);

    // Check if a function is hot
    bool isHot(const std::string& funcName) const;

    // Get all hot functions
    std::vector<std::string> getHotFunctions() const;

    // Mark a function as JIT-compiled
    void markCompiled(const std::string& funcName);

    // Get profile for a function
    const JITProfile* getProfile(const std::string& funcName) const;

private:
    int hotThreshold_;
    std::unordered_map<std::string, JITProfile> profiles_;
};

// --- Plugin/Extension API ---

// Plugin interface
class VMPlugin {
public:
    virtual ~VMPlugin() = default;

    virtual std::string name() const = 0;
    virtual std::string version() const = 0;

    // Called when plugin is loaded
    virtual bool onLoad(VM& vm) = 0;

    // Called when plugin is unloaded
    virtual void onUnload() = 0;

    // Register native functions provided by this plugin
    virtual std::vector<std::string> providedFunctions() const = 0;
};

// Plugin manager
class PluginManager {
public:
    PluginManager(VM& vm);

    // Load a plugin
    bool loadPlugin(std::unique_ptr<VMPlugin> plugin);

    // Unload a plugin by name
    bool unloadPlugin(const std::string& name);

    // Get loaded plugin names
    std::vector<std::string> loadedPlugins() const;

    // Check if a plugin is loaded
    bool isLoaded(const std::string& name) const;

    // Get plugin count
    size_t pluginCount() const { return plugins_.size(); }

private:
    VM& vm_;
    std::unordered_map<std::string, std::unique_ptr<VMPlugin>> plugins_;
};

} // namespace runtime
} // namespace gard
