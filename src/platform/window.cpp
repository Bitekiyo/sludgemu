#include "platform/window.h"

#include "common/log.h"
#include "common/str.h"
#include "hle/kernel/overrides.h"

#define MIN_CLIENT_WIDTH 320
#define MIN_CLIENT_HEIGHT 180

struct WindowState {
    wchar_t display_name[256];
    HWND hwnd;
    WNDPROC title_proc;
    bool fullscreen;
    WINDOWPLACEMENT windowed_placement;
    volatile LONG client_width;
    volatile LONG client_height;
};

static WindowState g_window;

void window_init(const wchar_t* display_name) {
    memset(&g_window, 0, sizeof(g_window));
    g_window.windowed_placement.length = sizeof(WINDOWPLACEMENT);
    if (display_name)
        str_copy_w(g_window.display_name, ARRAY_COUNT(g_window.display_name), display_name);
}

HWND window_main() { return g_window.hwnd; }

void window_client_size(u32* width, u32* height) {
    *width = (u32)g_window.client_width;
    *height = (u32)g_window.client_height;
}

bool window_is_fullscreen() { return g_window.fullscreen; }

static const DWORD k_windowed_style = WS_OVERLAPPEDWINDOW;

void window_toggle_fullscreen() {
    HWND hwnd = g_window.hwnd;
    if (!hwnd)
        return;
    DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (!g_window.fullscreen) {
        MONITORINFO mi = {sizeof(mi)};
        if (!GetWindowPlacement(hwnd, &g_window.windowed_placement) ||
            !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi))
            return;
        SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~k_windowed_style) | WS_POPUP);
        const RECT& r = mi.rcMonitor;
        SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_window.fullscreen = true;
        LOG_I("window", "entered borderless fullscreen (%ldx%ld)", r.right - r.left, r.bottom - r.top);
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~WS_POPUP) | k_windowed_style);
        SetWindowPlacement(hwnd, &g_window.windowed_placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_window.fullscreen = false;
        LOG_I("window", "left fullscreen");
    }
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        bool repeat = (lp & (1 << 30)) != 0;
        bool alt = (lp & (1 << 29)) != 0;
        if (!repeat && (wp == VK_F11 || (wp == VK_RETURN && alt && msg == WM_SYSKEYDOWN))) {
            window_toggle_fullscreen();
            return 0;
        }
        break;
    }
    case WM_SYSCHAR:
        if (wp == VK_RETURN)
            return 0; // suppress the Alt+Enter beep
        break;
    case WM_SIZE:
        InterlockedExchange(&g_window.client_width, LOWORD(lp));
        InterlockedExchange(&g_window.client_height, HIWORD(lp));
        break;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        RECT r = {0, 0, MIN_CLIENT_WIDTH, MIN_CLIENT_HEIGHT};
        AdjustWindowRectEx(&r, k_windowed_style, FALSE, 0);
        mmi->ptMinTrackSize.x = r.right - r.left;
        mmi->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_DESTROY:
        if (hwnd == g_window.hwnd) {
            // Console titles never expect their window to go away; treat it as quitting the game.
            LOG_I("window", "main window destroyed, exiting");
            CallWindowProcA(g_window.title_proc, hwnd, msg, wp, lp);
            log_shutdown();
            ExitProcess(0);
        }
        break;
    default:
        break;
    }
    return CallWindowProcA(g_window.title_proc, hwnd, msg, wp, lp);
}

// Picks a windowed client size: the title's requested size, shrunk (keeping aspect) to fit 85%
// of the primary monitor's work area.
static void initial_client_size(int req_w, int req_h, int* out_w, int* out_h) {
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int max_w = (work.right - work.left) * 85 / 100;
    int max_h = (work.bottom - work.top) * 85 / 100;
    if (req_w <= 0 || req_h <= 0 || req_w == CW_USEDEFAULT || req_h == CW_USEDEFAULT) {
        req_w = 1920;
        req_h = 1080;
    }
    f64 scale = 1.0;
    if (req_w > max_w)
        scale = (f64)max_w / req_w;
    if (req_h * scale > max_h)
        scale = (f64)max_h / req_h;
    *out_w = (int)(req_w * scale);
    *out_h = (int)(req_h * scale);
}

static HWND WINAPI hk_CreateWindowExA(DWORD ex_style, LPCSTR class_name, LPCSTR window_name, DWORD style, int x,
                                      int y, int w, int h, HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
    bool top_level = !(style & WS_CHILD) && !parent;
    if (!top_level || g_window.hwnd)
        return CreateWindowExA(ex_style, class_name, window_name, style, x, y, w, h, parent, menu, instance, param);

    LOG_I("window", "title creates top-level window: style=0x%08lx ex=0x%08lx %dx%d", style, ex_style, w, h);

    int cw, ch;
    initial_client_size(w, h, &cw, &ch);
    DWORD new_style = k_windowed_style | (style & WS_VISIBLE);
    DWORD new_ex = (ex_style & ~(WS_EX_TOPMOST | WS_EX_TOOLWINDOW)) | WS_EX_APPWINDOW;
    RECT r = {0, 0, cw, ch};
    AdjustWindowRectEx(&r, new_style, FALSE, new_ex);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int wx = work.left + ((work.right - work.left) - ww) / 2;
    int wy = work.top + ((work.bottom - work.top) - wh) / 2;

    HWND hwnd = CreateWindowExA(new_ex, class_name, window_name, new_style, wx, wy, ww, wh, nullptr, menu, instance, param);
    if (!hwnd) {
        LOG_E("window", "CreateWindowExA failed (%lu)", GetLastError());
        return nullptr;
    }
    g_window.hwnd = hwnd;
    g_window.title_proc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)&window_proc);
    if (g_window.display_name[0])
        SetWindowTextW(hwnd, g_window.display_name);
    RECT cr;
    GetClientRect(hwnd, &cr);
    g_window.client_width = cr.right;
    g_window.client_height = cr.bottom;
    LOG_I("window", "main window %p, client %ldx%ld", hwnd, cr.right, cr.bottom);
    return hwnd;
}

static BOOL WINAPI hk_ShowWindow(HWND hwnd, int cmd) {
    // Titles ask for SW_SHOWMAXIMIZED / fullscreen-ish states meant for the console shell.
    if (hwnd == g_window.hwnd && (cmd == SW_SHOWMAXIMIZED || cmd == SW_MAXIMIZE))
        cmd = SW_SHOWNORMAL;
    return ShowWindow(hwnd, cmd);
}

const HleExport g_window_overrides[] = {
    HLE_EXPORT_AS(hk_CreateWindowExA, "CreateWindowExA"),
    HLE_EXPORT_AS(hk_ShowWindow, "ShowWindow"),
};
const u32 g_window_override_count = ARRAY_COUNT(g_window_overrides);
