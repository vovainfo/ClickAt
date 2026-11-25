#include "scheduler.h"

#include <algorithm>
#include <cwchar>
#include <intrin.h>
#include <wtsapi32.h>

namespace clickat {
namespace {

constexpr Tick prepareLead = 12 * ticksPerMs;
constexpr Tick spinLead = 2 * ticksPerMs;

bool interactiveDesktop() {
    auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!input) return false;

    wchar_t active[MAX_PATH]{};
    wchar_t own[MAX_PATH]{};
    bool same = GetUserObjectInformationW(input, UOI_NAME, active, sizeof(active), nullptr)
        && GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
                                    own, sizeof(own), nullptr)
        && _wcsicmp(active, own) == 0;
    CloseDesktop(input);
    return same;
}

bool activeSession() {
    LPWSTR data = nullptr;
    DWORD size = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                                    WTSConnectState, &data, &size)) return false;
    const bool active = size >= sizeof(WTS_CONNECTSTATE_CLASS)
        && *reinterpret_cast<const WTS_CONNECTSTATE_CLASS*>(data) == WTSActive;
    WTSFreeMemory(data);
    return active;
}

bool prepareInput(const Job& job, INPUT (&inputs)[3]) {
    if (!MonitorFromPoint(job.point, MONITOR_DEFAULTTONULL)) return false;
    const auto absolute = [](int coordinate, int origin, int extent) {
        // Select the center of the requested pixel in the 16-bit input range.
        const Tick offset = static_cast<Tick>(coordinate) - origin;
        const Tick normalized = ((2 * offset + 1) * 65'536) / (2LL * extent);
        return static_cast<LONG>(std::clamp<Tick>(normalized, 0, 65'535));
    };
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 0 || height <= 0) return false;
    inputs[0].mi.dx = absolute(job.point.x, GetSystemMetrics(SM_XVIRTUALSCREEN), width);
    inputs[0].mi.dy = absolute(job.point.y, GetSystemMetrics(SM_YVIRTUALSCREEN), height);
    inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    for (auto& input : inputs) {
        input.type = INPUT_MOUSE;
        input.mi.dwExtraInfo = job.cookie;
    }
    return true;
}

struct StayAwake {
    bool requested = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) != 0;
    ~StayAwake() { if (requested) SetThreadExecutionState(ES_CONTINUOUS); }
};

struct SchedulingPriority {
    const int previous = GetThreadPriority(GetCurrentThread());
    const bool raised = previous != THREAD_PRIORITY_ERROR_RETURN && previous < THREAD_PRIORITY_HIGHEST
        && SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    ~SchedulingPriority() { if (raised) SetThreadPriority(GetCurrentThread(), previous); }
};

bool toFileTicks(const SYSTEMTIME& time, Tick& result) {
    FILETIME file{};
    if (!SystemTimeToFileTime(&time, &file)) return false;
    result = asTicks(file);
    return true;
}

} // namespace

Tick asTicks(FILETIME time) {
    return (static_cast<Tick>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

Tick utcNow() {
    FILETIME time{};
    GetSystemTimePreciseAsFileTime(&time);
    return asTicks(time);
}

SYSTEMTIME asSystemTime(Tick time) {
    const FILETIME file{static_cast<DWORD>(time), static_cast<DWORD>(time >> 32)};
    SYSTEMTIME result{};
    FileTimeToSystemTime(&file, &result);
    return result;
}

Tick counterNow() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

Tick counterFrequency() {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value.QuadPart;
}

double milliseconds(Tick ticks) {
    return static_cast<double>(ticks) / ticksPerMs;
}

LocalTimeKind localToUtc(SYSTEMTIME local, DYNAMIC_TIME_ZONE_INFORMATION zone, Tick& result) {
    TIME_ZONE_INFORMATION rules{};
    Tick localTicks = 0;
    if (!toFileTicks(local, localTicks)
        || !GetTimeZoneInformationForYear(local.wYear, &zone, &rules)) return LocalTimeKind::error;
    const LONG standard = rules.Bias + rules.StandardBias;
    const LONG daylight = zone.DynamicDaylightTimeDisabled || rules.DaylightDate.wMonth == 0
        ? standard : rules.Bias + rules.DaylightBias;
    const LONG biases[]{standard, daylight};
    int matches = 0;
    for (int index = 0; index < 2; ++index) {
        if (index == 1 && daylight == standard) break;
        const Tick candidate = localTicks + biases[index] * 60 * ticksPerSecond;
        const auto utc = asSystemTime(candidate);
        SYSTEMTIME roundTrip{};
        Tick roundTripTicks = 0;
        if (!SystemTimeToTzSpecificLocalTimeEx(&zone, &utc, &roundTrip)
            || !toFileTicks(roundTrip, roundTripTicks)) return LocalTimeKind::error;
        if (roundTripTicks == localTicks) {
            result = candidate;
            ++matches;
        }
    }
    if (matches == 1) return LocalTimeKind::unique;
    return matches == 0 ? LocalTimeKind::nonexistent : LocalTimeKind::ambiguous;
}

const char* outcomeName(Outcome outcome) {
    switch (outcome) {
    case Outcome::sent: return "sent";
    case Outcome::cancelled: return "cancelled";
    case Outcome::invalidPoint: return "invalid_point";
    case Outcome::targetChanged: return "target_changed";
    case Outcome::partial: return "input_failed";
    default: return "error";
    }
}

bool Scheduler::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finished_ || cancelled_) return false;
    cancelled_ = true;
    clockChanged();
    return true;
}

