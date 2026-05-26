#include "runtime/vm_extensions.h"
#include <iostream>
#include <thread>

namespace gard {
namespace runtime {

// ============================================================
// Sandboxed VM
// ============================================================

SandboxedVM::SandboxedVM(const SandboxLimits& limits) : limits_(limits) {}

SandboxResult SandboxedVM::run(const bytecode::BytecodeModule& module) {
    SandboxResult result;
    auto startTime = std::chrono::steady_clock::now();

    VMConfig config;
    config.maxStackSize = limits_.maxStackDepth;
    config.maxCallDepth = limits_.maxCallDepth;
    config.maxHeapSize = limits_.maxMemoryBytes;

    VM vm(config);

    // Run with instruction counting
    // In a full implementation, the VM would check limits each instruction.
    // Here we use the timeout approach.
    std::atomic<bool> finished{false};
    int exitCode = 0;

    std::thread runner([&]() {
        exitCode = vm.run(module);
        finished = true;
    });

    // Wait with timeout
    auto deadline = startTime + limits_.timeout;
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!finished) {
        result.timedOut = true;
        result.error = "Execution timed out";
        // In production, we'd need to forcefully stop the VM thread
        runner.detach();
    } else {
        runner.join();
        result.exitCode = exitCode;
    }

    auto endTime = std::chrono::steady_clock::now();
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    result.peakMemory = vm.heapUsage();

    return result;
}

// ============================================================
// Hot Reload
// ============================================================

HotReloader::HotReloader(VM& vm) : vm_(vm) {}

bool HotReloader::reloadFunction(const std::string& name, const bytecode::BytecodeModule& newModule) {
    // Find the function in the new module
    for (auto& fn : newModule.functions) {
        if (fn.name == name) {
            // In a full implementation, we'd patch the function table
            // in the running VM to point to the new code
            pendingReload_ = true;
            reloadCount_++;
            return true;
        }
    }
    return false;
}

bool HotReloader::reloadModule(const bytecode::BytecodeModule& newModule) {
    //TODO:
    // Replace the entire module
    // In production, this would:
    // 1. Wait for a safe point (between instructions)
    // 2. Swap the module pointer
    // 3. Remap any active call frames to new code offsets
    pendingReload_ = true;
    reloadCount_++;
    return true;
}

// ============================================================
// JIT Trigger
// ============================================================

JITTrigger::JITTrigger(int hotThreshold) : hotThreshold_(hotThreshold) {}

bool JITTrigger::recordCall(const std::string& funcName) {
    auto& profile = profiles_[funcName];
    profile.functionName = funcName;
    profile.callCount++;

    if (!profile.isHot && static_cast<int>(profile.callCount) >= hotThreshold_) {
        profile.isHot = true;
        return true; // just became hot
    }
    return false;
}

void JITTrigger::recordInstructions(const std::string& funcName, uint64_t count) {
    auto& profile = profiles_[funcName];
    profile.functionName = funcName;
    profile.instructionCount += count;
}

bool JITTrigger::isHot(const std::string& funcName) const {
    auto it = profiles_.find(funcName);
    if (it != profiles_.end()) return it->second.isHot;
    return false;
}

std::vector<std::string> JITTrigger::getHotFunctions() const {
    std::vector<std::string> result;
    for (auto& [name, profile] : profiles_) {
        if (profile.isHot && !profile.isCompiled) {
            result.push_back(name);
        }
    }
    return result;
}

void JITTrigger::markCompiled(const std::string& funcName) {
    auto it = profiles_.find(funcName);
    if (it != profiles_.end()) {
        it->second.isCompiled = true;
    }
}

const JITProfile* JITTrigger::getProfile(const std::string& funcName) const {
    auto it = profiles_.find(funcName);
    if (it != profiles_.end()) return &it->second;
    return nullptr;
}

// ============================================================
// Plugin Manager
// ============================================================

PluginManager::PluginManager(VM& vm) : vm_(vm) {}

bool PluginManager::loadPlugin(std::unique_ptr<VMPlugin> plugin) {
    if (!plugin) return false;
    std::string pluginName = plugin->name();

    if (isLoaded(pluginName)) return false; // already loaded

    if (!plugin->onLoad(vm_)) return false;

    plugins_[pluginName] = std::move(plugin);
    return true;
}

bool PluginManager::unloadPlugin(const std::string& name) {
    auto it = plugins_.find(name);
    if (it == plugins_.end()) return false;

    it->second->onUnload();
    plugins_.erase(it);
    return true;
}

std::vector<std::string> PluginManager::loadedPlugins() const {
    std::vector<std::string> names;
    for (auto& [name, _] : plugins_) {
        names.push_back(name);
    }
    return names;
}

bool PluginManager::isLoaded(const std::string& name) const {
    return plugins_.find(name) != plugins_.end();
}

} // namespace runtime
} // namespace gard
