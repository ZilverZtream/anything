# Anything Project

**Anything** is a high-performance file system indexer and search engine.
It scans drives (via NTFS USN journal, generic directory walking, or native Linux/macOS watchers), stores file metadata in an LMDB database, and allows fast full-text search using trigram indices and filters.

## Features
- Multi-threaded file system scanning (NTFS, generic, and POSIX inotify/FSEvents).
- Lock-free MPMC queue for fast inter-thread communication.
- Efficient LMDB storage with multiple indices:
  - Filename
  - Parent path
  - File size
  - Modified date
  - Extension
  - Path hierarchy
  - Trigram-based full-text search
  - Full Boolean query syntax (AND/OR/NOT with grouping)
  - Full content extraction for text and common document formats (TXT, source code, PDF, DOCX, etc.)
  - Optional content snippets and rich metadata extraction (author, photo EXIF camera/lens, music ID3 artist/album, document title)
- Archive content indexing (ZIP/RAR/7z) via libzip.
- Pluggable scanners for cloud drives (OneDrive/Google Drive/pCloud/Dropbox).
- Experimental WSL filesystem indexing.
- Extensible plugin system so third parties can add new scanners.
- Bloom filters for quick name filtering.
- Fuzzy filename and content search using Levenshtein distance.
- Command-line tools:
  - `anything.exe index` — build or update the database
  - `search.exe` — query the database with filters (size, date, path, extension, content, author, camera, lens, artist, album, title, regex, etc.). Add `--json` to emit results as structured JSON

## Archive Content Indexing
Anything can open common archives and index contained filenames so a search result can point directly to a file inside a ZIP, RAR, or 7z archive.

## Cloud Drive Integration
Stub scanners exist for OneDrive, Google Drive, and Dropbox. They will use official APIs to pull file listings and merge them into the local database.

## Native WSL File System Indexing
An experimental scanner illustrates how to traverse and index files stored under the Windows Subsystem for Linux.

## Plugin API
Custom data source scanners can be implemented as DLLs. Place compiled plugins in a `plugins` folder next to the executable and they will be loaded at startup. The `plugin.h` header documents the API each DLL must expose.

## Build
This project is written in **C** for Windows.

### Requirements
- **Microsoft Visual Studio** (with C compiler and Windows SDK)
- **LMDB** library (include and link `lmdb.h`, `lmdb.lib`)

### Example Build Command (Visual Studio Developer Command Prompt)
```bat
cl /O2 /MD anything.c database.c exfat.c ntfs.c search.c util.c plugin.c lmdb.lib shlwapi.lib
```

## Graphical User Interface
`ui_imgui.cpp` provides a Dear ImGui front end that streams instant search results from the LMDB database. It features a preview pane with highlighted text snippets or image thumbnails and an "Advanced Search Builder" for extension, size, date, path, regex and whole-word filters.

Compile with `HAS_IMGUI` (and optionally `HAS_STB_IMAGE` for image previews) and link against ImGui, GLFW, OpenGL, LMDB, stb_image and the Win32 shlwapi library:

```
cl /std:c++17 /EHsc ui_imgui.cpp imgui*.cpp glfw3.lib opengl32.lib lmdb.lib shlwapi.lib
```

Without these libraries the `run_ui` function prints a message and exits so command-line tools remain usable.
