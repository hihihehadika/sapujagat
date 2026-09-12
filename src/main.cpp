// cache-cleaner: a simple Windows cache/junk file cleaner CLI.
//
// Scans common cache/junk locations on Windows, shows their size, lets
// the user pick which ones to clean, then deletes them (skipping any
// file that's locked/in use instead of crashing).
//
// Also supports user-defined "custom" folders (e.g. a personal dump
// folder) which are saved to a config file so they persist across runs.
//
// Build (Windows, MSVC "Developer Command Prompt"):
//   cl /std:c++17 /EHsc /O2 src\main.cpp /Fe:cache-cleaner.exe
//
// Build (Windows, MinGW-w64 g++):
//   g++ -std=c++17 -O2 src/main.cpp -o cache-cleaner.exe
//
// Run as Administrator for full access (Windows\Temp, Prefetch, and the
// Windows Update cache all require elevated permissions).
//
// Flags:
//   --dry-run   Show what would be deleted without actually deleting anything.
//   --yes       Skip the confirmation prompt (useful for scripting / Task Scheduler).

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>
#endif

namespace fs = std::filesystem;

// -------------------------------------------------------------------------
// Global flags set from argv in parse_args().
// -------------------------------------------------------------------------
static bool g_dry_run  = false;
static bool g_auto_yes = false;

// -------------------------------------------------------------------------
// A cache "target": a human-readable name + the folder it lives in.
// `is_custom` marks folders the user added themselves vs. built-in ones.
// -------------------------------------------------------------------------
struct CacheTarget {
    std::string name;
    fs::path    path;
    bool        is_custom   = false;
    uintmax_t   size_bytes  = 0;
    bool        exists      = false;
};

// -------------------------------------------------------------------------
// A persisted custom folder entry.
//   whole_folder = true  -> delete the entire folder (folder + contents)
//   whole_folder = false -> delete only the folder's contents, keep folder
// Stored as "path|whole" or "path|contents" in custom_folders.txt.
// -------------------------------------------------------------------------
struct CustomFolderEntry {
    fs::path path;
    bool     whole_folder = true;
};

// -------------------------------------------------------------------------
// Reads an environment variable safely (Windows-friendly).
// -------------------------------------------------------------------------
static std::string env(const char* name) {
    char*  buf = nullptr;
    size_t len = 0;
#ifdef _WIN32
    _dupenv_s(&buf, &len, name);
    std::string result = buf ? buf : "";
    free(buf);
    return result;
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

// -------------------------------------------------------------------------
// Console color helpers (Windows console API).
//
// Color codes:
//   10 = green   (success / "done")
//   14 = yellow  (sizes)
//   12 = red     (warnings / skipped files)
//    9 = cyan    (headers / info)
//    7 = default gray/white
// -------------------------------------------------------------------------
static WORD g_default_color = 7;

static void init_console_color() {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(hOut, &info))
        g_default_color = info.wAttributes;
#endif
}

static void set_color(int color) {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hOut, static_cast<WORD>(color));
#else
    (void)color;
#endif
}

static void reset_color() {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hOut, g_default_color);
#endif
}

// -------------------------------------------------------------------------
// Argument parsing.
// Supported flags: --dry-run, --yes
// -------------------------------------------------------------------------
static void parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) {
            g_dry_run = true;
        } else if (std::strcmp(argv[i], "--yes") == 0) {
            g_auto_yes = true;
        }
    }
}

// -------------------------------------------------------------------------
// Persistence: custom folders saved to
//   %APPDATA%\cache-cleaner\custom_folders.txt
// Format per line: "path|whole" or "path|contents"
// -------------------------------------------------------------------------
static fs::path config_file_path() {
    fs::path dir = env("APPDATA");
    dir /= "cache-cleaner";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / "custom_folders.txt";
}

static std::vector<CustomFolderEntry> load_custom_folders() {
    std::vector<CustomFolderEntry> result;
    std::ifstream in(config_file_path());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        CustomFolderEntry entry;
        // Parse "path|whole" or "path|contents" format.
        size_t sep = line.rfind('|');
        if (sep != std::string::npos) {
            std::string mode = line.substr(sep + 1);
            entry.path         = line.substr(0, sep);
            entry.whole_folder = (mode != "contents");
        } else {
            // Legacy format: path only, default to whole_folder = true.
            entry.path         = line;
            entry.whole_folder = true;
        }
        result.push_back(entry);
    }
    return result;
}

