// sapujagat CLI entry point.
//
// Build (MSVC):    cl /std:c++17 /EHsc /O2 src\main.cpp /Fe:sapujagat.exe
// Build (MinGW):   g++ -std=c++17 -O2 -static src/main.cpp -o sapujagat.exe
//
// Run as Administrator for full access (Windows\Temp, Prefetch,
// Windows Update cache all require elevated permissions).
//
// Flags:
//   --dry-run   Show what would be deleted without actually deleting anything.
//   --yes       Skip the confirmation prompt (useful for Task Scheduler).

#include "core.h"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

// -------------------------------------------------------------------------
// Global CLI flags.
// -------------------------------------------------------------------------
static bool g_dry_run  = false;
static bool g_auto_yes = false;

// -------------------------------------------------------------------------
// Console color helpers.
//   10 = green   12 = red   14 = yellow   9 = cyan   7 = default
// -------------------------------------------------------------------------
static WORD g_default_color = 7;

static void init_console_color() {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(h, &info))
        g_default_color = info.wAttributes;
#endif
}

static void set_color(int c) {
#ifdef _WIN32
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), static_cast<WORD>(c));
#else
    (void)c;
#endif
}

static void reset_color() {
#ifdef _WIN32
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), g_default_color);
#endif
}

// -------------------------------------------------------------------------
// Argument parsing.
// -------------------------------------------------------------------------
static void parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) g_dry_run  = true;
        else if (std::strcmp(argv[i], "--yes") == 0) g_auto_yes = true;
    }
}

// -------------------------------------------------------------------------
// Banner.
// -------------------------------------------------------------------------
static void print_header() {
    set_color(9);
    std::cout << "============================================\n";
    std::cout << "   sapujagat  --  Windows junk remover\n";
    std::cout << "============================================\n";
    reset_color();
    if (g_dry_run) {
        set_color(14);
        std::cout << "  [DRY-RUN MODE] Nothing will be deleted.\n";
        reset_color();
    }
    std::cout << "\n";
}

// -------------------------------------------------------------------------
// Flow: scan & clean.
// -------------------------------------------------------------------------
static void scan_and_clean_flow() {
    std::cout << "\nScanning... (this may take a few seconds)\n\n";

    ScanResult sr = sj_scan();

    // Display categories.
    std::cout << "Cache categories found:\n\n";
    for (size_t i = 0; i < sr.targets.size(); i++) {
        std::cout << "  [" << (i + 1) << "] " << sr.targets[i].name;
        if (sr.targets[i].exists) {
            set_color(14);
            std::cout << " (" << sj_human_size(sr.targets[i].size_bytes) << ")";
            reset_color();
        } else {
            set_color(12);
            std::cout << " (not found)";
            reset_color();
        }
        std::cout << "\n";
    }
    std::cout << "  [" << (sr.targets.size() + 1) << "] Recycle Bin\n\n";
    std::cout << "Total cleanable space (estimate): ";
    set_color(14);
    std::cout << sj_human_size(sr.total_bytes);
    reset_color();
    std::cout << "\n\n";

    // Select categories.
    std::cout << "Select numbers to clean (space-separated), 'a' for all, 'q' to cancel:\n> ";
    std::string line;
    std::getline(std::cin, line);
    if (line == "q" || line.empty()) { std::cout << "Cancelled.\n"; return; }

    std::vector<int> chosen;
    if (line == "a") {
        for (size_t i = 1; i <= sr.targets.size() + 1; i++)
            chosen.push_back(static_cast<int>(i));
    } else {
        size_t pos = 0;
        while (pos < line.size()) {
            size_t next = line.find(' ', pos);
            std::string tok = line.substr(pos, next - pos);
            if (!tok.empty()) { try { chosen.push_back(std::stoi(tok)); } catch (...) {} }
            if (next == std::string::npos) break;
            pos = next + 1;
        }
    }

    // Confirmation.
    if (!g_auto_yes) {
        std::cout << "\nAre you sure you want to clean the selected categories? (y/n): ";
        std::string confirm;
        std::getline(std::cin, confirm);
        if (confirm != "y" && confirm != "Y") { std::cout << "Cancelled.\n"; return; }
    }

    // Clean.
    std::cout << "\n";
    if (g_dry_run) {
        set_color(14);
        std::cout << "[DRY-RUN] Showing what would be deleted:\n\n";
        reset_color();
    } else {
        std::cout << "Cleaning...\n\n";
    }

    int recycle_bin_idx = static_cast<int>(sr.targets.size()) + 1;
    uintmax_t total_freed = 0;

    for (int n : chosen) {
        if (n == recycle_bin_idx) {
            std::cout << "  - Recycle Bin... ";
            if (g_dry_run) {
                set_color(14);
                std::cout << "[dry-run] would empty Recycle Bin\n";
                reset_color();
            } else {
                sj_empty_recycle_bin();
                set_color(10);
                std::cout << "done\n";
                reset_color();
            }
            continue;
        }
        int idx = n - 1;
        if (idx < 0 || idx >= static_cast<int>(sr.targets.size())) continue;
        const CacheTarget& t = sr.targets[idx];
        if (!t.exists) continue;

        std::cout << "  - " << t.name << "...\n";

        CleanResult r;
        if (t.is_custom) {
            bool whole = true;
            for (auto& ce : sr.custom_entries)
                if (ce.path == t.path) { whole = ce.whole_folder; break; }

            if (g_dry_run) {
                r = whole ? sj_clean_custom_folder(t.path, true)
                          : sj_clean_folder_contents(t.path, true);
                set_color(14);
                std::cout << "    [dry-run] would delete "
                          << (whole ? "folder" : "contents") << ": "
                          << t.path.string()
                          << " (" << sj_human_size(r.bytes_freed) << ")\n";
                reset_color();
            } else {
                r = whole ? sj_clean_custom_folder(t.path, false)
                          : sj_clean_folder_contents(t.path, false);
            }
        } else {
            if (g_dry_run) {
                r = sj_clean_folder_contents(t.path, true);
                set_color(14);
                std::cout << "    [dry-run] would free ~"
                          << sj_human_size(r.bytes_freed) << "\n";
                reset_color();
            } else {
                r = sj_clean_folder_contents(t.path, false);
            }
        }

        total_freed += r.bytes_freed;

        if (!g_dry_run) {
            std::cout << "    ";
            set_color(10);
            std::cout << "freed " << sj_human_size(r.bytes_freed);
            reset_color();
            if (r.files_skipped > 0) {
                std::cout << " ";
                set_color(12);
                std::cout << "(" << r.files_skipped << " item(s) skipped — in use)";
                reset_color();
            }
            std::cout << "\n";
        }
    }

    std::cout << "\n";
    if (g_dry_run) {
        set_color(14);
        std::cout << "Dry-run complete. No files were deleted.\n";
        reset_color();
    } else {
        set_color(10);
        std::cout << "Done! Total space freed: " << sj_human_size(total_freed) << "\n";
        reset_color();
    }
}

