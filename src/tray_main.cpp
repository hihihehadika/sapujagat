// sapujagat tray app entry point.
//
// Runs sapujagat as a system tray app instead of a CLI.
// Right-click the tray icon to get the menu.
//
// Build (MinGW):
//   g++ -std=c++17 -O2 -static -mwindows src/tray_main.cpp -o sapujagat-tray.exe -lshell32 -lcomctl32
//
// Build (MSVC):
//   cl /std:c++17 /EHsc /O2 /DWIN32 src\tray_main.cpp /Fe:sapujagat-tray.exe shell32.lib comctl32.lib /link /SUBSYSTEM:WINDOWS
//
// Run as Administrator for full access to all cache locations.

#include "core.h"

#include <string>
#include <thread>
#include <vector>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>

// -------------------------------------------------------------------------
// Constants
// -------------------------------------------------------------------------
#define WM_TRAY_MSG   (WM_APP + 1)
#define IDM_CLEAN_ALL  1001
#define IDM_CLEAN_SEL  1002
#define IDM_DRY_RUN    1003
#define IDM_SCHEDULE   1004
#define IDM_STATUS     1005
#define IDM_EXIT       1006

static const char* WINDOW_CLASS = "sapujagat_tray_wnd";
static const char* APP_NAME     = "sapujagat";

// -------------------------------------------------------------------------
// Tray icon handle and last scan result (cached for status dialog).
// -------------------------------------------------------------------------
static NOTIFYICONDATAA g_nid     = {};
static HWND            g_hwnd    = nullptr;
static std::atomic<bool> g_busy  = false;

// -------------------------------------------------------------------------
// Show a simple message box (used for all dialogs in tray mode).
// -------------------------------------------------------------------------
static void tray_msg(const std::string& text, UINT type = MB_OK | MB_ICONINFORMATION) {
    MessageBoxA(g_hwnd, text.c_str(), APP_NAME, type);
}

// -------------------------------------------------------------------------
// Build a human-readable scan summary string.
// -------------------------------------------------------------------------
static std::string build_scan_summary(const ScanResult& sr) {
    std::string s;
    s += "Cache scan results:\n\n";
    for (size_t i = 0; i < sr.targets.size(); i++) {
        s += "  [" + std::to_string(i + 1) + "] " + sr.targets[i].name;
        if (sr.targets[i].exists)
            s += "  (" + sj_human_size(sr.targets[i].size_bytes) + ")";
        else
            s += "  (not found)";
        s += "\n";
    }
    s += "\nTotal: " + sj_human_size(sr.total_bytes);
    return s;
}

// -------------------------------------------------------------------------
// Add / remove tray icon.
// -------------------------------------------------------------------------
static void tray_add_icon() {
    g_nid.cbSize           = sizeof(NOTIFYICONDATAA);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY_MSG;
    g_nid.hIcon            = LoadIcon(nullptr, IDI_SHIELD);  // built-in shield icon
    strncpy_s(g_nid.szTip, sizeof(g_nid.szTip), "sapujagat - Windows junk remover", _TRUNCATE);
    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

static void tray_remove_icon() {
    Shell_NotifyIconA(NIM_DELETE, &g_nid);
}

// -------------------------------------------------------------------------
// Show context menu at cursor position.
// -------------------------------------------------------------------------
static void show_tray_menu() {
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    AppendMenuA(menu, MF_STRING, IDM_CLEAN_ALL, "Scan && Clean All");
    AppendMenuA(menu, MF_STRING, IDM_DRY_RUN,   "Dry Run (preview only)");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, IDM_STATUS,    "Show Last Scan Status");
    AppendMenuA(menu, MF_STRING, IDM_SCHEDULE,  "Manage Schedule...");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, IDM_EXIT,      "Exit");

    if (g_busy.load())
        EnableMenuItem(menu, IDM_CLEAN_ALL, MF_BYCOMMAND | MF_GRAYED);

    // Required so the menu disappears if user clicks elsewhere.
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON,
                   pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
}

// -------------------------------------------------------------------------
// Scan + clean in a worker thread so the tray stays responsive.
// -------------------------------------------------------------------------
static ScanResult g_last_scan;

