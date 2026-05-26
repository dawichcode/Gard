#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <string>
#include <memory>
#include <queue>

namespace gard {
namespace runtime {

// --- Event types ---

enum class EventType {
    Read,
    Write,
    Error,
    Timer,
    Signal,
};

// --- Event callback ---

using EventCallback = std::function<void(int fd, EventType type)>;
using TimerCallback = std::function<void()>;
using SignalCallback = std::function<void(int signum)>;

// --- Platform backend interface ---

class IOBackend {
public:
    virtual ~IOBackend() = default;

    virtual bool init() = 0;
    virtual void shutdown() = 0;

    // Register/unregister file descriptors
    virtual bool addFd(int fd, EventType interest) = 0;
    virtual bool modFd(int fd, EventType interest) = 0;
    virtual bool removeFd(int fd) = 0;

    // Poll for events (returns number of ready fds, -1 on error)
    // timeout_ms: -1 = block forever, 0 = non-blocking, >0 = wait up to N ms
    virtual int poll(int timeout_ms) = 0;

    // Get ready events after poll
    struct ReadyEvent {
        int fd;
        EventType type;
    };
    virtual std::vector<ReadyEvent> getReadyEvents() = 0;

    // Backend name
    virtual const char* name() const = 0;
};

// --- Linux epoll backend ---

#ifdef __linux__
class EpollBackend : public IOBackend {
public:
    EpollBackend();
    ~EpollBackend() override;

    bool init() override;
    void shutdown() override;
    bool addFd(int fd, EventType interest) override;
    bool modFd(int fd, EventType interest) override;
    bool removeFd(int fd) override;
    int poll(int timeout_ms) override;
    std::vector<ReadyEvent> getReadyEvents() override;
    const char* name() const override { return "epoll"; }

private:
    int epollFd_ = -1;
    static constexpr int MAX_EVENTS = 64;
    void* events_ = nullptr; // struct epoll_event* (opaque in header)
    int numReady_ = 0;
};
#endif

// --- Timer wheel entry ---

struct TimerEntry {
    uint64_t id;
    std::chrono::steady_clock::time_point deadline;
    TimerCallback callback;
    bool repeating;
    std::chrono::milliseconds interval;
    bool cancelled = false;
};

// --- Signal handler entry ---

struct SignalEntry {
    int signum;
    SignalCallback callback;
};

// --- Event Loop ---

class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    // Initialize with platform-appropriate backend
    bool init();

    // Run the event loop (blocks until stop() is called or no more work)
    void run();

    // Run one iteration
    bool runOnce(int timeout_ms = -1);

    // Stop the event loop
    void stop();

    // --- File descriptor management ---
    void watchRead(int fd, EventCallback cb);
    void watchWrite(int fd, EventCallback cb);
    void unwatch(int fd);

    // --- Timers ---
    uint64_t setTimeout(std::chrono::milliseconds delay, TimerCallback cb);
    uint64_t setInterval(std::chrono::milliseconds interval, TimerCallback cb);
    void clearTimer(uint64_t timerId);

    // --- Signals ---
    void onSignal(int signum, SignalCallback cb);

    // --- Non-blocking I/O helpers ---
    // These return immediately; callback is invoked when ready
    void asyncRead(int fd, EventCallback cb);
    void asyncWrite(int fd, EventCallback cb);

    // --- State ---
    bool isRunning() const { return running_; }
    size_t watchedFds() const { return fdCallbacks_.size(); }
    size_t pendingTimers() const { return timers_.size(); }

private:
    void processTimers();
    int nextTimerTimeout() const;
    uint64_t nextTimerId();

    std::unique_ptr<IOBackend> backend_;
    bool running_ = false;

    // FD -> callback mapping
    std::unordered_map<int, EventCallback> fdCallbacks_;

    // Timer management
    std::vector<TimerEntry> timers_;
    uint64_t timerIdCounter_ = 0;

    // Signal handlers
    std::vector<SignalEntry> signalHandlers_;
};

// --- DNS resolution (async) ---

struct DNSResult {
    std::string hostname;
    std::vector<std::string> addresses;
    std::string error;
    bool success = false;
};

using DNSCallback = std::function<void(const DNSResult&)>;

// Non-blocking DNS resolve (uses a thread internally)
void asyncResolve(const std::string& hostname, DNSCallback callback);

} // namespace runtime
} // namespace gard
