#include "scheduler.h"
#include "version.h"

#include <cerrno>
#include <commctrl.h>
#include <cwchar>
#include <exception>
#include <memory>
#include <shellapi.h>
#include <string>
#include <thread>
#include <wtsapi32.h>

namespace {
using namespace clickat;

constexpr int timeId = 101, xId = 102, yId = 103;
constexpr int scheduleId = 105, cancelId = 106, moveId = 107, hotkeyId = 108, githubId = 109;
constexpr UINT completedMessage = WM_APP + 1;
constexpr int clientWidth = 472, clientHeight = 358;
constexpr DWORD windowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
constexpr wchar_t settingsKey[] = L"Software\\ClickAt";
constexpr wchar_t savedTimeName[] = L"Time";
constexpr wchar_t savedPointName[] = L"Point";
constexpr wchar_t repositoryUrl[] = L"https://github.com/vovainfo/ClickAt";

struct Control {
    HWND window;
    int x, y, width, height;
};

std::wstring textOf(HWND window) {
    wchar_t text[64]{};
    GetWindowTextW(window, text, ARRAYSIZE(text));
    return text;
}

bool parseCoordinate(HWND edit, LONG& value) {
    const auto text = textOf(edit);
    wchar_t* end = nullptr;
    errno = 0;
    const LONG parsed = wcstol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end || errno == ERANGE) return false;
    value = parsed;
    return true;
}

bool parseTime(const std::wstring& input, SYSTEMTIME& time) {
    if (input.size() != 8 || input[2] != L':' || input[5] != L':') return false;
    for (const int digit : {0, 1, 3, 4, 6, 7}) {
        if (input[digit] < L'0' || input[digit] > L'9') return false;
    }
    const auto twoDigits = [&](int pos) {
        return static_cast<WORD>(10 * (input[pos] - L'0') + input[pos + 1] - L'0');
    };
    time.wHour = twoDigits(0);
    time.wMinute = twoDigits(3);
    time.wSecond = twoDigits(6);
    time.wMilliseconds = 0;
    return time.wHour < 24 && time.wMinute < 60 && time.wSecond < 60;
}

std::wstring loadTime() {
    wchar_t value[9]{};
    DWORD bytes = sizeof(value);
    SYSTEMTIME time{};
    if (RegGetValueW(HKEY_CURRENT_USER, settingsKey, savedTimeName, RRF_RT_REG_SZ,
                     nullptr, value, &bytes) == ERROR_SUCCESS && parseTime(value, time)) {
        return value;
    }

    const SYSTEMTIME utc = asSystemTime(utcNow() + 15 * ticksPerSecond);
    if (!SystemTimeToTzSpecificLocalTimeEx(nullptr, &utc, &time)) return {};
    swprintf_s(value, L"%02u:%02u:%02u", time.wHour, time.wMinute, time.wSecond);
    return value;
}

std::wstring localLabel(const SYSTEMTIME& time) {
    wchar_t label[48]{};
    swprintf_s(label, L"%02u.%02u.%04u  %02u:%02u:%02u",
               time.wDay, time.wMonth, time.wYear, time.wHour, time.wMinute, time.wSecond);
    return label;
}

