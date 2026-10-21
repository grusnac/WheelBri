// The `--ui` window on Windows: a plain Win32 window drawn with GDI. Nothing to ship.
//
// Each frame is recorded, then drawn into a memory bitmap the size of the window (the logical canvas scaled to it,
// UiWindow.hpp fitCanvas) and copied to the screen in one go, so it never flickers; a frame like the one shown is
// not drawn again. The window is DPI aware: on a 150 % display it
// is drawn at full resolution instead of being blown up blurry by Windows.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "UiWindow.hpp"

#include "wheelbri/DrawList.hpp"
#include "wheelbri/Logging.hpp"

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace wheelbri::service::ui {

namespace {

constexpr wchar_t kClassName[] = L"wheelbri-ui";
// The text's font: Cascadia Mono (Windows Terminal's, with Windows 11) for its larger, clearer letters; Consolas
// where it is not installed.
constexpr wchar_t kFontFace[] = L"Cascadia Mono";
constexpr wchar_t kFallbackFace[] = L"Consolas";
// The letters' size (em) in logical pixels: capitals fill the 8-pixel line, descenders hang below it.
constexpr float kFontEm = 11.5F;

std::string hex(const HRESULT value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08lX", static_cast<unsigned long>(value));
    return text;
}

std::wstring widen(const std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {   // not UTF-8: take the bytes as they are (ANSI code page)
        std::wstring out(text.size(), L'?');
        for (std::size_t i = 0; i < text.size(); ++i) out[i] = static_cast<unsigned char>(text[i]);
        return out;
    }
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

// `text` into a fixed WCHAR array of a Windows struct, cut to fit (a cut word keeps "..." at its end) and ended.
template <std::size_t N>
void copyCut(WCHAR (&out)[N], const std::wstring_view text) {
    static_assert(N > 4);
    std::size_t n = std::min(text.size(), N - 1);
    for (std::size_t i = 0; i < n; ++i) out[i] = text[i];
    if (text.size() > N - 1) {
        for (std::size_t i = N - 4; i < N - 1; ++i) out[i] = L'.';
        n = N - 1;
    }
    out[n] = L'\0';
}

constexpr UINT kTrayId = 1;   // WheelBri's one icon in the notification area (notify)

std::string lastError(const char* what) {
    return std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")";
}

// Per-monitor DPI awareness (Windows 10 1703+), else system DPI awareness (Vista+). Looked up at run time
// so the program still starts on older Windows.
void becomeDpiAware() {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) {
        return;
    }
    using SetContext = BOOL(WINAPI*)(HANDLE);
    const auto setContext =
        reinterpret_cast<SetContext>(reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
    const auto perMonitorV2 = reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4));   // DPI_AWARENESS_CONTEXT_..._V2
    if (setContext != nullptr && setContext(perMonitorV2) != FALSE) {
        return;
    }
    SetProcessDPIAware();
}

UINT windowDpi(const HWND window) {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    using GetDpi = UINT(WINAPI*)(HWND);
    const auto getDpi =
        user32 != nullptr ? reinterpret_cast<GetDpi>(reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow")))
                          : nullptr;
    if (getDpi != nullptr && window != nullptr) {
        return getDpi(window);
    }
    const HDC screen = GetDC(nullptr);
    const int dpi = screen != nullptr ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen != nullptr) ReleaseDC(nullptr, screen);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96U;
}

class Win32Window final : public UiWindow {
public:
    Win32Window(const int logicalWidth, const int logicalHeight)
        : logicalWidth_(logicalWidth), logicalHeight_(logicalHeight) {}

    ~Win32Window() override {
        if (trayIcon_) {
            NOTIFYICONDATAW data{};
            data.cbSize = sizeof(data);
            data.hWnd = window_;
            data.uID = kTrayId;
            Shell_NotifyIconW(NIM_DELETE, &data);
        }
        releaseBuffer();
        if (font_ != nullptr) DeleteObject(font_);
        if (window_ != nullptr) {
            SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
            DestroyWindow(window_);
        }
    }

    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    bool create(const std::string& title, const int windowWidth, const int windowHeight, std::string& error) {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.style = CS_HREDRAW | CS_VREDRAW;
        cls.lpfnWndProc = &Win32Window::procedure;
        cls.hInstance = instance;
        cls.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        // WheelBri's icon (resource 1 of the program, app/wheelbri.ico), in the sizes Windows asks for; the generic one
        // if the program has none.
        cls.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                                  GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR | LR_SHARED));
        cls.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                                    LR_DEFAULTCOLOR | LR_SHARED));
        if (cls.hIcon == nullptr) {
            cls.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
        }
        cls.lpszClassName = kClassName;
        if (RegisterClassExW(&cls) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            error = lastError("RegisterClassEx");
            return false;
        }

        // The size asked for at 100 %, times the display scale, made smaller (same shape) if the screen is.
        const DWORD style = WS_OVERLAPPEDWINDOW;
        const double scale = static_cast<double>(windowDpi(nullptr)) / 96.0;
        RECT work{0, 0, 1920, 1040};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        RECT frame{0, 0, 0, 0};
        AdjustWindowRectEx(&frame, style, FALSE, 0);
        const int frameW = frame.right - frame.left;
        const int frameH = frame.bottom - frame.top;
        double clientW = windowWidth * scale;
        double clientH = windowHeight * scale;
        const double fit = std::min({1.0, static_cast<double>(work.right - work.left - frameW) / clientW,
                                     static_cast<double>(work.bottom - work.top - frameH) / clientH});
        clientW *= fit;
        clientH *= fit;
        const int outerW = static_cast<int>(clientW) + frameW;
        const int outerH = static_cast<int>(clientH) + frameH;
        const int workW = static_cast<int>(work.right - work.left);
        const int workH = static_cast<int>(work.bottom - work.top);
        const int x = static_cast<int>(work.left) + std::max(0, (workW - outerW) / 2);
        const int y = static_cast<int>(work.top) + std::max(0, (workH - outerH) / 2);

        window_ = CreateWindowExW(0, kClassName, widen(title).c_str(), style, x, y, outerW, outerH, nullptr, nullptr,
                                  instance, this);
        if (window_ == nullptr) {
            error = lastError("CreateWindowEx");
            return false;
        }
        refit();
        ShowWindow(window_, SW_SHOWNORMAL);
        UpdateWindow(window_);
        return true;
    }

    WindowInput pump() override {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        refit();   // a resize: the views lay out for the new canvas from this frame's input on
        WindowInput input;
        input.close = closed_;
        input.keys.swap(keys_);
        input.text.swap(text_);
        input.clicks.swap(clicks_);
        input.wheel = wheelDelta_ / WHEEL_DELTA;
        wheelDelta_ %= WHEEL_DELTA;   // a fine-grained wheel keeps its remainder for the next notch
        input.hwheel = hwheelDelta_ / WHEEL_DELTA;
        hwheelDelta_ %= WHEEL_DELTA;
        input.pointer = pointer_;
        input.leftDown = leftDown_;
        input.pointerInside = inside_;
        return input;
    }

    [[nodiscard]] bool visible() const override {
        return window_ != nullptr && IsWindowVisible(window_) != FALSE && IsIconic(window_) == FALSE;
    }

    // A frame is recorded (core DrawList) and drawn in present() only when it differs from the one on screen, or the
    // window's size or scale changed: an unchanged window costs no GDI work.
    void begin(const Colour background) override {
        refit();
        frame_.begin(rgbOf(background));
    }

    void fill(const float x, const float y, const float w, const float h, const Colour colour) override {
        if (w <= 0.0F || h <= 0.0F) {
            return;
        }
        frame_.fill(x + originX_, y + originY_, w, h, rgbOf(colour));   // recorded on the canvas, origin applied
    }

    void outline(const float x, const float y, const float w, const float h, const Colour colour) override {
        const float line = 1.0F / box_.scale * static_cast<float>(std::max(1L, std::lround(box_.scale)));
        fill(x, y, w, line, colour);
        fill(x, y + h - line, w, line, colour);
        fill(x, y, line, h, colour);
        fill(x + w - line, y, line, h, colour);
    }

    void text(const float x, const float y, const std::string_view value, const Colour colour) override {
        if (value.empty()) {
            return;
        }
        frame_.text(x + originX_, y + originY_, value, rgbOf(colour));
    }

    void present() override {
        const bool sameSize = buffer_ != nullptr && frameWidth_ == bufferWidth_ && frameHeight_ == bufferHeight_;
        if (sameSize && frame_.sameAs(shown_)) {
            return;   // what is on screen already (WM_PAINT repaints it from the buffer when needed)
        }
        if (!sameSize) {
            makeBuffer(frameWidth_, frameHeight_);
        }
        if (buffer_ == nullptr) {
            return;   // Windows is out of drawing memory: nothing this frame, again next frame
        }
        updateFont();
        SetDCBrushColor(buffer_, rgbRef(frame_.background()));
        const RECT all{0, 0, bufferWidth_, bufferHeight_};
        FillRect(buffer_, &all, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        for (const DrawList::Command& command : frame_.commands()) {
            if (command.kind == DrawList::Command::Kind::Fill) {
                drawFill(command);
            } else {
                drawText(command);
            }
        }
        const HDC screen = GetDC(window_);
        if (screen != nullptr) {
            BitBlt(screen, 0, 0, bufferWidth_, bufferHeight_, buffer_, 0, 0, SRCCOPY);
            ReleaseDC(window_, screen);
        }
        frame_.swap(shown_);
    }

    bool openFolder(const std::filesystem::path& folder, std::string& error) override {
        const std::wstring arguments = L"\"" + folder.wstring() + L"\"";
        const auto result = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL));
        if (result > 32) {
            return true;
        }
        error = "ShellExecute error " + std::to_string(result);
        return false;
    }

    bool pickFolder(const std::string& title, std::filesystem::path& folder, std::string& error) override {
        // The Vista+ folder window (IFileOpenDialog, FOS_PICKFOLDERS), modal over this window.
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const bool uninit = SUCCEEDED(init);   // S_FALSE too: already initialised on this thread, still balanced
        const auto finish = [&](const bool chosen) {
            if (uninit) CoUninitialize();
            return chosen;
        };
        IFileOpenDialog* dialog = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        if (FAILED(hr)) {
            error = "cannot open the folder window (error 0x" + hex(hr) + ")";
            return finish(false);
        }
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(widen(title).c_str());
        if (!folder.empty()) {
            IShellItem* start = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(folder.wstring().c_str(), nullptr, IID_PPV_ARGS(&start)))) {
                dialog->SetFolder(start);
                start->Release();
            }
        }
        hr = dialog->Show(window_);
        bool chosen = false;
        if (SUCCEEDED(hr)) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                    folder = std::filesystem::path(std::wstring(path));
                    CoTaskMemFree(path);
                    chosen = true;
                }
                item->Release();
            }
        } else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            error = "the folder window failed (error 0x" + hex(hr) + ")";
        }
        dialog->Release();
        return finish(chosen);
    }

    // WheelBri's icon in the notification area, added with the first notification and there until the window goes (a
    // toast whose icon is gone may vanish from the notification centre); each notification is a balloon on it.
    bool notify(const std::string& title, const std::string& text) override {
        if (window_ == nullptr) return false;
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = kTrayId;
        data.uFlags = NIF_ICON | NIF_TIP | NIF_INFO;
        data.hIcon = reinterpret_cast<HICON>(GetClassLongPtrW(window_, GCLP_HICONSM));
        if (data.hIcon == nullptr) data.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
        copyCut(data.szTip, L"WheelBri");
        copyCut(data.szInfoTitle, widen(title));
        copyCut(data.szInfo, widen(text));
        data.dwInfoFlags = NIIF_WARNING;
        if (!trayIcon_) {
            if (Shell_NotifyIconW(NIM_ADD, &data) == FALSE) {
                WE_LOG_WARN(logging::get("service.ui"), "notification: the notification area refused WheelBri's icon");
                return false;
            }
            trayIcon_ = true;
        } else if (Shell_NotifyIconW(NIM_MODIFY, &data) == FALSE) {
            WE_LOG_WARN(logging::get("service.ui"), "notification: not shown by Windows");
            return false;
        }
        return true;
    }

    [[nodiscard]] std::string describe() const override {
        return "Win32 GDI, " + std::to_string(windowDpi(window_) * 100 / 96) + " % display scale";
    }

