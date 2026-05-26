#include "runtime/event_loop.h"
#include <algorithm>
#include <iostream>
#include <cstring>
#include <thread>

#ifdef __linux__
#include <sys/epoll.h>
#include <unistd.h>
#include <signal.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#endif

namespace gard {
namespace runtime {

// ============================================================
// Epoll Backend (Linux)
// ============================================================

#ifdef __linux__

EpollBackend::EpollBackend() {}

EpollBackend::~EpollBackend() {
    shutdown();
}

bool EpollBackend::init() {
    epollFd_ = epoll_create1(0);
    if (epollFd_ < 0) return false;
    events_ = new struct ::epoll_event[MAX_EVENTS];
    return true;
}

void EpollBackend::shutdown() {
    if (epollFd_ >= 0) {
        close(epollFd_);
        epollFd_ = -1;
    }
    delete[] static_cast<struct ::epoll_event*>(events_);
    events_ = nullptr;
}

bool EpollBackend::addFd(int fd, EventType interest) {
    struct ::epoll_event ev;
    ev.data.fd = fd;
    ev.events = 0;
    if (interest == EventType::Read) ev.events = EPOLLIN;
    else if (interest == EventType::Write) ev.events = EPOLLOUT;
    else ev.events = EPOLLIN | EPOLLOUT;
    return epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool EpollBackend::modFd(int fd, EventType interest) {
    struct ::epoll_event ev;
    ev.data.fd = fd;
    ev.events = 0;
    if (interest == EventType::Read) ev.events = EPOLLIN;
    else if (interest == EventType::Write) ev.events = EPOLLOUT;
    else ev.events = EPOLLIN | EPOLLOUT;
    return epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev) == 0;
}

bool EpollBackend::removeFd(int fd) {
    return epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
}

int EpollBackend::poll(int timeout_ms) {
    numReady_ = epoll_wait(epollFd_, static_cast<struct ::epoll_event*>(events_), MAX_EVENTS, timeout_ms);
    return numReady_;
}

std::vector<IOBackend::ReadyEvent> EpollBackend::getReadyEvents() {
    std::vector<ReadyEvent> result;
    auto* evts = static_cast<struct ::epoll_event*>(events_);
    for (int i = 0; i < numReady_; i++) {
        ReadyEvent re;
        re.fd = evts[i].data.fd;
        if (evts[i].events & EPOLLIN) re.type = EventType::Read;
        else if (evts[i].events & EPOLLOUT) re.type = EventType::Write;
        else re.type = EventType::Error;
        result.push_back(re);
    }
    return result;
}

#endif // __linux__

// ============================================================
// Event Loop
// ============================================================

EventLoop::EventLoop() {}

EventLoop::~EventLoop() {
    stop();
}

bool EventLoop::init() {
#ifdef __linux__
    backend_ = std::make_unique<EpollBackend>();
#else
    // Fallback: no backend (timers still work)
    backend_ = nullptr;
#endif
    if (backend_) {
        return backend_->init();
    }
    return true; // timer-only mode
}

void EventLoop::run() {
    running_ = true;
    while (running_) {
        if (!runOnce()) {
            // No more work
            if (fdCallbacks_.empty() && timers_.empty()) break;
        }
    }
}

bool EventLoop::runOnce(int timeout_ms) {
    // Calculate timeout based on next timer
    int effectiveTimeout = timeout_ms;
    int timerTimeout = nextTimerTimeout();
    if (timerTimeout >= 0) {
        if (effectiveTimeout < 0) effectiveTimeout = timerTimeout;
        else effectiveTimeout = std::min(effectiveTimeout, timerTimeout);
    }

    // Poll for I/O events
    if (backend_ && !fdCallbacks_.empty()) {
        int ready = backend_->poll(effectiveTimeout);
        if (ready > 0) {
            auto events = backend_->getReadyEvents();
            for (auto& ev : events) {
                auto it = fdCallbacks_.find(ev.fd);
                if (it != fdCallbacks_.end()) {
                    it->second(ev.fd, ev.type);
                }
            }
        }
    } else if (effectiveTimeout > 0) {
        // No fds to watch, just sleep for timer
        std::this_thread::sleep_for(std::chrono::milliseconds(
            std::min(effectiveTimeout, 10)));
    }

    // Process expired timers
    processTimers();

    return !fdCallbacks_.empty() || !timers_.empty();
}

void EventLoop::stop() {
    running_ = false;
    if (backend_) {
        backend_->shutdown();
    }
}

// --- FD management ---

void EventLoop::watchRead(int fd, EventCallback cb) {
    fdCallbacks_[fd] = std::move(cb);
    if (backend_) backend_->addFd(fd, EventType::Read);
}

void EventLoop::watchWrite(int fd, EventCallback cb) {
    fdCallbacks_[fd] = std::move(cb);
    if (backend_) backend_->addFd(fd, EventType::Write);
}

void EventLoop::unwatch(int fd) {
    fdCallbacks_.erase(fd);
    if (backend_) backend_->removeFd(fd);
}

void EventLoop::asyncRead(int fd, EventCallback cb) {
    watchRead(fd, std::move(cb));
}

void EventLoop::asyncWrite(int fd, EventCallback cb) {
    watchWrite(fd, std::move(cb));
}

// --- Timers ---

uint64_t EventLoop::nextTimerId() {
    return ++timerIdCounter_;
}

uint64_t EventLoop::setTimeout(std::chrono::milliseconds delay, TimerCallback cb) {
    TimerEntry entry;
    entry.id = nextTimerId();
    entry.deadline = std::chrono::steady_clock::now() + delay;
    entry.callback = std::move(cb);
    entry.repeating = false;
    entry.interval = delay;
    timers_.push_back(std::move(entry));
    return entry.id;
}

uint64_t EventLoop::setInterval(std::chrono::milliseconds interval, TimerCallback cb) {
    TimerEntry entry;
    entry.id = nextTimerId();
    entry.deadline = std::chrono::steady_clock::now() + interval;
    entry.callback = std::move(cb);
    entry.repeating = true;
    entry.interval = interval;
    timers_.push_back(std::move(entry));
    return entry.id;
}

void EventLoop::clearTimer(uint64_t timerId) {
    for (auto& t : timers_) {
        if (t.id == timerId) {
            t.cancelled = true;
            break;
        }
    }
    timers_.erase(
        std::remove_if(timers_.begin(), timers_.end(),
            [](const TimerEntry& t) { return t.cancelled; }),
        timers_.end());
}

void EventLoop::processTimers() {
    auto now = std::chrono::steady_clock::now();
    std::vector<TimerEntry> fired;

    // Find expired timers
    for (auto& t : timers_) {
        if (!t.cancelled && now >= t.deadline) {
            fired.push_back(t);
        }
    }

    // Remove one-shot timers, reschedule repeating ones
    timers_.erase(
        std::remove_if(timers_.begin(), timers_.end(),
            [&now](const TimerEntry& t) {
                return !t.cancelled && now >= t.deadline && !t.repeating;
            }),
        timers_.end());

    // Reschedule repeating timers
    for (auto& t : timers_) {
        if (!t.cancelled && t.repeating && now >= t.deadline) {
            t.deadline = now + t.interval;
        }
    }

    // Fire callbacks
    for (auto& t : fired) {
        if (t.callback) t.callback();
    }
}

int EventLoop::nextTimerTimeout() const {
    if (timers_.empty()) return -1;

    auto now = std::chrono::steady_clock::now();
    auto earliest = timers_[0].deadline;
    for (auto& t : timers_) {
        if (!t.cancelled && t.deadline < earliest) {
            earliest = t.deadline;
        }
    }

    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(earliest - now);
    return std::max(0, static_cast<int>(diff.count()));
}

// --- Signals ---

void EventLoop::onSignal(int signum, SignalCallback cb) {
    signalHandlers_.push_back({signum, std::move(cb)});
#ifdef __linux__
    // Set up signal handler (simplified — real impl would use signalfd)
    signal(signum, [](int) {}); // ignore default
#endif
}

// --- Async DNS ---

void asyncResolve(const std::string& hostname, DNSCallback callback) {
    // Run DNS resolution in a separate thread
    std::thread([hostname, callback]() {
        DNSResult result;
        result.hostname = hostname;

#ifdef __linux__
        struct addrinfo hints, *res;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        int err = getaddrinfo(hostname.c_str(), nullptr, &hints, &res);
        if (err != 0) {
            result.error = gai_strerror(err);
            result.success = false;
        } else {
            result.success = true;
            for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
                char addr[INET6_ADDRSTRLEN];
                if (p->ai_family == AF_INET) {
                    struct sockaddr_in* ipv4 = (struct sockaddr_in*)p->ai_addr;
                    inet_ntop(AF_INET, &ipv4->sin_addr, addr, sizeof(addr));
                    result.addresses.push_back(addr);
                } else if (p->ai_family == AF_INET6) {
                    struct sockaddr_in6* ipv6 = (struct sockaddr_in6*)p->ai_addr;
                    inet_ntop(AF_INET6, &ipv6->sin6_addr, addr, sizeof(addr));
                    result.addresses.push_back(addr);
                }
            }
            freeaddrinfo(res);
        }
#else
        result.error = "DNS not supported on this platform";
        result.success = false;
#endif

        callback(result);
    }).detach();
}

} // namespace runtime
} // namespace gard
