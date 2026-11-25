#include "scheduler.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <intrin.h>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include <windowsx.h>

namespace {
using namespace clickat;

constexpr int maxSamples = 20'000;
constexpr ULONG_PTR marker = 0xCA710000;
constexpr wchar_t receiverClass[] = L"ClickAtBenchmarkReceiver";
constexpr UINT stopReceiver = WM_APP + 1;

struct Reception {
    ClockSample first{};
    POINT point{};
    unsigned count = 0;
};

struct Shared {
    HWND window = nullptr;
    POINT target{};
    Reception receptions[maxSamples]{};
};

struct View {
    explicit View(HANDLE mapping)
        : shared(static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)))) {}
    ~View() { if (shared) UnmapViewOfFile(shared); }
    Shared* shared;
};

struct Trial {
    Tick target = 0;
    Result result{Outcome::error};
};

LRESULT CALLBACK receiverProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    } else if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN) {
        const ClockSample stamp{utcNow(), counterNow()};
        const auto cookie = static_cast<ULONG_PTR>(GetMessageExtraInfo());
        if (cookie >= marker && cookie < marker + maxSamples) {
            const auto shared = reinterpret_cast<Shared*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            auto& received = shared->receptions[cookie - marker];
            if (++received.count == 1) {
                received.first = stamp;
                received.point = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ClientToScreen(window, &received.point);
            }
        }
        return 0;
    } else if (message == WM_CLOSE || message == stopReceiver) {
        DestroyWindow(window);
        return 0;
    } else if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    } else if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT area{};
        GetClientRect(window, &area);
        DrawTextW(dc, L"ClickAt benchmark target", -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

int receive(HANDLE mapping, HANDLE ready) {
    const View view(mapping);
    if (!view.shared) return 1;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = receiverProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = receiverClass;
    if (!RegisterClassW(&wc)) return 1;
    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) return 1;
    const auto& work = monitorInfo.rcWork;
    const int width = 360, height = 180;
    const HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        receiverClass, L"ClickAt benchmark", WS_OVERLAPPED | WS_CAPTION,
        work.left + 20, work.top + 20,
        width, height, nullptr, nullptr, instance, view.shared);
    if (!window) return 1;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    RECT area{};
    GetClientRect(window, &area);
    view.shared->target = {area.right / 2, area.bottom / 2};
    ClientToScreen(window, &view.shared->target);
    view.shared->window = window;
    SetEvent(ready);

    MSG message{};
    BOOL status = 0;
    while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return status < 0 ? 1 : 0;
}

class CpuLoad {
public:
    ~CpuLoad() {
        stop_.store(true, std::memory_order_relaxed);
        for (auto& thread : threads_) thread.join();
    }
    void start() {
        const unsigned cores = std::max(std::thread::hardware_concurrency(), 1u);
        const unsigned count = cores > 1 ? cores - 1 : 1;
        for (unsigned i = 0; i < count; ++i) {
            threads_.emplace_back([this] {
                while (!stop_.load(std::memory_order_relaxed)) _mm_pause();
            });
        }
    }
private:
    std::atomic<bool> stop_{false};
    std::vector<std::thread> threads_;
};

std::mutex interruptMutex;
bool interrupted = false;
Scheduler* activeTrial = nullptr;

BOOL WINAPI consoleControl(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    std::lock_guard<std::mutex> lock(interruptMutex);
    interrupted = true;
    if (activeTrial) activeTrial->cancel();
    return TRUE;
}

double percentile(const std::vector<double>& sorted, double fraction) {
    const auto index = static_cast<size_t>(ceil(fraction * sorted.size())) - 1;
    return sorted[index];
}