private:
    static LRESULT CALLBACK procedure(const HWND hwnd, const UINT message, const WPARAM wParam, const LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self != nullptr) {
            // An exception must not unwind through Windows' code (DispatchMessage): that ends the process without its
            // clean-up, and here it may be the logoff handler stopping the session. Logged; Windows' default follows.
            try {
                LRESULT result = 0;
                if (self->handle(hwnd, message, wParam, lParam, result)) {
                    return result;
                }
            } catch (const std::exception& e) {
                static logging::RateLimiter limiter(std::chrono::seconds(10));
                std::uint64_t suppressed = 0;
                if (limiter.allow(suppressed)) {
                    WE_LOG_ERROR(logging::get("service.ui"), "window message 0x{:04x}: {} ({} similar suppressed)",
                                 message, e.what(), suppressed);
                }
            } catch (...) {
                WE_LOG_ERROR(logging::get("service.ui"), "window message 0x{:04x}: unknown error", message);
            }
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool handle(const HWND hwnd, const UINT message, const WPARAM wParam, const LPARAM lParam, LRESULT& result) {
        switch (message) {
            case WM_CLOSE:   // the owner destroys the window; here it only says so
                closed_ = true;
                return true;
            case WM_QUERYENDSESSION:   // logoff or shutdown asks: yes (the clean-up runs on WM_ENDSESSION)
                if (queryEndSession_) queryEndSession_();
                result = TRUE;
                return true;
            case WM_ENDSESSION:
                if (wParam != FALSE && endSession_) {
                    closed_ = true;
                    endSession_();   // the process ends soon after this returns
                }
                result = 0;
                return true;
            case WM_KEYDOWN:
                if (wParam == VK_ESCAPE) {
                    keys_ += kEscape;
                } else if (wParam == VK_PRIOR || wParam == VK_NEXT || wParam == VK_HOME || wParam == VK_END ||
                           wParam == VK_UP || wParam == VK_DOWN || wParam == VK_LEFT ||
                           wParam == VK_RIGHT) {   // repeats too: holding scrolls on
                    keys_ += wParam == VK_PRIOR  ? kPageUp
                             : wParam == VK_NEXT ? kPageDown
                             : wParam == VK_HOME ? kHome
                             : wParam == VK_END  ? kEnd
                             : wParam == VK_UP   ? kUp
                             : wParam == VK_DOWN ? kDown
                             : wParam == VK_LEFT ? kLeft
                                                 : kRight;
                } else if (wParam >= 'A' && wParam <= 'Z' && (lParam & (1L << 30)) == 0) {   // not a repeat
                    keys_ += static_cast<char>('a' + (wParam - 'A'));
                }
                return true;
            case WM_CHAR:   // after TranslateMessage: the character typed, with Shift and the keyboard layout applied
                if (wParam == VK_BACK) {
                    text_ += kBackspace;
                } else if (wParam == VK_RETURN) {
                    text_ += kEnter;
                } else if (wParam >= 32 && wParam < 127) {
                    text_ += static_cast<char>(wParam);
                }
                return true;
            case WM_MOUSEWHEEL:
                if ((GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0) {
                    hwheelDelta_ -= GET_WHEEL_DELTA_WPARAM(wParam);   // Shift + wheel: sideways (away = left)
                } else {
                    wheelDelta_ += GET_WHEEL_DELTA_WPARAM(wParam);
                }
                return true;
            case 0x020E:   // WM_MOUSEHWHEEL: a tilt wheel or a touchpad's sideways swipe (> 0 to the right)
                hwheelDelta_ += GET_WHEEL_DELTA_WPARAM(wParam);
                return true;
            case WM_LBUTTONDOWN:
                pointer_ = toCanvas(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
                clicks_.push_back(pointer_);
                leftDown_ = true;
                SetCapture(hwnd);   // the drag goes on outside the window
                return true;
            case WM_MOUSEMOVE:
                pointer_ = toCanvas(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
                if (!inside_) {
                    // Asks for one WM_MOUSELEAVE when the mouse leaves (then asked again on the next move in).
                    TRACKMOUSEEVENT track{};
                    track.cbSize = sizeof(track);
                    track.dwFlags = TME_LEAVE;
                    track.hwndTrack = hwnd;
                    inside_ = TrackMouseEvent(&track) != FALSE;
                }
                return true;
            case WM_MOUSELEAVE:
                inside_ = false;
                return true;
            case WM_LBUTTONUP:
                pointer_ = toCanvas(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
                leftDown_ = false;
                ReleaseCapture();
                return true;
            case WM_CAPTURECHANGED:   // another window took the mouse: the drag ends
                leftDown_ = false;
                return true;
            case WM_ERASEBKGND:   // everything is painted from the buffer: no flicker
                result = 1;
                return true;
            case WM_PAINT: {
                PAINTSTRUCT paint;
                const HDC dc = BeginPaint(hwnd, &paint);
                if (buffer_ != nullptr) {
                    BitBlt(dc, 0, 0, bufferWidth_, bufferHeight_, buffer_, 0, 0, SRCCOPY);
                }
                EndPaint(hwnd, &paint);
                return true;
            }
            case 0x02E0: {   // WM_DPICHANGED: moved to a screen with another scale; take the size Windows suggests
                const auto* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                             suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
                return true;
            }
            case WM_GETMINMAXINFO: {
                // At least kMinZoom times the design (at this display scale), at most the screen the window is on
                // (its work area: not over the taskbar); maximised, exactly that.
                auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
                MONITORINFO monitor{};
                monitor.cbSize = sizeof(monitor);
                RECT work{0, 0, 1920, 1040};
                RECT whole = work;
                if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE) {
                    work = monitor.rcWork;
                    whole = monitor.rcMonitor;
                }
                const LONG workW = work.right - work.left;
                const LONG workH = work.bottom - work.top;
                RECT frame{0, 0, 0, 0};
                AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0);
                const double scale = static_cast<double>(windowDpi(hwnd)) / 96.0 * kMinZoom;
                const LONG minW = std::lround(logicalWidth_ * scale) + (frame.right - frame.left);
                const LONG minH = std::lround(logicalHeight_ * scale) + (frame.bottom - frame.top);
                info->ptMinTrackSize.x = std::min(minW, workW);
                info->ptMinTrackSize.y = std::min(minH, workH);
                info->ptMaxTrackSize.x = workW;
                info->ptMaxTrackSize.y = workH;
                info->ptMaxSize.x = workW;
                info->ptMaxSize.y = workH;
                info->ptMaxPosition.x = work.left - whole.left;
                info->ptMaxPosition.y = work.top - whole.top;
                return true;
            }
            default:
                return false;
        }
    }

    // The canvas for the window's size now: the design scaled up to kMaxZoom times the display scale, the rest as room
    // (fitCanvas); the views read it through width() and height().
    void refit() {
        if (window_ == nullptr) return;
        RECT client{};
        GetClientRect(window_, &client);
        frameWidth_ = std::max(1, static_cast<int>(client.right - client.left));
        frameHeight_ = std::max(1, static_cast<int>(client.bottom - client.top));
        const float maxScale = static_cast<float>(windowDpi(window_)) / 96.0F * kMaxZoom;
        box_ = fitCanvas(frameWidth_, frameHeight_, logicalWidth_, logicalHeight_, maxScale);
        canvasWidth_ = box_.width;
        canvasHeight_ = box_.height;
    }

    // Window pixels -> logical canvas pixels.
    [[nodiscard]] Click toCanvas(const int x, const int y) const {
        const float scale = box_.scale > 0.0F ? box_.scale : 1.0F;
        return {(static_cast<float>(x) - box_.offsetX) / scale, (static_cast<float>(y) - box_.offsetY) / scale};
    }

    // A recorded canvas position (origin already applied) -> window pixels.
    [[nodiscard]] int canvasX(const float x) const { return static_cast<int>(std::lround(box_.offsetX + x * box_.scale)); }
    [[nodiscard]] int canvasY(const float y) const { return static_cast<int>(std::lround(box_.offsetY + y * box_.scale)); }

    static DrawList::Rgb rgbOf(const Colour c) { return {c.r, c.g, c.b}; }
    static COLORREF rgbRef(const DrawList::Rgb c) { return RGB(c[0], c[1], c[2]); }

    void drawFill(const DrawList::Command& c) {
        RECT rect{canvasX(c.x), canvasY(c.y), canvasX(c.x + c.w), canvasY(c.y + c.h)};
        if (rect.right <= rect.left) rect.right = rect.left + 1;   // a thin line never disappears
        if (rect.bottom <= rect.top) rect.bottom = rect.top + 1;
        SetDCBrushColor(buffer_, rgbRef(c.colour));
        FillRect(buffer_, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }

    void drawText(const DrawList::Command& c) {
        const std::wstring wide = widen(c.text);
        if (wide.empty()) {
            return;
        }
        // Every character on the 8-pixel grid, exactly where the layout put it.
        advances_.resize(wide.size());
        for (std::size_t i = 0; i < wide.size(); ++i) {
            const float left = c.x + static_cast<float>(i) * kCharWidth;
            advances_[i] = canvasX(left + kCharWidth) - canvasX(left);
        }
        SetTextColor(buffer_, rgbRef(c.colour));
        // On the baseline at the bottom of the 8-pixel line the layout put it on: the capitals fill the line,
        // descenders hang below (rows are 11..16 apart, so they still don't touch).
        ExtTextOutW(buffer_, canvasX(c.x), canvasY(c.y + kCharHeight), 0, nullptr, wide.c_str(),
                    static_cast<UINT>(wide.size()), advances_.data());
    }

    void makeBuffer(const int width, const int height) {
        releaseBuffer();
        const HDC screen = GetDC(window_);
        buffer_ = CreateCompatibleDC(screen);
        bitmap_ = CreateCompatibleBitmap(screen, width, height);
        ReleaseDC(window_, screen);
        if (buffer_ == nullptr || bitmap_ == nullptr) {
            // Out of GDI memory (a huge window, or a leak elsewhere): no buffer; begin() tries again next frame.
            if (buffer_ != nullptr) DeleteDC(buffer_);
            if (bitmap_ != nullptr) DeleteObject(bitmap_);
            buffer_ = nullptr;
            bitmap_ = nullptr;
            bufferWidth_ = 0;
            bufferHeight_ = 0;
            return;
        }
        previousBitmap_ = SelectObject(buffer_, bitmap_);
        SetBkMode(buffer_, TRANSPARENT);
        SetTextAlign(buffer_, TA_BASELINE | TA_LEFT | TA_NOUPDATECP);
        bufferWidth_ = width;
        bufferHeight_ = height;
        fontScale_ = -1.0F;   // select the font into the new DC
    }

    void releaseBuffer() {
        if (buffer_ != nullptr) {
            if (previousFont_ != nullptr) SelectObject(buffer_, previousFont_);
            SelectObject(buffer_, previousBitmap_);
            DeleteDC(buffer_);
            buffer_ = nullptr;
            previousFont_ = nullptr;
        }
        if (bitmap_ != nullptr) {
            DeleteObject(bitmap_);
            bitmap_ = nullptr;
        }
    }

    // A monospaced font of kFontEm at the current scale, its own width (narrower than the 8-pixel grid, which
    // drawText keeps): Cascadia Mono, or Consolas when Windows has no Cascadia Mono.
    [[nodiscard]] HFONT createFont(const wchar_t* face) const {
        return CreateFontW(-std::max(6L, std::lround(kFontEm * box_.scale)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           FIXED_PITCH | FF_MODERN, face);
    }

    void updateFont() {
        if (font_ != nullptr && std::abs(fontScale_ - box_.scale) < 0.001F) {
            return;
        }
        HFONT font = createFont(kFontFace);
        if (font != nullptr) {
            // Windows gives another font when the face is not installed: check which one it chose.
            const HGDIOBJ before = SelectObject(buffer_, font);
            wchar_t chosen[LF_FACESIZE] = {};
            GetTextFaceW(buffer_, LF_FACESIZE, chosen);
            SelectObject(buffer_, before);
            if (lstrcmpiW(chosen, kFontFace) != 0) {
                DeleteObject(font);
                font = createFont(kFallbackFace);
            }
        } else {
            font = createFont(kFallbackFace);
        }
        if (font == nullptr) {
            return;   // keep the previous font (or the system one)
        }
        const HGDIOBJ previous = SelectObject(buffer_, font);
        if (previousFont_ == nullptr) previousFont_ = previous;
        if (font_ != nullptr) DeleteObject(font_);
        font_ = font;
        fontScale_ = box_.scale;
    }

    int logicalWidth_;
    int logicalHeight_;
    HWND window_ = nullptr;
    bool trayIcon_ = false;   // notify() added WheelBri's icon to the notification area
    HDC buffer_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previousBitmap_ = nullptr;
    HGDIOBJ previousFont_ = nullptr;
    HFONT font_ = nullptr;
    float fontScale_ = -1.0F;
    int bufferWidth_ = 0;
    int bufferHeight_ = 0;
    Letterbox box_;
    int frameWidth_ = 1;
    int frameHeight_ = 1;
    DrawList frame_;   // being recorded
    DrawList shown_;   // in the buffer and on screen
    std::vector<INT> advances_;
    bool closed_ = false;
    std::string keys_;
    std::string text_;
    int wheelDelta_ = 0;
    int hwheelDelta_ = 0;
    Click pointer_;
    bool leftDown_ = false;
    bool inside_ = false;   // the mouse is over the window (WM_MOUSELEAVE is being tracked)
    std::vector<Click> clicks_;
};

}  // namespace

std::unique_ptr<UiWindow> openWindow(const std::string& title, const int logicalWidth, const int logicalHeight,
                                     const int windowWidth, const int windowHeight, std::string& error) {
    becomeDpiAware();
    auto window = std::make_unique<Win32Window>(logicalWidth, logicalHeight);
    if (!window->create(title, windowWidth, windowHeight, error)) {
        return nullptr;
    }
    return window;
}

}  // namespace wheelbri::service::ui