static void save_custom_folders(const std::vector<CustomFolderEntry>& folders) {
    std::ofstream out(config_file_path(), std::ios::trunc);
    for (auto& f : folders) {
        out << f.path.string() << "|"
            << (f.whole_folder ? "whole" : "contents") << "\n";
    }
}

// -------------------------------------------------------------------------
// Built-in Windows / browser cache targets.
// -------------------------------------------------------------------------
static std::vector<CacheTarget> build_builtin_targets() {
    std::string localAppData = env("LOCALAPPDATA");
    std::string userTemp     = env("TEMP");
    std::string systemRoot   = env("SystemRoot");
    std::string appData      = env("APPDATA");

    return {
        {"Windows Temp (user)",            userTemp,                                                                     false, 0, false},
        {"Windows Temp (system)",          systemRoot + "\\Temp",                                                        false, 0, false},
        {"Windows Prefetch",               systemRoot + "\\Prefetch",                                                    false, 0, false},
        {"Windows Update download cache",  systemRoot + "\\SoftwareDistribution\\Download",                              false, 0, false},
        {"Explorer thumbnail cache",       localAppData + "\\Microsoft\\Windows\\Explorer",                              false, 0, false},
        {"Chrome browser cache",           localAppData + "\\Google\\Chrome\\User Data\\Default\\Cache",                 false, 0, false},
        {"Edge browser cache",             localAppData + "\\Microsoft\\Edge\\User Data\\Default\\Cache",                false, 0, false},
    };
}

// -------------------------------------------------------------------------
// Firefox cache targets.
// Firefox stores cache under:
//   %APPDATA%\Mozilla\Firefox\Profiles\<random>.default-release\cache2
// We enumerate all profile folders and add each cache2 path found.
// -------------------------------------------------------------------------
static std::vector<CacheTarget> build_firefox_targets() {
    std::vector<CacheTarget> result;
    std::string appData = env("APPDATA");
    if (appData.empty()) return result;

    fs::path profiles_dir = fs::path(appData) / "Mozilla" / "Firefox" / "Profiles";
    std::error_code ec;
    if (!fs::exists(profiles_dir, ec) || ec) return result;

    for (auto& entry : fs::directory_iterator(profiles_dir, ec)) {
        if (!entry.is_directory()) continue;
        fs::path cache2 = entry.path() / "cache2";
        std::error_code cec;
        if (fs::exists(cache2, cec) && !cec) {
            std::string label = "Firefox cache (" + entry.path().filename().string() + ")";
            result.push_back({label, cache2, false, 0, false});
        }
    }
    return result;
}

// -------------------------------------------------------------------------
// Recursively sums the size of every regular file under `dir`.
// -------------------------------------------------------------------------
static uintmax_t folder_size(const fs::path& dir) {
    uintmax_t total = 0;
    std::error_code ec;

    if (!fs::exists(dir, ec) || ec) return 0;

    for (auto it = fs::recursive_directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        std::error_code fec;
        if (it->is_regular_file(fec) && !fec) {
            auto sz = it->file_size(fec);
            if (!fec) total += sz;
        }
    }
    return total;
}

// Human-readable byte formatting: 1234567 -> "1.18 MB"
static std::string human_size(uintmax_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double size = static_cast<double>(bytes);
    int    unit = 0;
    while (size >= 1024.0 && unit < 4) { size /= 1024.0; ++unit; }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f %s", size, units[unit]);
    return std::string(buf);
}

// -------------------------------------------------------------------------
// Deletes the *contents* of a built-in cache folder (not the folder itself).
// Skips files that are locked/in use instead of crashing.
// In dry-run mode: prints what would be deleted without touching anything.
// -------------------------------------------------------------------------
static void clean_folder_contents(const fs::path& dir,
                                  uintmax_t& bytes_freed,
                                  int& files_skipped) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) return;

    for (auto& entry : fs::directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code fec;
        uintmax_t entry_size = entry.is_regular_file(fec)
                                   ? entry.file_size(fec)
                                   : folder_size(entry.path());

        if (g_dry_run) {
            set_color(14);
            std::cout << "    [dry-run] would delete: " << entry.path().string()
                      << " (" << human_size(entry_size) << ")\n";
            reset_color();
            continue;
        }

        std::error_code rm_ec;
        uintmax_t removed = fs::remove_all(entry.path(), rm_ec);

        if (rm_ec || removed == 0) {
            ++files_skipped;
        } else {
            bytes_freed += entry_size;
        }
    }
}

