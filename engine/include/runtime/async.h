#pragma once

#include "runtime/runtime.h"
#include <queue>
#include <deque>
#include <functional>
#include <memory>
#include <chrono>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>

namespace gard {
namespace runtime {

// --- Thread Pool for async native dispatch ---

class ThreadPool {
public:
    static ThreadPool& instance();

    // Submit a task to the thread pool (work-stealing: tasks go to least-loaded worker)
    void submit(std::function<void()> task);

    // Get number of worker threads
    unsigned int workerCount() const { return (unsigned int)workers_.size(); }

    ~ThreadPool();

private:
    ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    std::vector<std::thread> workers_;

    // Work-stealing: per-worker queues + shared overflow queue
    struct WorkerQueue {
        std::deque<std::function<void()>> tasks;
        std::mutex mutex;
        std::atomic<int> size{0};
    };
    std::vector<std::unique_ptr<WorkerQueue>> workerQueues_;
    std::atomic<unsigned int> nextWorker_{0}; // round-robin for submit

    // Shared overflow queue (fallback when all workers are busy)
    std::queue<std::function<void()>> overflowQueue_;
    std::mutex overflowMutex_;

    std::condition_variable cv_;
    std::mutex cvMutex_;
    bool shutdown_;
};

// --- Async Future (represents a pending async result) ---

struct AsyncFuture {
    std::atomic<bool> resolved{false};
    Value result;
    std::mutex mutex;
    uint64_t taskId = 0; // task waiting on this future

    AsyncFuture() {}
};

// --- Task State ---

enum class TaskState {
    Ready,      // ready to run
    Running,    // currently executing
    Suspended,  // awaiting something
    Completed,  // finished with a value
    Cancelled,  // cancelled
    Failed,     // threw an exception
};

// --- Task (lightweight coroutine) ---

struct Task {
    uint64_t id;
    TaskState state = TaskState::Ready;
    Value result;

    // Coroutine state (saved execution context)
    uint32_t savedPC = 0;
    uint16_t savedFuncIndex = 0;
    std::vector<Value> savedStack;
    std::vector<Value> savedLocals;
    std::vector<CallFrame> savedCallStack;

    // Continuation: what to do when this task completes
    std::vector<uint64_t> waiters; // tasks waiting on this one

    // Timer: when to wake up (for Task.delay)
    std::chrono::steady_clock::time_point wakeTime;
    bool hasTimer = false;

    // Cancellation
    bool cancelRequested = false;

    Task(uint64_t id) : id(id) {}
};

// --- Future<T> ---

struct Future {
    uint64_t taskId;
    bool isResolved = false;
    Value value;

    Future(uint64_t tid) : taskId(tid) {}
};

// --- Stream<T> ---

struct Stream {
    std::queue<Value> buffer;
    bool closed = false;
    std::vector<uint64_t> readers; // tasks waiting to read

    void write(const Value& val) { buffer.push(val); }
    bool hasData() const { return !buffer.empty(); }
    Value read() {
        if (buffer.empty()) return Value::makeNull();
        Value v = buffer.front();
        buffer.pop();
        return v;
    }
};

// --- Cooperative Scheduler ---

class Scheduler {
public:
    Scheduler();
    ~Scheduler();

    // Create a new task from a function
    uint64_t spawn(uint16_t funcIndex, const std::vector<Value>& args = {});

    // Run the scheduler until all tasks complete
    void runUntilComplete();

    // Run one tick (process one ready task)
    bool tick();

    // Suspend the current task (called by await)
    void suspendCurrent(uint64_t waitOnTaskId);

    // Resume a task (called when awaited value is ready)
    void resume(uint64_t taskId);

    // Complete a task with a value
    void complete(uint64_t taskId, const Value& result);

    // Cancel a task
    void cancel(uint64_t taskId);

    // Timer: schedule wakeup after delay
    void scheduleTimer(uint64_t taskId, std::chrono::milliseconds delay);

    // Promise.all: wait for multiple tasks
    uint64_t promiseAll(const std::vector<uint64_t>& taskIds);

    // Stream operations
    uint64_t createStream();
    void streamWrite(uint64_t streamId, const Value& val);
    void streamClose(uint64_t streamId);
    Value streamRead(uint64_t streamId, uint64_t readerTaskId);

    // State
    uint64_t currentTaskId() const { return currentTaskId_; }
    Task* getTask(uint64_t id);
    size_t pendingTasks() const { return readyQueue_.size() + suspendedTasks_.size(); }

private:
    void checkTimers();
    uint64_t nextTaskId();

    // Task storage
    std::unordered_map<uint64_t, std::unique_ptr<Task>> tasks_;
    std::queue<uint64_t> readyQueue_;
    std::vector<uint64_t> suspendedTasks_;

    // Streams
    std::unordered_map<uint64_t, std::unique_ptr<Stream>> streams_;
    uint64_t nextStreamId_ = 1;

    // Current execution
    uint64_t currentTaskId_ = 0;
    uint64_t taskIdCounter_ = 0;

    // Timer tracking
    struct TimerEntry {
        uint64_t taskId;
        std::chrono::steady_clock::time_point wakeTime;
    };
    std::vector<TimerEntry> timers_;
};

// --- Async-aware VM extension ---

class AsyncVM {
public:
    AsyncVM(const VMConfig& config = VMConfig());

    // Run a program with async support
    int run(const bytecode::BytecodeModule& module);

private:
    void executeTask(Task& task);

    VM vm_;
    Scheduler scheduler_;
    const bytecode::BytecodeModule* module_ = nullptr;
};

} // namespace runtime
} // namespace gard
