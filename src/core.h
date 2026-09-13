// core.h -- shared scanning/cleaning logic for sapujagat.
//
// Included by both the CLI (main.cpp) and the tray app (tray_main.cpp).
// No std::cin / std::cout dependency so it works in any context.
//
// Usage:
//   #define SAPUJAGAT_CORE_IMPL   (in exactly ONE .cpp file before including)
//   #include "core.h"
//
// In all other translation units, include without the define.

#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

// -------------------------------------------------------------------------
// Structs
// -------------------------------------------------------------------------

struct CacheTarget {
    std::string name;
    fs::path    path;
    bool        is_custom  = false;
    uintmax_t   size_bytes = 0;
    bool        exists     = false;
};

struct CustomFolderEntry {
    fs::path path;
    bool     whole_folder = true; // true = delete folder+contents; false = contents only
};

struct ScanResult {
    uintmax_t              total_bytes = 0;
    std::vector<CacheTarget>           targets;
    std::vector<CustomFolderEntry>     custom_entries;
};

struct CleanResult {
    uintmax_t bytes_freed  = 0;
    int       files_skipped = 0;
};

// -------------------------------------------------------------------------
// Utility: read environment variable safely.
// -------------------------------------------------------------------------
inline std::string sj_env(const char* name) {
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
// Utility: human-readable byte size.
// -------------------------------------------------------------------------
inline std::string sj_human_size(uintmax_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double size = static_cast<double>(bytes);
    int    unit = 0;
    while (size >= 1024.0 && unit < 4) { size /= 1024.0; ++unit; }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f %s", size, units[unit]);
    return std::string(buf);
}

// -------------------------------------------------------------------------
// Utility: recursively sum all file sizes under a directory.
// -------------------------------------------------------------------------
inline uintmax_t sj_folder_size(const fs::path& dir) {
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

// -------------------------------------------------------------------------
// Persistence: %APPDATA%\sapujagat\custom_folders.txt
// Format: "path|whole" or "path|contents" per line.
// -------------------------------------------------------------------------
inline fs::path sj_config_file_path() {
    fs::path dir = sj_env("APPDATA");
    dir /= "sapujagat";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / "custom_folders.txt";
}

inline std::vector<CustomFolderEntry> sj_load_custom_folders() {
    std::vector<CustomFolderEntry> result;
    std::ifstream in(sj_config_file_path());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        CustomFolderEntry entry;
        size_t sep = line.rfind('|');
        if (sep != std::string::npos) {
            std::string mode   = line.substr(sep + 1);
            entry.path         = line.substr(0, sep);
            entry.whole_folder = (mode != "contents");
        } else {
            entry.path         = line;
            entry.whole_folder = true;
        }
        result.push_back(entry);
    }
    return result;
}

inline void sj_save_custom_folders(const std::vector<CustomFolderEntry>& folders) {
    std::ofstream out(sj_config_file_path(), std::ios::trunc);
    for (auto& f : folders)
        out << f.path.string() << "|"
            << (f.whole_folder ? "whole" : "contents") << "\n";
}

// -------------------------------------------------------------------------
// Build target lists.
// -------------------------------------------------------------------------
inline std::vector<CacheTarget> sj_build_builtin_targets() {
    std::string local = sj_env("LOCALAPPDATA");
    std::string temp  = sj_env("TEMP");
    std::string root  = sj_env("SystemRoot");
    return {
        {"Windows Temp (user)",           temp,                                                              false, 0, false},
        {"Windows Temp (system)",         root + "\\Temp",                                                  false, 0, false},
        {"Windows Prefetch",              root + "\\Prefetch",                                              false, 0, false},
        {"Windows Update download cache", root + "\\SoftwareDistribution\\Download",                        false, 0, false},
        {"Explorer thumbnail cache",      local + "\\Microsoft\\Windows\\Explorer",                         false, 0, false},
        {"Chrome browser cache",          local + "\\Google\\Chrome\\User Data\\Default\\Cache",            false, 0, false},
        {"Edge browser cache",            local + "\\Microsoft\\Edge\\User Data\\Default\\Cache",           false, 0, false},
    };
}

inline std::vector<CacheTarget> sj_build_firefox_targets() {
    std::vector<CacheTarget> result;
    std::string appData = sj_env("APPDATA");
    if (appData.empty()) return result;
    fs::path profiles = fs::path(appData) / "Mozilla" / "Firefox" / "Profiles";
    std::error_code ec;
    if (!fs::exists(profiles, ec) || ec) return result;
    for (auto& e : fs::directory_iterator(profiles, ec)) {
        if (!e.is_directory()) continue;
        fs::path cache2 = e.path() / "cache2";
        std::error_code cec;
        if (fs::exists(cache2, cec) && !cec)
            result.push_back({"Firefox cache (" + e.path().filename().string() + ")",
                              cache2, false, 0, false});
    }
    return result;
}

// -------------------------------------------------------------------------
// Full scan: returns a ScanResult with sizes filled in.
// -------------------------------------------------------------------------
inline ScanResult sj_scan() {
    ScanResult sr;
    sr.targets = sj_build_builtin_targets();

    auto ff = sj_build_firefox_targets();
    sr.targets.insert(sr.targets.end(), ff.begin(), ff.end());

    sr.custom_entries = sj_load_custom_folders();
    for (auto& c : sr.custom_entries) {
        std::string label = c.path.filename().string().empty()
                                ? c.path.string()
                                : "[Custom] " + c.path.filename().string();
        sr.targets.push_back({label, c.path, true, 0, false});
    }

    for (auto& t : sr.targets) {
        t.exists     = fs::exists(t.path);
        t.size_bytes = t.exists ? sj_folder_size(t.path) : 0;
        if (t.exists) sr.total_bytes += t.size_bytes;
    }
    return sr;
}

// -------------------------------------------------------------------------
// Clean operations.
// dry_run = true  -> count but don't delete anything.
// -------------------------------------------------------------------------
inline CleanResult sj_clean_folder_contents(const fs::path& dir, bool dry_run = false) {
    CleanResult r;
    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) return r;
    for (auto& entry : fs::directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code fec;
        uintmax_t entry_size = entry.is_regular_file(fec)
                                   ? entry.file_size(fec)
                                   : sj_folder_size(entry.path());
        if (dry_run) {
            r.bytes_freed += entry_size;
            continue;
        }
        std::error_code rm_ec;
        uintmax_t removed = fs::remove_all(entry.path(), rm_ec);
        if (rm_ec || removed == 0) ++r.files_skipped;
        else r.bytes_freed += entry_size;
    }
    return r;
}