// -------------------------------------------------------------------------
// Deletes a custom folder entirely (folder + all contents).
// In dry-run mode: prints what would be deleted without touching anything.
// -------------------------------------------------------------------------
static void clean_custom_folder(const fs::path& dir,
                                uintmax_t& bytes_freed,
                                int& files_skipped) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) return;

    uintmax_t size_before = folder_size(dir);

    if (g_dry_run) {
        set_color(14);
        std::cout << "    [dry-run] would delete folder: " << dir.string()
                  << " (" << human_size(size_before) << ")\n";
        reset_color();
        return;
    }

    std::error_code rm_ec;
    fs::remove_all(dir, rm_ec);

    if (rm_ec) {
        ++files_skipped;
        clean_folder_contents(dir, bytes_freed, files_skipped);
    } else {
        bytes_freed += size_before;
    }
}

static void empty_recycle_bin() {
#ifdef _WIN32
    SHEmptyRecycleBinA(nullptr, nullptr,
                        SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI |
                            SHERB_NOSOUND);
#endif
}

// -------------------------------------------------------------------------
// Print the app header banner.
// -------------------------------------------------------------------------
static void print_header() {
    set_color(9);
    std::cout << "==============================================\n";
    std::cout << "   cache-cleaner  --  Windows junk remover\n";
    std::cout << "==============================================\n";
    reset_color();
    if (g_dry_run) {
        set_color(14);
        std::cout << "  [DRY-RUN MODE] Nothing will be deleted.\n";
        reset_color();
    }
    std::cout << "\n";
}

// -------------------------------------------------------------------------
// Flow: add a new custom folder to the list.
// -------------------------------------------------------------------------
static void add_custom_folder_flow() {
    std::cout << "\nEnter the folder path to add (e.g. D:\\Junk):\n> ";
    std::string path_str;
    std::getline(std::cin, path_str);
    if (path_str.empty()) {
        std::cout << "Cancelled.\n";
        return;
    }

    fs::path p(path_str);
    if (!fs::exists(p)) {
        std::cout << "Folder not found. Add it anyway? (y/n): ";
        std::string confirm;
        std::getline(std::cin, confirm);
        if (confirm != "y" && confirm != "Y") return;
    }

    auto folders = load_custom_folders();
    for (auto& f : folders) {
        if (f.path == p) {
            std::cout << "That folder is already in the list.\n";
            return;
        }
    }

    // Ask the user which delete mode to use for this folder.
    std::cout << "Delete mode:\n";
    std::cout << "  [1] Delete the entire folder (folder + contents)\n";
    std::cout << "  [2] Delete only the contents, keep the folder itself\n";
    std::cout << "> ";
    std::string mode_str;
    std::getline(std::cin, mode_str);
    bool whole = (mode_str != "2");

    CustomFolderEntry entry;
    entry.path         = p;
    entry.whole_folder = whole;
    folders.push_back(entry);
    save_custom_folders(folders);

    std::cout << "Added! This folder will stay in the list even after closing the app.\n";
    std::cout << "Delete mode: " << (whole ? "entire folder" : "contents only") << "\n";
}

// -------------------------------------------------------------------------
// Flow: remove a custom folder from the list.
// -------------------------------------------------------------------------
static void remove_custom_folder_flow() {
    auto folders = load_custom_folders();
    if (folders.empty()) {
        std::cout << "\nNo custom folders added yet.\n";
        return;
    }

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
            save_custom_folders(folders);
        } else {
            std::cout << "Invalid number.\n";
        }
    } catch (...) {
        std::cout << "Invalid input.\n";
    }
}