bool report(const wchar_t* csvPath, const std::vector<Trial>& trials, const Shared& shared) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, csvPath, L"w") != 0) {
        fwprintf(stderr, L"Cannot write CSV: %ls\n", csvPath);
        return false;
    }
    fprintf(file, "trial,target_utc_filetime,outcome,events_inserted,win32_error,decision_utc_filetime,"
                  "decision_qpc,receive_utc_filetime,receive_qpc,decision_delta_ms,receive_delta_ms,"
                  "send_to_receive_qpc_ms,expected_x,expected_y,received_x,received_y,receptions,clock_step_suspected\n");
    const double qpcMs = 1000.0 / static_cast<double>(counterFrequency());
    size_t sent = 0, received = 0, duplicates = 0, wrongPoint = 0, onTime = 0;
    size_t buckets[5]{}, clockSteps = 0, early = 0;
    std::vector<double> latency;
    std::vector<double> dispatchLatency;
    for (size_t i = 0; i < trials.size(); ++i) {
        const auto& trial = trials[i];
        const auto& result = trial.result;
        const auto& sample = shared.receptions[i];
        const bool got = sample.count != 0;
        const bool correct = sample.point.x == shared.target.x && sample.point.y == shared.target.y;
        const double decision = milliseconds(result.dispatch.utc - trial.target);
        const double delay = milliseconds(sample.first.utc - trial.target);
        const double delivery = static_cast<double>(sample.first.counter - result.dispatch.counter) * qpcMs;
        const bool clockStep = got && result.dispatch.utc && fabs(delay - decision - delivery) > 1.0;
        clockSteps += clockStep ? 1 : 0;
        if (result.outcome == Outcome::sent) {
            ++sent;
            dispatchLatency.push_back(decision);
            if (decision < 0) ++early;
        }
        if (got) {
            ++received;
            duplicates += sample.count - 1;
            if (!correct) ++wrongPoint;
            if (!clockStep) {
                latency.push_back(delay);
                const int bucket = delay < 0 ? 0 : delay <= 5 ? 1 : delay <= 20 ? 2 : delay < 100 ? 3 : 4;
                ++buckets[bucket];
                if (bucket == 1 && correct && sample.count == 1 && result.outcome == Outcome::sent) ++onTime;
            }
        }
        fprintf(file, "%zu,%lld,%s,%u,%lu,", i + 1, trial.target, outcomeName(result.outcome),
                result.inserted, result.error);
        if (result.dispatch.utc) fprintf(file, "%lld,%lld,", result.dispatch.utc, result.dispatch.counter);
        else fprintf(file, ",,");
        if (got) fprintf(file, "%lld,%lld,", sample.first.utc, sample.first.counter);
        else fprintf(file, ",,");
        if (result.dispatch.utc) fprintf(file, "%.4f,", decision);
        else fprintf(file, ",");
        if (got) fprintf(file, "%.4f,", delay);
        else fprintf(file, ",");
        if (got && result.dispatch.utc) fprintf(file, "%.4f,", delivery);
        else fprintf(file, ",");
        fprintf(file, "%ld,%ld,", shared.target.x, shared.target.y);
        if (got) fprintf(file, "%ld,%ld,", sample.point.x, sample.point.y);
        else fprintf(file, ",,");
        fprintf(file, "%u,%d\n", sample.count, clockStep ? 1 : 0);
    }
    const bool written = fclose(file) == 0;
    if (!written) fwprintf(stderr, L"Error saving CSV: %ls\n", csvPath);
    printf("Scheduled %zu, sent %zu, received %zu, missed %zu, duplicates %zu, wrong point %zu\n",
           trials.size(), sent, received, trials.size() - received, duplicates, wrongPoint);
    printf("Correct and on time (0-5 ms): %zu/%zu\n", onTime, trials.size());
    printf("Delivery buckets <0 / 0-5 / >5-20 / >20-<100 / >=100 ms: %zu / %zu / %zu / %zu / %zu\n",
           buckets[0], buckets[1], buckets[2], buckets[3], buckets[4]);
    printf("Possible UTC clock step during delivery: %zu trials\n", clockSteps);
    printf("Early dispatches: %zu\n", early);
    if (!dispatchLatency.empty()) {
        std::sort(dispatchLatency.begin(), dispatchLatency.end());
        printf("Target-to-dispatch median %.3f ms, p99 %.3f ms, maximum %.3f ms\n",
               percentile(dispatchLatency, 0.5), percentile(dispatchLatency, 0.99), dispatchLatency.back());
    }
    if (!latency.empty()) {
        std::sort(latency.begin(), latency.end());
        printf("Target-to-receive median %.3f ms, p99 %.3f ms, maximum %.3f ms\n",
               percentile(latency, 0.5), percentile(latency, 0.99), latency.back());
    }
    return written;
}