inline CleanResult sj_clean_custom_folder(const fs::path& dir, bool dry_run = false) {
    CleanResult r;
    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) return r;
    uintmax_t size_before = sj_folder_size(dir);
    if (dry_run) { r.bytes_freed = size_before; return r; }
    std::error_code rm_ec;
    fs::remove_all(dir, rm_ec);
    if (rm_ec) {
        ++r.files_skipped;
        auto sub = sj_clean_folder_contents(dir, false);
        r.bytes_freed   += sub.bytes_freed;
        r.files_skipped += sub.files_skipped;
    } else {
        r.bytes_freed = size_before;
    }
    return r;
}

inline void sj_empty_recycle_bin() {
#ifdef _WIN32
    SHEmptyRecycleBinA(nullptr, nullptr,
                        SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
#endif
}

// -------------------------------------------------------------------------
// Clean a list of chosen target indices (1-based, last+1 = Recycle Bin).
// Returns total CleanResult.
// -------------------------------------------------------------------------
inline CleanResult sj_clean_chosen(const ScanResult& sr,
                                   const std::vector<int>& chosen,
                                   bool dry_run = false) {
    CleanResult total;
    int recycle_bin_idx = static_cast<int>(sr.targets.size()) + 1;

    for (int n : chosen) {
        if (n == recycle_bin_idx) {
            if (!dry_run) sj_empty_recycle_bin();
            continue;
        }
        int idx = n - 1;
        if (idx < 0 || idx >= static_cast<int>(sr.targets.size())) continue;
        const CacheTarget& t = sr.targets[idx];
        if (!t.exists) continue;

        CleanResult r;
        if (t.is_custom) {
            bool whole = true;
            for (auto& ce : sr.custom_entries)
                if (ce.path == t.path) { whole = ce.whole_folder; break; }
            r = whole ? sj_clean_custom_folder(t.path, dry_run)
                      : sj_clean_folder_contents(t.path, dry_run);
        } else {
            r = sj_clean_folder_contents(t.path, dry_run);
        }
        total.bytes_freed   += r.bytes_freed;
        total.files_skipped += r.files_skipped;
    }
    return total;
}

// -------------------------------------------------------------------------
// Task Scheduler helpers.
// -------------------------------------------------------------------------

// Returns the path of the currently running executable.
inline std::string sj_exe_path() {
#ifdef _WIN32
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return std::string(buf);
#else
    return "";
#endif
}

// Returns the directory containing the currently running executable.
inline fs::path sj_exe_dir() {
    fs::path p(sj_exe_path());
    return p.parent_path();
}

// Writes run-cleanup.bat next to the exe and registers a Windows Task
// Scheduler task named "sapujagat-auto-clean".
//   schedule: "WEEKLY" or "ONLOGON"
//   day:      e.g. "MON" (only used for WEEKLY)
//   time:     e.g. "09:00" (only used for WEEKLY)
// Returns true on success.
inline bool sj_register_scheduled_task(const std::string& schedule,
                                        const std::string& day  = "MON",
                                        const std::string& time = "09:00") {
    fs::path bat = sj_exe_dir() / "run-cleanup.bat";
    std::string exe = sj_exe_path();

    // Write the .bat launcher.
    {
        std::ofstream f(bat);
        if (!f) return false;
        f << "@echo off\n";
        f << "\"" << exe << "\" --yes\n";
    }

    // Build schtasks command.
    std::string cmd;
    if (schedule == "ONLOGON") {
        cmd = "schtasks /Create /TN \"sapujagat-auto-clean\""
              " /TR \"\\\"" + bat.string() + "\\\"\""
              " /SC ONLOGON"
              " /RL HIGHEST /F";
    } else {
        cmd = "schtasks /Create /TN \"sapujagat-auto-clean\""
              " /TR \"\\\"" + bat.string() + "\\\"\""
              " /SC WEEKLY /D " + day +
              " /ST " + time +
              " /RL HIGHEST /F";
    }

#ifdef _WIN32
    int ret = system(cmd.c_str());
    return (ret == 0);
#else
    (void)cmd;
    return false;
#endif
}

// Deletes the "sapujagat-auto-clean" scheduled task.
inline bool sj_unregister_scheduled_task() {
#ifdef _WIN32
    int ret = system("schtasks /Delete /TN \"sapujagat-auto-clean\" /F");
    return (ret == 0);
#else
    return false;
#endif
}
