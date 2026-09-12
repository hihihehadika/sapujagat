# cache-cleaner

Simple Windows CLI tool that scans and cleans common cache/junk folders —
no more manually digging through File Explorer to clear temp files.

## What it cleans

**Built-in categories:**
- Windows Temp (user & system)
- Windows Prefetch
- Windows Update download cache
- Explorer thumbnail cache
- Chrome / Edge browser cache
- Firefox browser cache (auto-detects all profiles)
- Recycle Bin

**Custom folders:** add your own folders through the app's menu.
These are saved to `%APPDATA%\cache-cleaner\custom_folders.txt`, so they
persist across sessions. Custom folders also support two delete modes:
- **Entire folder** — deletes the folder and everything inside it
- **Contents only** — deletes only the contents, keeping the folder itself

The tool **scans first and shows the size of each category** before deleting
anything — you pick what to clean, nothing happens automatically.

## Flags

| Flag        | Description                                                |
|-------------|------------------------------------------------------------|
| `--dry-run` | Show what *would* be deleted without actually deleting anything. |
| `--yes`     | Skip the confirmation prompt (useful for scripting / Task Scheduler). |

## Build

**MSVC (Developer Command Prompt for VS):**
```
cl /std:c++17 /EHsc /O2 src\main.cpp /Fe:cache-cleaner.exe
```

**MinGW-w64 (g++):**
```
g++ -std=c++17 -O2 src/main.cpp -o cache-cleaner.exe
```

## Run

```
cache-cleaner.exe
```

> **Run as Administrator** for full access — `Windows\Temp`, `Prefetch`,
> and the Windows Update cache all require elevated permissions to clean
> fully. Without admin rights the tool still works, it'll just skip files
> it can't touch (and tell you how many it skipped).

## Safety notes

- Files currently in use (e.g. a browser that's open) are **skipped
  automatically**, not force-closed — the tool won't crash your open apps.
- Nothing is deleted without you explicitly selecting the category and
  confirming (unless `--yes` is passed).
- This clears *cache*, not your actual data (bookmarks, saved passwords,
  documents, etc. are untouched). Close your browser before cleaning its
  cache as a good practice.
- `--dry-run` mode shows exactly what would be deleted — use it to preview
  before committing to a clean.

## Note on custom folders

Unlike built-in cache categories (where only the *contents* get deleted,
keeping the folder itself intact), you can choose the delete mode per
custom folder when adding it:
- **Entire folder** mode — removes the folder completely
- **Contents only** mode — removes only what's inside, keeps the folder

Removing a folder from the list (menu option 3) only removes it from
`custom_folders.txt` — it does **not** delete the actual folder from disk.

## Roadmap

See [ROADMAP.md](./ROADMAP.md) for planned features:
- [ ] Schedule automatic cleanup via Windows Task Scheduler
- [ ] System tray app (background, tray icon with popup menu)