void Scheduler::clockChanged() {
    if (changed_.value) SetEvent(changed_.value);
}

void Scheduler::setUnavailable(bool& flag, bool value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (flag == value) return;
    flag = value;
    clockChanged();
}

void Scheduler::setLocked(bool value) { setUnavailable(locked_, value); }
void Scheduler::setSuspended(bool value) { setUnavailable(suspended_, value); }

Result Scheduler::run(const Job& job) {
    const Tick spinBudget = std::max<Tick>(1, counterFrequency() * 2 / 1'000);
    auto finish = [this](Result result) {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
        return cancelled_ ? Result{Outcome::cancelled} : result;
    };
    Handle timer(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_MODIFY_STATE | SYNCHRONIZE));
    if (!timer.value || !changed_.value) {
        return finish({Outcome::error, {}, 0, GetLastError()});
    }
    StayAwake awake;
    SchedulingPriority priority;
    const HANDLE wake[]{changed_.value, timer.value};
    const auto waitUntil = [&](Tick utc) {
        LARGE_INTEGER due{};
        due.QuadPart = utc;
        if (!SetWaitableTimer(timer.value, &due, 0, nullptr, nullptr, FALSE)) return WAIT_FAILED;
        return WaitForMultipleObjects(2, wake, FALSE, INFINITE);
    };

    bool prepared = false;
    INPUT inputs[3]{};
    while (true) {
        const Tick now = utcNow();
        bool blocked = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled_) {
                finished_ = true;
                return {Outcome::cancelled};
            }
            blocked = locked_ || suspended_;
        }

        if (now >= job.target - prepareLead && (blocked || (!prepared && !activeSession()))) {
            // Session notifications wake us immediately. Poll as well for
            // desktop switches without a notification, such as a UAC prompt.
            if (WaitForSingleObject(changed_.value, 250) == WAIT_FAILED) {
                return finish({Outcome::error, {}, 0, GetLastError()});
            }
            prepared = false;
            continue;
        }
        if (!prepared && now >= job.target - prepareLead) {
            if (!prepareInput(job, inputs)) {
                return finish({Outcome::invalidPoint, {utcNow(), counterNow()}});
            }
            prepared = true;
        }

        const Tick remaining = job.target - utcNow();
        if (remaining > spinLead || !prepared) {
            const Tick wakeAt = job.target - (prepared ? spinLead : prepareLead);
            const DWORD status = waitUntil(wakeAt);
            if (status == WAIT_FAILED) return finish({Outcome::error, {}, 0, GetLastError()});
            if (status == WAIT_OBJECT_0) prepared = false;
            continue;
        }

        if (!interactiveDesktop()) {
            if (WaitForSingleObject(changed_.value, 250) == WAIT_FAILED) {
                return finish({Outcome::error, {}, 0, GetLastError()});
            }
            prepared = false;
            continue;
        }
        const Tick spinEnd = counterNow() + spinBudget;
        while (utcNow() < job.target && counterNow() < spinEnd) _mm_pause();

        const bool expectedWindow = !job.expectedWindow || WindowFromPoint(job.point) == job.expectedWindow;
        std::lock_guard<std::mutex> lock(mutex_);
        const ClockSample dispatch{utcNow(), counterNow()};
        if (cancelled_) {
            finished_ = true;
            return {Outcome::cancelled};
        }
        if (dispatch.utc < job.target || locked_ || suspended_) {
            prepared = false;
            continue;
        }
        finished_ = true;
        if (!expectedWindow) return {Outcome::targetChanged, dispatch};
        SetLastError(ERROR_SUCCESS);
        const UINT inserted = SendInput(3, inputs, sizeof(INPUT));
        const DWORD error = GetLastError();
        if (inserted == 2) SendInput(1, &inputs[2], sizeof(INPUT));
        return {inserted == 3 ? Outcome::sent : Outcome::partial, dispatch, inserted, error};
    }
}

} // namespace clickat