unsigned long long argumentNumber(const wchar_t* arg, unsigned long long maximum) {
    if (!arg[0]) return 0;
    for (const wchar_t* p = arg; *p; ++p) if (*p < L'0' || *p > L'9') return 0;
    const auto value = wcstoull(arg, nullptr, 10);
    return value <= maximum ? value : 0;
}

int selfTest() {
    int errors = 0;
    const auto check = [&](bool condition, const char* label) {
        printf("%s %s\n", condition ? "PASS" : "FAIL", label);
        if (!condition) ++errors;
    };
    DYNAMIC_TIME_ZONE_INFORMATION zone{};
    for (DWORD index = 0; EnumDynamicTimeZoneInformation(index, &zone) == ERROR_SUCCESS; ++index) {
        if (wcscmp(zone.TimeZoneKeyName, L"Pacific Standard Time") == 0) break;
    }
    Tick target = 0;
    SYSTEMTIME local{2026, 3, 0, 8, 2, 30, 0, 0};
    check(localToUtc(local, zone, target) == LocalTimeKind::nonexistent, "DST gap");
    local = {2026, 11, 0, 1, 1, 30, 0, 0};
    check(localToUtc(local, zone, target) == LocalTimeKind::ambiguous, "DST repeat");
    local.wHour = 3;
    check(localToUtc(local, zone, target) == LocalTimeKind::unique
          && asSystemTime(target).wHour == 11, "standard time conversion");

    Job job{};
    job.target = utcNow() + 5 * ticksPerSecond;
    Scheduler cancelled;
    Result result{Outcome::error};
    std::thread worker([&] { result = cancelled.run(job); });
    Sleep(15);
    const bool accepted = cancelled.cancel();
    worker.join();
    check(accepted && result.outcome == Outcome::cancelled, "cancel wakes waiting worker");

    Scheduler locked;
    locked.setLocked(true);
    job.target = utcNow() + 30 * ticksPerMs;
    std::thread blocked([&] { result = locked.run(job); });
    while (utcNow() < job.target + 120 * ticksPerMs) Sleep(10);
    const bool cancelledAfterTarget = locked.cancel();
    blocked.join();
    check(cancelledAfterTarget && result.outcome == Outcome::cancelled,
          "locked job remains pending beyond target");
    return errors == 0 ? 0 : 1;
}

