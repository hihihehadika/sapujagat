# ROADMAP.md

All planned features are now implemented. See the feature summary below.

---

## Feature Summary

### CLI app (`sapujagat.exe`) — `src/main.cpp`
- Scan & clean: Windows Temp, Prefetch, Update cache, Explorer thumbnails,
  Chrome, Edge, Firefox (all profiles), Recycle Bin
- Custom folders with per-folder delete mode (entire folder or contents-only)
- `--dry-run` flag: preview what would be deleted
- `--yes` flag: skip confirmation prompt (for scripting)
- Colorized output (yellow = sizes, green = done, red = skipped)
- **[NEW] Schedule automatic cleanup** via Windows Task Scheduler (menu option 4)
  — generates `run-cleanup.bat` and registers task via `schtasks`

### System Tray app (`sapujagat-tray.exe`) — `src/tray_main.cpp`
- Runs silently in the background (no console window)
- Right-click tray icon for popup menu:
  - **Scan & Clean All** — scans then asks confirmation, cleans in background thread
  - **Dry Run** — shows what would be deleted without deleting
  - **Show Last Scan Status** — displays cached scan results
  - **Manage Schedule** — create/remove Task Scheduler task via dialog
  - **Exit**
- Double-click tray icon: quick dry run
- Balloon tip on startup

### Shared core (`src/core.h`)
All scan/clean/scheduler logic lives here so both apps share the same code.
No `std::cin` / `std::cout` dependency in core — safe to call from any context.

---

## Build

**CLI (MinGW):**
```
g++ -std=c++17 -O2 -static src/main.cpp -o sapujagat.exe
```

**Tray app (MinGW):**
```
g++ -std=c++17 -O2 -static -mwindows src/tray_main.cpp -o sapujagat-tray.exe -lshell32 -lcomctl32
```

**CLI (MSVC):**
```
cl /std=c++17 /EHsc /O2 src\main.cpp /Fe:sapujagat.exe
```

**Tray app (MSVC):**
```
cl /std:c++17 /EHsc /O2 /DWIN32 src\tray_main.cpp /Fe:sapujagat-tray.exe shell32.lib comctl32.lib /link /SUBSYSTEM:WINDOWS
```

---

## Future Ideas

- Bundle an `.ico` file and use it for the tray icon (instead of the generic shield).
- Log cleanup results to a file for scheduled runs.
- Per-category dry-run: show exact file list before deleting.
- GUI installer (NSIS or Inno Setup).