class App {
public:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        if (message == WM_NCCREATE) {
            const auto create = reinterpret_cast<const CREATESTRUCTW*>(lp);
            auto app = static_cast<App*>(create->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        const auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        return app ? app->message(message, wp, lp) : DefWindowProcW(window, message, wp, lp);
    }

private:
    HWND add(const wchar_t* cls, const wchar_t* title, DWORD style,
             int id, int x, int y, int width, int height) {
        const DWORD edge = wcscmp(cls, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0;
        const HWND control = CreateWindowExW(edge, cls, title, WS_CHILD | WS_VISIBLE | style,
                                             0, 0, 0, 0, window_,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                             GetModuleHandleW(nullptr), nullptr);
        controls_[controlCount_++] = {control, x, y, width, height};
        return control;
    }

    void resize(UINT dpi) {
        HFONT font = CreateFontW(-MulDiv(10, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
                                 FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        for (int i = 0; i < controlCount_; ++i) {
            const auto& item = controls_[i];
            const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
            MoveWindow(item.window, scale(item.x), scale(item.y),
                       scale(item.width), scale(item.height), TRUE);
            if (font) SendMessageW(item.window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        if (font) {
            if (font_) DeleteObject(font_);
            font_ = font;
        }
    }

    bool createControls() {
        constexpr DWORD editStyle = WS_TABSTOP | ES_AUTOHSCROLL;
        add(L"STATIC", L"Локальное время (ЧЧ:ММ:СС)", 0, 0, 20, 22, 246, 22);
        time_ = add(L"EDIT", L"", editStyle, timeId, 308, 18, 144, 26);
        add(L"STATIC", L"Координаты экрана", 0, 0, 20, 63, 170, 22);
        add(L"STATIC", L"X", 0, 0, 200, 63, 17, 22);
        x_ = add(L"EDIT", L"", editStyle, xId, 221, 59, 77, 26);
        add(L"STATIC", L"Y", 0, 0, 317, 63, 17, 22);
        y_ = add(L"EDIT", L"", editStyle, yId, 341, 59, 111, 26);
        const bool hotkeyRegistered = RegisterHotKey(window_, hotkeyId,
            MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F8) != FALSE;
        hotkeyRegistered_ = hotkeyRegistered;
        const wchar_t* instructions = hotkeyRegistered
            ? L"1. Укажите время.\r\n2. Наведите курсор на цель и нажмите Ctrl+Shift+F8.\r\n3. Проверьте дату и нажмите «Запланировать»."
            : L"1. Укажите время.\r\n2. Введите координаты X и Y. Горячая клавиша занята.\r\n3. Проверьте дату и нажмите «Запланировать».";
        add(L"STATIC", instructions, SS_NOPREFIX, 0, 20, 98, 432, 60);
        preview_ = add(L"STATIC", L"", SS_NOPREFIX, 0, 20, 163, 432, 36);
        add(L"BUTTON", L"Переместить в точку", WS_TABSTOP, moveId, 20, 201, 432, 32);
        schedule_ = add(L"BUTTON", L"Запланировать", WS_TABSTOP | BS_DEFPUSHBUTTON,
                        scheduleId, 20, 241, 206, 32);
        cancel_ = add(L"BUTTON", L"Отменить", WS_TABSTOP, cancelId, 246, 241, 206, 32);
        status_ = add(L"STATIC", L"", SS_NOPREFIX, 0, 20, 281, 432, 33);
        add(L"STATIC", L"© Владимир Гуменников  ·  Версия " CLICKAT_VERSION_TEXT,
            SS_NOPREFIX, 0, 20, 315, 432, 19);
        const auto link = std::wstring(L"<a href=\"") + repositoryUrl + L"\">" + repositoryUrl + L"</a>";
        add(WC_LINK, link.c_str(), WS_TABSTOP, githubId, 20, 334, 432, 20);
        for (int i = 0; i < controlCount_; ++i) if (!controls_[i].window) return false;
        SendMessageW(time_, EM_SETLIMITTEXT, 8, 0);
        SendMessageW(x_, EM_SETLIMITTEXT, 11, 0);
        SendMessageW(y_, EM_SETLIMITTEXT, 11, 0);
        sessionRegistered_ = WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION) != FALSE;
        SetWindowTextW(time_, loadTime().c_str());
        POINT point{};
        DWORD pointSize = sizeof(point);
        if (RegGetValueW(HKEY_CURRENT_USER, settingsKey, savedPointName, RRF_RT_REG_BINARY,
                         nullptr, &point, &pointSize) == ERROR_SUCCESS && pointSize == sizeof(point)) {
            setPointFields(point);
        }
        settingsReady_ = true;
        resize(GetDpiForWindow(window_));
        refreshPreview();
        SetFocus(time_);
        return true;
    }

    void refreshPreview() {
        const Tick now = utcNow();
        const SYSTEMTIME utc = asSystemTime(now);
        SYSTEMTIME local{};
        DYNAMIC_TIME_ZONE_INFORMATION zone{};
        target_ = 0;
        std::wstring label;
        if (GetDynamicTimeZoneInformation(&zone) == TIME_ZONE_ID_INVALID
            || !SystemTimeToTzSpecificLocalTimeEx(&zone, &utc, &local)) {
            label = L"Не удалось получить часовой пояс Windows.";
        } else if (!parseTime(textOf(time_), local)) {
            label = L"Введите время в формате ЧЧ:ММ:СС.";
        } else {
            auto kind = localToUtc(local, zone, target_);
            if (kind == LocalTimeKind::unique && target_ <= now) {
                FILETIME day{};
                SystemTimeToFileTime(&local, &day);
                local = asSystemTime(asTicks(day) + 24 * 60 * 60 * ticksPerSecond);
                kind = localToUtc(local, zone, target_);
            }
            switch (kind) {
            case LocalTimeKind::unique:
                label = L"Клик назначается на " + localLabel(local);
                break;
            case LocalTimeKind::ambiguous:
                label = L"Это время наступает дважды. Укажите другую секунду.";
                break;
            case LocalTimeKind::nonexistent:
                label = L"Это время пропущено при переводе часов.";
                break;
            default:
                label = L"Не удалось определить целевой момент.";
                break;
            }
            if (kind != LocalTimeKind::unique) target_ = 0;
        }
        SetWindowTextW(preview_, label.c_str());
        enableControls();
    }

    void enableControls() {
        const bool waiting = scheduler_ != nullptr;
        for (const HWND control : {time_, x_, y_}) EnableWindow(control, !waiting);
        EnableWindow(schedule_, !waiting && target_ != 0);
        EnableWindow(cancel_, waiting);
    }

    void setPointFields(const POINT& point) {
        const bool ready = settingsReady_;
        settingsReady_ = false;
        SetWindowTextW(x_, std::to_wstring(point.x).c_str());
        SetWindowTextW(y_, std::to_wstring(point.y).c_str());
        settingsReady_ = ready;
    }

    bool readPoint(POINT& point) const {
        return parseCoordinate(x_, point.x) && parseCoordinate(y_, point.y);
    }

    bool savePoint(const POINT& point) {
        if (RegSetKeyValueW(HKEY_CURRENT_USER, settingsKey, savedPointName, REG_BINARY,
                            &point, sizeof(point)) == ERROR_SUCCESS) return true;
        SetWindowTextW(status_, L"Не удалось сохранить координаты.");
        return false;
    }

    bool validatePoint(POINT& point) {
        if (!readPoint(point)) {
            SetWindowTextW(status_, L"Введите X и Y или выберите точку горячей клавишей.");
            return false;
        }
        if (!MonitorFromPoint(point, MONITOR_DEFAULTTONULL)) {
            SetWindowTextW(status_, L"Точка находится за пределами подключённых экранов.");
            return false;
        }
        return true;
    }

    void capturePoint() {
        POINT point{};
        if (scheduler_ || !GetCursorPos(&point)) return;
        setPointFields(point);
        if (savePoint(point)) SetWindowTextW(status_, L"Точка сохранена.");
    }

    void moveToPoint() {
        POINT point{}, actual{};
        if (!validatePoint(point)) return;
        if (!SetCursorPos(point.x, point.y) || !GetCursorPos(&actual)
            || actual.x != point.x || actual.y != point.y) {
            SetWindowTextW(status_, L"Не удалось переместить курсор в указанную точку.");
            return;
        }
        SetWindowTextW(status_, scheduler_ ? L"Курсор перемещён. Задание активно."
                                          : L"Курсор перемещён.");
    }

    void schedule() {
        if (scheduler_ || !target_) return;
        Job job{};
        if (!validatePoint(job.point)) return;
        if (target_ <= utcNow()) {
            refreshPreview();
            SetWindowTextW(status_, L"Время истекло. Проверьте новую дату и подтвердите её.");
            return;
        }

        job.target = target_;
        try {
            scheduler_ = std::make_unique<Scheduler>();
            scheduler_->setLocked(locked_);
            scheduler_->setSuspended(suspended_);
            worker_ = std::thread([this, job] {
                result_ = scheduler_->run(job);
                PostMessageW(window_, completedMessage, 0, 0);
            });
        } catch (const std::exception&) {
            scheduler_.reset();
            SetWindowTextW(status_, L"Не удалось запустить поток ожидания.");
            return;
        }
        SetWindowTextW(status_, L"Ожидание назначенного времени.");
        enableControls();
        SetFocus(cancel_);
    }

    void onCompletion() {
        worker_.join();
        scheduler_.reset();
        const double delay = milliseconds(result_.dispatch.utc - target_);
        wchar_t status[180]{};
        switch (result_.outcome) {
        case Outcome::sent:
            swprintf_s(status, L"Ввод отправлен. Опоздание отправки %.3f мс.", delay);
            break;
        case Outcome::cancelled:
            wcscpy_s(status, L"Задание отменено.");
            break;
        case Outcome::invalidPoint:
            wcscpy_s(status, L"Пропущено. Точка находится вне доступных экранов.");
            break;
        case Outcome::partial:
            swprintf_s(status, L"Windows вставила %u из 3 событий ввода. Код %lu.",
                       result_.inserted, result_.error);
            break;
        default:
            swprintf_s(status, L"Ошибка планировщика Windows. Код %lu.", result_.error);
            break;
        }
        SetWindowTextW(status_, status);
        enableControls();
        SetFocus(time_);
    }

    LRESULT message(UINT message, WPARAM wp, LPARAM lp) {
        switch (message) {
        case WM_CREATE: return createControls() ? 0 : -1;
        case WM_DPICHANGED: {
            const auto bounds = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(window_, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
                         bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
            resize(HIWORD(wp));
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == timeId && HIWORD(wp) == EN_CHANGE && settingsReady_ && !scheduler_) {
                refreshPreview();
                const auto value = textOf(time_);
                SYSTEMTIME parsed{};
                if (parseTime(value, parsed)
                    && RegSetKeyValueW(HKEY_CURRENT_USER, settingsKey, savedTimeName, REG_SZ, value.c_str(),
                                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
                    SetWindowTextW(status_, L"Не удалось сохранить время.");
                }
            }
            if ((LOWORD(wp) == xId || LOWORD(wp) == yId) && HIWORD(wp) == EN_CHANGE
                && settingsReady_ && !scheduler_) {
                POINT point{};
                if (readPoint(point)) savePoint(point);
            }
            if (LOWORD(wp) == moveId) moveToPoint();
            if (LOWORD(wp) == scheduleId) schedule();
            if (LOWORD(wp) == cancelId && scheduler_) scheduler_->cancel();
            return 0;
        case WM_NOTIFY: {
            const auto notification = reinterpret_cast<const NMHDR*>(lp);
            if (notification->idFrom == githubId
                && (notification->code == NM_CLICK || notification->code == NM_RETURN)) {
                ShellExecuteW(window_, L"open", repositoryUrl, nullptr, nullptr, SW_SHOWNORMAL);
            }
            return 0;
        }
        case WM_HOTKEY:
            if (wp == hotkeyId) capturePoint();
            return 0;
        case WM_TIMECHANGE:
            if (scheduler_) scheduler_->clockChanged();
            else if (preview_) refreshPreview();
            return 0;
        case WM_DISPLAYCHANGE:
            if (scheduler_) scheduler_->clockChanged();
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMSUSPEND || wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
                suspended_ = wp == PBT_APMSUSPEND;
                if (scheduler_) {
                    scheduler_->setSuspended(suspended_);
                    scheduler_->clockChanged();
                }
            }
            return TRUE;
        case WM_WTSSESSION_CHANGE:
            if (wp == WTS_SESSION_LOCK || wp == WTS_SESSION_UNLOCK) {
                locked_ = wp == WTS_SESSION_LOCK;
                if (scheduler_) {
                    scheduler_->setLocked(locked_);
                    SetWindowTextW(status_, locked_ ? L"Ожидание разблокировки рабочего стола."
                                                    : L"Задание активно.");
                }
            }
            return 0;
        case completedMessage:
            onCompletion();
            return 0;
        case WM_DESTROY:
            if (scheduler_) scheduler_->cancel();
            if (worker_.joinable()) worker_.join();
            if (hotkeyRegistered_) UnregisterHotKey(window_, hotkeyId);
            if (sessionRegistered_) WTSUnRegisterSessionNotification(window_);
            if (font_) DeleteObject(font_);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window_, message, wp, lp);
    }

    HWND window_ = nullptr, time_ = nullptr, x_ = nullptr, y_ = nullptr;
    HWND preview_ = nullptr, schedule_ = nullptr, cancel_ = nullptr, status_ = nullptr;
    HFONT font_ = nullptr;
    Control controls_[16]{};
    int controlCount_ = 0;
    Tick target_ = 0;
    bool hotkeyRegistered_ = false, sessionRegistered_ = false;
    bool locked_ = false, suspended_ = false, settingsReady_ = false;
    std::unique_ptr<Scheduler> scheduler_;
    std::thread worker_;
    Result result_{Outcome::error};
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    const INITCOMMONCONTROLSEX commonControls{sizeof(INITCOMMONCONTROLSEX), ICC_LINK_CLASS};
    if (!InitCommonControlsEx(&commonControls)) return 1;
    App app;
    WNDCLASSW wc{};
    wc.lpfnWndProc = App::windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"ClickAtWindow";
    if (!RegisterClassW(&wc)) return 1;

    const UINT dpi = GetDpiForSystem();
    const int dpiValue = static_cast<int>(dpi);
    RECT rect{0, 0, MulDiv(clientWidth, dpiValue, 96), MulDiv(clientHeight, dpiValue, 96)};
    AdjustWindowRectExForDpi(&rect, windowStyle, FALSE, 0, dpi);
    const HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"ClickAt " CLICKAT_VERSION_TEXT,
                                        windowStyle, CW_USEDEFAULT, CW_USEDEFAULT,
                                        rect.right - rect.left, rect.bottom - rect.top,
                                        nullptr, nullptr, instance, &app);
    if (!window) return 1;
    ShowWindow(window, show);
    MSG message{};
    BOOL status = 0;
    while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (status < 0) {
        DestroyWindow(window);
        return 1;
    }
    return 0;
}
