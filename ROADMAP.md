# ROADMAP.md

The core features (scan, clean, custom folders, --dry-run, --yes, Firefox
detection, colorized output, custom folder delete modes) are all implemented
in `src/main.cpp`.

The two features below are larger architectural changes documented here as
design notes.

---

## 1. Schedule automatic cleanup (Windows Task Scheduler)

You don't need to write any C++ for this part — it's OS configuration
around the `.exe` you already have.

**Steps:**
1. `--yes` flag is already implemented — a scheduled task can't answer an
   interactive prompt, so auto-confirm is a prerequisite (done ✓).
2. Build `cache-cleaner.exe` in release mode.
3. Create a small wrapper batch file, e.g. `run-cleanup.bat`:
   ```bat
   @echo off
   "C:\path\to\cache-cleaner.exe" --yes
   ```
4. Open Task Scheduler → Create Task:
   - General: run whether user is logged in or not; run with highest
     privileges (needed for `Windows\Temp` / `Prefetch`).
   - Triggers: e.g. weekly, or "at log on".
   - Actions: start `run-cleanup.bat`.
5. Test it once with "Run" in Task Scheduler before trusting the
   schedule — check that it actually freed space (you could have it
   append a line to a log file each run for a quick sanity check).

**Planned improvement:** Add a menu option (`[5] Schedule automatic cleanup`)
that auto-generates `run-cleanup.bat` and registers the task via `schtasks`
without the user needing to open Task Scheduler manually.

---

## 2. System tray version

A bigger rewrite: instead of a console app, this runs in the background
with just a tray icon, and shows a small menu when clicked
(Scan & Clean / Manage Custom Folders / Exit).

**Core pieces you'd need (Win32 API):**
1. A hidden message-only window (`CreateWindowEx` with `HWND_MESSAGE`)
   to receive Windows messages — a tray app still needs a window to own
   the icon and process clicks.
2. `NOTIFYICONDATA` + `Shell_NotifyIcon(NIM_ADD, ...)` to create the
   tray icon itself.
3. A custom window message (e.g. `WM_APP + 1`) that Windows sends you
   when the user interacts with the tray icon — handle it in `WindowProc`.
4. On right-click, build a popup menu with `CreatePopupMenu` +
   `AppendMenu`, show it with `TrackPopupMenu` at the cursor position.
5. Wire menu item selection (`WM_COMMAND`) to call the scanning/cleaning
   logic — adapted to not use blocking `std::cin`. Use `MessageBox` for
   confirmation instead.
6. Standard Win32 message loop (`GetMessage` / `DispatchMessage`).

**Suggested approach:** Extract the scanning/cleaning logic
(`build_builtin_targets`, `clean_folder_contents`, etc.) into a shared
`core.h` / `core.cpp` with no `std::cin`/`std::cout` dependency, so both
the CLI version and a new `tray_main.cpp` can call the same core logic.

This is a genuine multi-day project — a good "next project" rather than
something to rush into the same repo.