// -------------------------------------------------------------------------
// Flow: add a custom folder.
// -------------------------------------------------------------------------
static void add_custom_folder_flow() {
    std::cout << "\nEnter the folder path to add (e.g. D:\\Junk):\n> ";
    std::string path_str;
    std::getline(std::cin, path_str);
    if (path_str.empty()) { std::cout << "Cancelled.\n"; return; }

    fs::path p(path_str);
    if (!fs::exists(p)) {
        std::cout << "Folder not found. Add it anyway? (y/n): ";
        std::string confirm;
        std::getline(std::cin, confirm);
        if (confirm != "y" && confirm != "Y") return;
    }

    auto folders = sj_load_custom_folders();
    for (auto& f : folders) {
        if (f.path == p) { std::cout << "That folder is already in the list.\n"; return; }
    }

    std::cout << "Delete mode:\n";
    std::cout << "  [1] Delete the entire folder (folder + contents)\n";
    std::cout << "  [2] Delete only the contents, keep the folder itself\n";
    std::cout << "> ";
    std::string mode_str;
    std::getline(std::cin, mode_str);
    bool whole = (mode_str != "2");

    folders.push_back({p, whole});
    sj_save_custom_folders(folders);

    std::cout << "Added! This folder will stay in the list even after closing the app.\n";
    std::cout << "Delete mode: " << (whole ? "entire folder" : "contents only") << "\n";
}

// -------------------------------------------------------------------------
// Flow: remove a custom folder.
// -------------------------------------------------------------------------
static void remove_custom_folder_flow() {
    auto folders = sj_load_custom_folders();
    if (folders.empty()) { std::cout << "\nNo custom folders added yet.\n"; return; }

    std::cout << "\nCurrent custom folders:\n";
    for (size_t i = 0; i < folders.size(); i++) {
        std::cout << "  [" << (i + 1) << "] " << folders[i].path.string()
                  << "  (" << (folders[i].whole_folder ? "entire folder" : "contents only") << ")\n";
    }
    std::cout << "\nWhich number to remove from the list?\n> ";
    std::string line;
    std::getline(std::cin, line);
    if (line.empty()) return;

    try {
        int idx = std::stoi(line) - 1;
        if (idx >= 0 && idx < static_cast<int>(folders.size())) {
            std::cout << "Removed from list: " << folders[idx].path.string() << "\n";
            folders.erase(folders.begin() + idx);
            sj_save_custom_folders(folders);
        } else {
            std::cout << "Invalid number.\n";
        }
    } catch (...) { std::cout << "Invalid input.\n"; }
}