static void do_clean_thread(bool dry_run) {
    g_busy.store(true);

    // Update tooltip to show "Working..."
    strncpy_s(g_nid.szTip, sizeof(g_nid.szTip), "sapujagat - Scanning...", _TRUNCATE);
    Shell_NotifyIconA(NIM_MODIFY, &g_nid);

    ScanResult sr = sj_scan();
    g_last_scan   = sr;

    if (dry_run) {
        std::string summary = build_scan_summary(sr);
        summary += "\n\n[DRY-RUN] No files were deleted.";
        tray_msg(summary, MB_OK | MB_ICONINFORMATION);
    } else {
        // Confirm before cleaning.
        std::string prompt = build_scan_summary(sr);
        prompt += "\n\nClean ALL categories now?";
        int answer = MessageBoxA(g_hwnd, prompt.c_str(), APP_NAME,
                                  MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (answer == IDYES) {
            // Build "all" list.
            std::vector<int> all;
            for (int i = 1; i <= static_cast<int>(sr.targets.size()) + 1; i++)
                all.push_back(i);

            CleanResult cr = sj_clean_chosen(sr, all, false);

            std::string result = "Cleanup complete!\n\n";
            result += "  Freed:   " + sj_human_size(cr.bytes_freed) + "\n";
            if (cr.files_skipped > 0)
                result += "  Skipped: " + std::to_string(cr.files_skipped) + " item(s) in use\n";
            tray_msg(result, MB_OK | MB_ICONINFORMATION);
        }
    }

    // Restore tooltip.
    strncpy_s(g_nid.szTip, sizeof(g_nid.szTip), "sapujagat - Windows junk remover", _TRUNCATE);
    Shell_NotifyIconA(NIM_MODIFY, &g_nid);
    g_busy.store(false);
}

// -------------------------------------------------------------------------
// Schedule management dialog (simple MessageBox-based flow).
// -------------------------------------------------------------------------
static void show_schedule_dialog() {
    std::string msg =
        "Schedule options:\n\n"
        "  [Yes]  Create task: run every Monday at 09:00\n"
        "  [No]   Create task: run at every login\n"
        "  [Cancel] Remove existing scheduled task\n";

    int answer = MessageBoxA(g_hwnd, msg.c_str(), "Manage Schedule",
                              MB_YESNOCANCEL | MB_ICONQUESTION);

    if (answer == IDYES) {
        bool ok = sj_register_scheduled_task("WEEKLY", "MON", "09:00");
        tray_msg(ok ? "Task 'sapujagat-auto-clean' created (weekly, Monday 09:00)."
                    : "Failed to create task. Try running as Administrator.",
                 ok ? MB_ICONINFORMATION : MB_ICONERROR);
    } else if (answer == IDNO) {
        bool ok = sj_register_scheduled_task("ONLOGON");
        tray_msg(ok ? "Task 'sapujagat-auto-clean' created (runs at login)."
                    : "Failed to create task. Try running as Administrator.",
                 ok ? MB_ICONINFORMATION : MB_ICONERROR);
    } else {
        bool ok = sj_unregister_scheduled_task();
        tray_msg(ok ? "Scheduled task removed."
                    : "Could not remove task (it may not exist).",
                 ok ? MB_ICONINFORMATION : MB_ICONWARNING);
    }
}

// -------------------------------------------------------------------------
// Window procedure.
// -------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY_MSG:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            show_tray_menu();
        } else if (lp == WM_LBUTTONDBLCLK) {
            // Double-click: quick dry-run scan.
            if (!g_busy.load())
                std::thread(do_clean_thread, true).detach();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_CLEAN_ALL:
            if (!g_busy.load())
                std::thread(do_clean_thread, false).detach();
            break;
        case IDM_DRY_RUN:
            if (!g_busy.load())
                std::thread(do_clean_thread, true).detach();
            break;
        case IDM_STATUS: {
            if (g_last_scan.targets.empty()) {
                tray_msg("No scan has been run yet.\nUse 'Scan & Clean All' or 'Dry Run' first.");
            } else {
                tray_msg(build_scan_summary(g_last_scan));
            }
            break;
        }
        case IDM_SCHEDULE:
            show_schedule_dialog();
            break;
        case IDM_EXIT:
            tray_remove_icon();
            PostQuitMessage(0);
            break;
        }
        return 0;

    case WM_DESTROY:
        tray_remove_icon();
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

// -------------------------------------------------------------------------
// WinMain entry point (no console window).
// -------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // Register window class.
    WNDCLASSEXA wc    = {};
    wc.cbSize         = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc    = WndProc;
    wc.hInstance      = hInst;
    wc.lpszClassName  = WINDOW_CLASS;
    RegisterClassExA(&wc);

    // Create hidden message-only window.
    g_hwnd = CreateWindowExA(0, WINDOW_CLASS, APP_NAME, 0,
                              0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    tray_add_icon();

    // Show startup balloon tip.
    g_nid.uFlags      |= NIF_INFO;
    strncpy_s(g_nid.szInfoTitle, sizeof(g_nid.szInfoTitle), "sapujagat running", _TRUNCATE);
    strncpy_s(g_nid.szInfo,      sizeof(g_nid.szInfo),
              "Right-click the tray icon to scan & clean.", _TRUNCATE);
    g_nid.dwInfoFlags  = NIIF_INFO;
    g_nid.uTimeout     = 3000;
    Shell_NotifyIconA(NIM_MODIFY, &g_nid);

    // Message loop.
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return static_cast<int>(msg.wParam);
}

#else
// Non-Windows stub.
int main() { return 0; }
#endif