int benchmark(int count, const wchar_t* csv, int interval, bool underLoad) {
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, &inherit, PAGE_READWRITE, 0,
                                           sizeof(Shared), nullptr));
    const Handle ready(CreateEventW(&inherit, TRUE, FALSE, nullptr));
    if (!mapping.value || !ready.value) return 1;
    const View view(mapping.value);
    if (!view.shared) return 1;
    new (view.shared) Shared{};

    wchar_t filename[32'768]{};
    const auto length = GetModuleFileNameW(nullptr, filename, ARRAYSIZE(filename));
    if (!length || length >= ARRAYSIZE(filename)) return 1;
    std::wstring command = L"\"" + std::wstring(filename) + L"\" --receiver "
        + std::to_wstring(reinterpret_cast<UINT_PTR>(mapping.value)) + L" "
        + std::to_wstring(reinterpret_cast<UINT_PTR>(ready.value));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(filename, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &child)) {
        fprintf(stderr, "Cannot start receiver (Windows error %lu).\n", GetLastError());
        return 1;
    }
    const Handle process(child.hProcess), thread(child.hThread);
    const HANDLE startupWait[]{ready.value, process.value};
    if (WaitForMultipleObjects(2, startupWait, FALSE, 5000) != WAIT_OBJECT_0) {
        if (view.shared->window) PostMessageW(view.shared->window, stopReceiver, 0, 0);
        fprintf(stderr, "Receiver could not create an interactive window.\n");
        return 1;
    }

    int result = 1;
    try {
        std::vector<Trial> trials;
        trials.reserve(count);
        SetConsoleCtrlHandler(consoleControl, TRUE);
        printf("Running %d scheduled clicks at %d ms cadence%s. Keep the target visible.\n",
               count, interval, underLoad ? " under CPU load" : "");
        fflush(stdout);
        {
            CpuLoad load;
            if (underLoad) load.start();
            Tick target = utcNow() + 150 * ticksPerMs;
            if (interval % 1000 == 0) {
                target = ((target + ticksPerSecond - 1) / ticksPerSecond) * ticksPerSecond;
            }
            for (int i = 0; i < count; ++i) {
                Scheduler scheduler;
                const Job job{target, view.shared->target,
                              marker + static_cast<ULONG_PTR>(i), view.shared->window};
                {
                    std::lock_guard<std::mutex> lock(interruptMutex);
                    if (interrupted) break;
                    activeTrial = &scheduler;
                }
                const Result fired = scheduler.run(job);
                {
                    std::lock_guard<std::mutex> lock(interruptMutex);
                    activeTrial = nullptr;
                }
                trials.push_back({target, fired});
                if (fired.outcome == Outcome::cancelled || fired.outcome == Outcome::targetChanged
                    || fired.outcome == Outcome::error || fired.outcome == Outcome::invalidPoint) {
                    printf("Trial %d stopped the run: %s (Windows error %lu).\n",
                           i + 1, outcomeName(fired.outcome), fired.error);
                    break;
                }
                target += interval * ticksPerMs;
                const Tick earliest = utcNow() + 20 * ticksPerMs;
                if (target < earliest) {
                    const Tick cadence = interval * ticksPerMs;
                    target += ((earliest - target + cadence - 1) / cadence) * cadence;
                }
            }
            Sleep(150);
        }
        PostMessageW(view.shared->window, stopReceiver, 0, 0);
        if (WaitForSingleObject(process.value, 2000) == WAIT_OBJECT_0) {
            const bool saved = report(csv, trials, *view.shared);
            const bool finished = trials.size() == static_cast<size_t>(count);
            const bool exercised = std::any_of(trials.begin(), trials.end(), [](const Trial& trial) {
                return trial.result.outcome == Outcome::sent;
            });
            result = saved && finished && exercised ? 0 : 1;
        } else {
            fprintf(stderr, "Receiver did not shut down.\n");
        }
    } catch (const std::exception& exception) {
        fprintf(stderr, "Benchmark error: %s\n", exception.what());
        PostMessageW(view.shared->window, stopReceiver, 0, 0);
    }
    SetConsoleCtrlHandler(consoleControl, FALSE);
    return result;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && wcscmp(argv[1], L"--receiver") == 0) {
        return receive(reinterpret_cast<HANDLE>(argumentNumber(argv[2], UINTPTR_MAX)),
                       reinterpret_cast<HANDLE>(argumentNumber(argv[3], UINTPTR_MAX)));
    }
    if (argc == 2 && wcscmp(argv[1], L"--self-test") == 0) return selfTest();
    const auto count = argc >= 3 ? argumentNumber(argv[1], maxSamples) : 0;
    const auto interval = argc >= 4 ? argumentNumber(argv[3], 28'800'000) : 35;
    const bool load = argc == 5 && wcscmp(argv[4], L"--load") == 0;
    if (count && interval && (argc == 3 || argc == 4 || load)) {
        return benchmark(static_cast<int>(count), argv[2], static_cast<int>(interval), load);
    }
    fprintf(stderr, "Usage: ClickAtBenchmark COUNT REPORT.csv [INTERVAL_MS [--load]]\n"
                    "       ClickAtBenchmark --self-test\n");
    return 2;
}