// -------------------------------------------------------------------------
// Flow: schedule automatic cleanup via Windows Task Scheduler.
// -------------------------------------------------------------------------
static void schedule_task_flow() {
    std::cout << "\n";
    set_color(9);
    std::cout << "--- Schedule Automatic Cleanup ---\n";
    reset_color();
    std::cout << "\nThis will:\n";
    std::cout << "  1. Create 'run-cleanup.bat' next to sapujagat.exe\n";
    std::cout << "  2. Register a Windows Task Scheduler task named\n";
    std::cout << "     'sapujagat-auto-clean' that runs with --yes flag.\n";
    std::cout << "\nNote: Run sapujagat.exe as Administrator for this to work fully.\n";
    std::cout << "\nSchedule options:\n";
    std::cout << "  [1] Every week on Monday at 09:00\n";
    std::cout << "  [2] Every time you log in to Windows\n";
    std::cout << "  [3] Custom (weekly — choose day & time)\n";
    std::cout << "  [4] Remove existing scheduled task\n";
    std::cout << "  [q] Cancel\n";
    std::cout << "> ";

    std::string choice;
    std::getline(std::cin, choice);

    if (choice == "q" || choice.empty()) { std::cout << "Cancelled.\n"; return; }

    if (choice == "4") {
        std::cout << "Removing scheduled task... ";
        if (sj_unregister_scheduled_task()) {
            set_color(10);
            std::cout << "done.\n";
        } else {
            set_color(12);
            std::cout << "failed (task may not exist or needs Administrator).\n";
        }
        reset_color();
        return;
    }

    std::string schedule = "WEEKLY";
    std::string day      = "MON";
    std::string time_str = "09:00";

    if (choice == "2") {
        schedule = "ONLOGON";
    } else if (choice == "3") {
        std::cout << "Day (MON/TUE/WED/THU/FRI/SAT/SUN): ";
        std::getline(std::cin, day);
        if (day.empty()) day = "MON";
        std::cout << "Time (HH:MM, 24-hour): ";
        std::getline(std::cin, time_str);
        if (time_str.empty()) time_str = "09:00";
    }

    std::cout << "Creating scheduled task... ";
    if (sj_register_scheduled_task(schedule, day, time_str)) {
        set_color(10);
        std::cout << "done!\n";
        reset_color();
        std::cout << "\nTask registered: 'sapujagat-auto-clean'\n";
        std::cout << "Launcher script: " << (sj_exe_dir() / "run-cleanup.bat").string() << "\n";
        std::cout << "Tip: Open Task Scheduler to view or edit the task.\n";
    } else {
        set_color(12);
        std::cout << "failed.\n";
        reset_color();
        std::cout << "This usually means sapujagat.exe was not run as Administrator,\n";
        std::cout << "or the path to the exe contains special characters.\n";
        std::cout << "You can also manually create the task:\n";
        std::cout << "  Launcher: " << (sj_exe_dir() / "run-cleanup.bat").string() << "\n";
    }
}

// -------------------------------------------------------------------------
// Entry point.
// -------------------------------------------------------------------------
int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    init_console_color();
    parse_args(argc, argv);
    print_header();

    while (true) {
        std::cout << "Menu:\n";
        std::cout << "  [1] Scan & clean cache\n";
        std::cout << "  [2] Add custom folder\n";
        std::cout << "  [3] Remove custom folder from list\n";
        std::cout << "  [4] Schedule automatic cleanup\n";
        std::cout << "  [5] Exit\n";
        std::cout << "> ";

        std::string choice;
        if (!std::getline(std::cin, choice)) break;

        if      (choice == "1") scan_and_clean_flow();
        else if (choice == "2") add_custom_folder_flow();
        else if (choice == "3") remove_custom_folder_flow();
        else if (choice == "4") schedule_task_flow();
        else if (choice == "5" || choice == "q") break;
        else {
            set_color(12);
            std::cout << "Invalid choice.\n";
            reset_color();
        }
        std::cout << "\n";
    }

    return 0;
}
