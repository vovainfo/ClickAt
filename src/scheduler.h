#pragma once

#include <windows.h>
#include <cstdint>
#include <mutex>

namespace clickat {

using Tick = std::int64_t;
constexpr Tick ticksPerMs = 10'000;
constexpr Tick ticksPerSecond = 1'000 * ticksPerMs;

struct Handle {
    explicit Handle(HANDLE value = nullptr) : value(value) {}
    ~Handle() { if (value) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE value;
};

Tick utcNow();
Tick asTicks(FILETIME time);
SYSTEMTIME asSystemTime(Tick time);
double milliseconds(Tick ticks);

enum class LocalTimeKind { unique, ambiguous, nonexistent, error };
LocalTimeKind localToUtc(SYSTEMTIME local, DYNAMIC_TIME_ZONE_INFORMATION zone, Tick& result);

struct Job {
    Tick target = 0;
    POINT point{};
};

enum class Outcome {
    sent, cancelled, invalidPoint, partial, error
};

struct Result {
    Outcome outcome;
    Tick dispatchedAt = 0;
    UINT inserted = 0;
    DWORD error = 0;
};

class Scheduler {
public:
    Result run(const Job& job);
    bool cancel();
    void clockChanged();
    void setLocked(bool locked);
    void setSuspended(bool suspended);

private:
    void setUnavailable(bool& flag, bool value);

    Handle changed_{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::mutex mutex_;
    bool cancelled_ = false;
    bool finished_ = false;
    bool locked_ = false;
    bool suspended_ = false;
};

} // namespace clickat