// -------------------------------------------------------------------------
// Main scan-and-clean flow.
// -------------------------------------------------------------------------
static void scan_and_clean_flow() {
    std::cout << "\nScanning... (this may take a few seconds)\n\n";

    auto targets = build_builtin_targets();

    // Merge Firefox targets (empty if Firefox not installed).
    auto ff = build_firefox_targets();
    targets.insert(targets.end(), ff.begin(), ff.end());

    // Merge custom folder targets.
    std::vector<CustomFolderEntry> custom_entries = load_custom_folders();
    for (auto& c : custom_entries) {
        std::string label = c.path.filename().string().empty()
                                ? c.path.string()
                                : "[Custom] " + c.path.filename().string();
        targets.push_back({label, c.path, true, 0, false});
    }

    // Scan sizes.
    for (auto& t : targets) {
        t.exists     = fs::exists(t.path);
        t.size_bytes = t.exists ? folder_size(t.path) : 0;
    }

    // Display results.
    std::cout << "Cache categories found:\n\n";
    uintmax_t total = 0;
    for (size_t i = 0; i < targets.size(); i++) {
        std::cout << "  [" << (i + 1) << "] " << targets[i].name;
        if (targets[i].exists) {
            set_color(14); // yellow
            std::cout << " (" << human_size(targets[i].size_bytes) << ")";
            reset_color();
            total += targets[i].size_bytes;
        } else {
            set_color(12); // red
            std::cout << " (not found)";
            reset_color();
        }
        std::cout << "\n";
    }
    std::cout << "  [" << (targets.size() + 1) << "] Recycle Bin\n\n";
    std::cout << "Total cleanable space (estimate): ";
    set_color(14);
    std::cout << human_size(total);
    reset_color();
    std::cout << "\n\n";

    std::cout << "Select numbers to clean (space-separated), 'a' for all, 'q' to cancel:\n> ";
    std::string line;
    std::getline(std::cin, line);

    if (line == "q" || line.empty()) {
        std::cout << "Cancelled.\n";
        return;
    }

    std::vector<int> chosen;
    if (line == "a") {
        for (size_t i = 1; i <= targets.size() + 1; i++)
            chosen.push_back(static_cast<int>(i));
    } else {
        size_t pos = 0;
        while (pos < line.size()) {
            size_t next = line.find(' ', pos);
            std::string tok = line.substr(pos, next - pos);
            if (!tok.empty()) {
                try { chosen.push_back(std::stoi(tok)); } catch (...) {}
            }
            if (next == std::string::npos) break;
            pos = next + 1;
        }
    }

    // Confirmation prompt (skipped if --yes flag is set).
    if (!g_auto_yes) {
        std::cout << "\nAre you sure you want to clean the selected categories? (y/n): ";
        std::string confirm;
        std::getline(std::cin, confirm);
        if (confirm != "y" && confirm != "Y") {
            std::cout << "Cancelled.\n";
            return;
        }
    }

    std::cout << "\n";
    if (g_dry_run) {
        set_color(14);
        std::cout << "[DRY-RUN] Showing what would be deleted:\n\n";
        reset_color();
    } else {
        std::cout << "Cleaning...\n\n";
    }

    uintmax_t total_freed = 0;

    for (int n : chosen) {
        // Recycle Bin is always the last item.
        if (n == static_cast<int>(targets.size() + 1)) {
            std::cout << "  - Recycle Bin... ";
            if (g_dry_run) {
                set_color(14);
                std::cout << "[dry-run] would empty Recycle Bin\n";
                reset_color();
            } else {
                empty_recycle_bin();
                set_color(10);
                std::cout << "done\n";
                reset_color();
            }
            continue;
        }

        int idx = n - 1;
        if (idx < 0 || idx >= static_cast<int>(targets.size())) continue;

        auto& t = targets[idx];
        if (!t.exists) continue;

        uintmax_t freed   = 0;
        int       skipped = 0;
        std::cout << "  - " << t.name << "...\n";

        if (t.is_custom) {
            // Find matching custom entry to check its delete mode.
            bool whole = true;
            for (auto& ce : custom_entries) {
                if (ce.path == t.path) { whole = ce.whole_folder; break; }
            }
            if (whole) {
                clean_custom_folder(t.path, freed, skipped);
            } else {
                clean_folder_contents(t.path, freed, skipped);
            }
        } else {
            clean_folder_contents(t.path, freed, skipped);
        }

        total_freed += freed;

        if (!g_dry_run) {
            std::cout << "    ";
            set_color(10);
            std::cout << "freed " << human_size(freed);
            reset_color();
            if (skipped > 0) {
                std::cout << " ";
                set_color(12);
                std::cout << "(" << skipped << " item(s) skipped — in use)";
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
        std::cout << "Done! Total space freed: " << human_size(total_freed) << "\n";
        reset_color();
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
        std::cout << "  [4] Exit\n";
        std::cout << "> ";

        std::string choice;
        std::getline(std::cin, choice);

        if (choice == "1") {
            scan_and_clean_flow();
        } else if (choice == "2") {
            add_custom_folder_flow();
        } else if (choice == "3") {
            remove_custom_folder_flow();
        } else if (choice == "4" || choice == "q") {
            break;
        } else {
            set_color(12);
            std::cout << "Invalid choice.\n";
            reset_color();
        }
        std::cout << "\n";
    }

    return 0;
}
