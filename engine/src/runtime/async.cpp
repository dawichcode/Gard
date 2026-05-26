#include "runtime/async.h"
#include <algorithm>
#include <iostream>
#include <thread>
#include <future>

namespace gard {
namespace runtime {

// --- Thread Pool for async native dispatch ---

ThreadPool& ThreadPool::instance() {
    static ThreadPool pool;
    return pool;
}

ThreadPool::ThreadPool() : shutdown_(false) {
    unsigned int numThreads = std::max(2u, std::thread::hardware_concurrency());

    // Create per-worker queues
    for (unsigned int i = 0; i < numThreads; i++) {
        workerQueues_.push_back(std::make_unique<WorkerQueue>());
    }

    // Create worker threads with work-stealing
    for (unsigned int i = 0; i < numThreads; i++) {
        workers_.emplace_back([this, i, numThreads]() {
            auto& myQueue = *workerQueues_[i];
            while (true) {
                std::function<void()> task;

                // 1. Try own queue first (no contention — fast path)
                {
                    std::lock_guard<std::mutex> lock(myQueue.mutex);
                    if (!myQueue.tasks.empty()) {
                        task = std::move(myQueue.tasks.front());
                        myQueue.tasks.pop_front();
                        myQueue.size.fetch_sub(1, std::memory_order_relaxed);
                    }
                }

                // 2. Try overflow queue
                if (!task) {
                    std::lock_guard<std::mutex> lock(overflowMutex_);
                    if (!overflowQueue_.empty()) {
                        task = std::move(overflowQueue_.front());
                        overflowQueue_.pop();
                    }
                }

                // 3. Work-stealing: steal from other workers (back of their deque)
                if (!task) {
                    for (unsigned int j = 1; j < numThreads; j++) {
                        unsigned int victim = (i + j) % numThreads;
                        auto& victimQueue = *workerQueues_[victim];
                        std::lock_guard<std::mutex> lock(victimQueue.mutex);
                        if (!victimQueue.tasks.empty()) {
                            task = std::move(victimQueue.tasks.back());
                            victimQueue.tasks.pop_back();
                            victimQueue.size.fetch_sub(1, std::memory_order_relaxed);
                            break;
                        }
                    }
                }

                // 4. No work found — wait
                if (!task) {
                    std::unique_lock<std::mutex> lock(cvMutex_);
                    cv_.wait_for(lock, std::chrono::microseconds(500), [this, &myQueue]() {
                        return shutdown_ || myQueue.size.load(std::memory_order_relaxed) > 0;
                    });
                    if (shutdown_) return;
                    continue;
                }

                task();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    shutdown_ = true;
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

void ThreadPool::submit(std::function<void()> task) {
    // Round-robin assignment to worker queues (distributes load evenly)
    unsigned int idx = nextWorker_.fetch_add(1, std::memory_order_relaxed) % (unsigned int)workerQueues_.size();
    {
        std::lock_guard<std::mutex> lock(workerQueues_[idx]->mutex);
        workerQueues_[idx]->tasks.push_back(std::move(task));
        workerQueues_[idx]->size.fetch_add(1, std::memory_order_relaxed);
    }
    cv_.notify_one();
}

// --- Scheduler ---

Scheduler::Scheduler() {}
Scheduler::~Scheduler() {}

uint64_t Scheduler::nextTaskId() {
    return ++taskIdCounter_;
}

uint64_t Scheduler::spawn(uint16_t funcIndex, const std::vector<Value>& args) {
    uint64_t id = nextTaskId();
    auto task = std::make_unique<Task>(id);
    task->state = TaskState::Ready;
    task->savedFuncIndex = funcIndex;

    // Store args in saved locals
    task->savedLocals = args;

    tasks_[id] = std::move(task);
    readyQueue_.push(id);
    return id;
}

void Scheduler::runUntilComplete() {
    while (!readyQueue_.empty() || !suspendedTasks_.empty() || !timers_.empty()) {
        // Check timers first
        checkTimers();

        // Process one ready task
        if (!tick()) {
            // No ready tasks — if there are timers, sleep briefly
            if (!timers_.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else if (!suspendedTasks_.empty()) {
                // Deadlock: suspended tasks with nothing to wake them
                break;
            } else {
                break;
            }
        }
    }
}

bool Scheduler::tick() {
    if (readyQueue_.empty()) return false;

    uint64_t taskId = readyQueue_.front();
    readyQueue_.pop();

    auto it = tasks_.find(taskId);
    if (it == tasks_.end()) return true; // task was removed

    Task* task = it->second.get();
    if (task->state == TaskState::Cancelled) return true;

    task->state = TaskState::Running;
    currentTaskId_ = taskId;

    // The actual execution is handled by AsyncVM::executeTask
    // Here we just manage the scheduling

    return true;
}

void Scheduler::suspendCurrent(uint64_t waitOnTaskId) {
    auto it = tasks_.find(currentTaskId_);
    if (it == tasks_.end()) return;

    Task* current = it->second.get();
    current->state = TaskState::Suspended;
    suspendedTasks_.push_back(currentTaskId_);

    // Register as waiter on the target task
    auto targetIt = tasks_.find(waitOnTaskId);
    if (targetIt != tasks_.end()) {
        targetIt->second->waiters.push_back(currentTaskId_);
    }
}

void Scheduler::resume(uint64_t taskId) {
    auto it = tasks_.find(taskId);
    if (it == tasks_.end()) return;

    Task* task = it->second.get();
    if (task->state != TaskState::Suspended) return;

    task->state = TaskState::Ready;
    readyQueue_.push(taskId);

    // Remove from suspended list
    suspendedTasks_.erase(
        std::remove(suspendedTasks_.begin(), suspendedTasks_.end(), taskId),
        suspendedTasks_.end());
}

void Scheduler::complete(uint64_t taskId, const Value& result) {
    auto it = tasks_.find(taskId);
    if (it == tasks_.end()) return;

    Task* task = it->second.get();
    task->state = TaskState::Completed;
    task->result = result;

    // Wake up all waiters
    for (uint64_t waiterId : task->waiters) {
        resume(waiterId);
    }
    task->waiters.clear();
}

void Scheduler::cancel(uint64_t taskId) {
    auto it = tasks_.find(taskId);
    if (it == tasks_.end()) return;

    Task* task = it->second.get();
    task->state = TaskState::Cancelled;
    task->cancelRequested = true;

    // Remove from suspended
    suspendedTasks_.erase(
        std::remove(suspendedTasks_.begin(), suspendedTasks_.end(), taskId),
        suspendedTasks_.end());
}

void Scheduler::scheduleTimer(uint64_t taskId, std::chrono::milliseconds delay) {
    TimerEntry entry;
    entry.taskId = taskId;
    entry.wakeTime = std::chrono::steady_clock::now() + delay;
    timers_.push_back(entry);

    // Suspend the task
    auto it = tasks_.find(taskId);
    if (it != tasks_.end()) {
        it->second->state = TaskState::Suspended;
        it->second->hasTimer = true;
        it->second->wakeTime = entry.wakeTime;
        suspendedTasks_.push_back(taskId);
    }
}

uint64_t Scheduler::promiseAll(const std::vector<uint64_t>& taskIds) {
    // Create a new task that waits for all given tasks
    uint64_t allId = nextTaskId();
    auto allTask = std::make_unique<Task>(allId);
    allTask->state = TaskState::Suspended;

    // Track how many we're waiting on
    int pending = 0;
    for (uint64_t tid : taskIds) {
        auto it = tasks_.find(tid);
        if (it != tasks_.end() && it->second->state != TaskState::Completed) {
            it->second->waiters.push_back(allId);
            pending++;
        }
    }

    if (pending == 0) {
        allTask->state = TaskState::Completed;
        // Collect results
        Value arr = Value::makeArray();
        for (uint64_t tid : taskIds) {
            auto it = tasks_.find(tid);
            if (it != tasks_.end()) {
                arr.arrVal->elements.push_back(it->second->result);
            }
        }
        allTask->result = arr;
    } else {
        suspendedTasks_.push_back(allId);
    }

    tasks_[allId] = std::move(allTask);
    return allId;
}

// --- Stream operations ---

uint64_t Scheduler::createStream() {
    uint64_t id = nextStreamId_++;
    streams_[id] = std::make_unique<Stream>();
    return id;
}

void Scheduler::streamWrite(uint64_t streamId, const Value& val) {
    auto it = streams_.find(streamId);
    if (it == streams_.end()) return;

    it->second->write(val);

    // Wake up any readers
    for (uint64_t readerId : it->second->readers) {
        resume(readerId);
    }
    it->second->readers.clear();
}

void Scheduler::streamClose(uint64_t streamId) {
    auto it = streams_.find(streamId);
    if (it == streams_.end()) return;

    it->second->closed = true;

    // Wake up readers so they see the close
    for (uint64_t readerId : it->second->readers) {
        resume(readerId);
    }
    it->second->readers.clear();
}

Value Scheduler::streamRead(uint64_t streamId, uint64_t readerTaskId) {
    auto it = streams_.find(streamId);
    if (it == streams_.end()) return Value::makeNull();

    if (it->second->hasData()) {
        return it->second->read();
    }

    if (it->second->closed) {
        return Value::makeNull(); // stream ended
    }

    // No data available — suspend reader
    it->second->readers.push_back(readerTaskId);
    suspendCurrent(0); // suspend without a specific task to wait on
    return Value::makeNull();
}

Task* Scheduler::getTask(uint64_t id) {
    auto it = tasks_.find(id);
    if (it != tasks_.end()) return it->second.get();
    return nullptr;
}

void Scheduler::checkTimers() {
    auto now = std::chrono::steady_clock::now();

    timers_.erase(
        std::remove_if(timers_.begin(), timers_.end(),
            [&](const TimerEntry& entry) {
                if (now >= entry.wakeTime) {
                    resume(entry.taskId);
                    return true;
                }
                return false;
            }),
        timers_.end());
}

// --- AsyncVM ---

AsyncVM::AsyncVM(const VMConfig& config) : vm_(config) {}

int AsyncVM::run(const bytecode::BytecodeModule& module) {
    module_ = &module;

    if (module.entryFunction < 0) {
        std::cerr << "Runtime error: no entry function found" << std::endl;
        return 1;
    }

    // Spawn main as the first task
    scheduler_.spawn(static_cast<uint16_t>(module.entryFunction));

    // For now, delegate to the synchronous VM
    // Full async execution would interleave task execution with the scheduler
    return vm_.run(module);
}

void AsyncVM::executeTask(Task& task) {
    // In a full implementation, this would:
    // 1. Restore task's saved state (PC, stack, locals)
    // 2. Execute until the task hits an await or completes
    // 3. Save state back to the task
    // 4. Return control to the scheduler
}

} // namespace runtime
} // namespace gard
