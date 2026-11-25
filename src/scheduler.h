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
Tick counterNow();
Tick counterFrequency();
Tick asTicks(FILETIME time);
SYSTEMTIME asSystemTime(Tick time);
double milliseconds(Tick ticks);

enum class LocalTimeKind { unique, ambiguous, nonexistent, error };
LocalTimeKind localToUtc(SYSTEMTIME local, DYNAMIC_TIME_ZONE_INFORMATION zone, Tick& result);

struct ClockSample {
    Tick utc = 0;
    Tick counter = 0;
};

struct Job {
    Tick target = 0;
    POINT point{};
    ULONG_PTR cookie = 0;
    HWND expectedWindow = nullptr;
};

enum class Outcome {
    sent, cancelled, invalidPoint, targetChanged, partial, error
};

struct Result {
    Outcome outcome;
    ClockSample dispatch{};
    UINT inserted = 0;
    DWORD error = 0;
};

const char* outcomeName(Outcome outcome);

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
