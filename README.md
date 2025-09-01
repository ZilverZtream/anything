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
  - Full content extraction for text and common document formats (TXT, source code, PDF, DOCX, etc.), plus native parsing for email (.eml), Outlook archives (.pst) and ebook (.epub) files
  - Native parsers for email, Outlook archives and ebook formats extract sender, subject, author and title without relying on system IFilters
  - Optional content snippets and rich metadata extraction (author, photo EXIF camera/lens, music ID3 artist/album, document title) with bundled cross-platform parsers
  - Archive content indexing (tar, cpio, zip, ar, iso, cab, rar, 7z, xar, lha, and compressed formats like gz/bz2/xz/Z/lzma/lz4/zst) via libarchive.
- Pluggable scanners for cloud drives (OneDrive/Google Drive/pCloud/Dropbox).
- Native WSL filesystem indexing without wsl.exe overhead.
- Experimental macOS virtualization disk indexing for Docker, Parallels and VMware images.
- Incremental content indexing avoids re-scanning unchanged files.
- Hybrid indexing levels for name-only, metadata-light, or full content extraction.
- Extensible plugin system so third parties can add new scanners.
- Optional code-aware indexing plugin that parses source code (C/C++/Rust/Go/C#/VB.NET/Java/Python) and indexes function, class, and variable definitions for queries like `function:render_widget`.
- Bloom filters for quick name filtering.
- Fuzzy filename and content search using Levenshtein distance.
- Optional Windows taskbar search integration (enable in the Settings tab to register an `anything:` URL protocol and launch searches from the taskbar).
- Switchable dark and light themes in the graphical interface.
- Command-line tools:
  - `anything.exe index` — build or update the database
  - `search.exe` — query the database with filters (size, date, path, extension, content, author, camera, lens, artist, album, title, regex, etc.). Add `--json` to emit results and errors as structured JSON. Administrative flags `--start-indexer` and `--pause-indexer` control the indexing service.

### Enterprise Features (paywalled)
The source tree contains placeholders for several enterprise-only capabilities.
Define the `ENTERPRISE` macro at build time to enable the stubs:

- Network share indexing
- Permission-aware search results
- Audit logging of search queries
- Active Directory based authentication
- Centralized deployment helpers

## Archive Content Indexing
Anything can open a wide range of archives and index contained filenames so a search result can point directly to a file inside formats such as tar, cpio, zip, ar, iso, cab, rar, 7z, xar, and lha, including compressed variants (gz, bz2, xz, Z, lzma, lz4, zst).

## Cloud Drive Integration
Stub scanners exist for OneDrive, Google Drive, and Dropbox. They will use official APIs to pull file listings and merge them into the local database.

## Native WSL File System Indexing
The WSL scanner reads the ext4.vhdx virtual disk directly to enumerate files and capture full metadata without shelling out to `wsl.exe` for every file.

## macOS Virtual Machine Disk Indexing
The MacVM scanner can read ext4-based virtual disk images used by Docker, Parallels or VMware on macOS. It opens the disk image directly and walks the contained filesystem without needing to boot the virtual machine.

## Plugin API
Custom data source scanners can be implemented as DLLs. Place compiled plugins in a `plugins` folder next to the executable and they will be loaded at startup. The `plugin.h` header documents the API each DLL must expose.

Example plugins include `ocr_plugin.c`, which uses Tesseract OCR to extract and index text from images and scanned PDFs, `registry_plugin.c`, a Windows-only plugin that scans the system registry for searchable keys and values, `gmail_plugin.c`, a plugin that indexes Gmail messages via the Google Gmail API using OAuth 2.0, and `microsoft_mail_plugin.c`, which indexes Outlook.com and Office 365 mail via the Microsoft Graph API with OAuth 2.0 tokens loaded from secure environment variables or token files.

The Microsoft Mail plugin looks for an access token in the `MS_MAIL_TOKEN` environment variable. As an alternative, a token may be stored on disk with user-only permissions in a file pointed to by `MS_MAIL_TOKEN_FILE` (defaulting to `~/.anything/ms_mail_token`). This allows credentials to be kept separate from the executable while still enabling automated scans.

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
`ui_imgui.cpp` provides a Dear ImGui front end that streams instant search results from the LMDB database. It features a preview pane with highlighted text snippets, syntax-colored code previews, or image thumbnails and an "Advanced Search Builder" for extension, size, date, path, regex and whole-word filters.

Compile with `HAS_IMGUI` (and optionally `HAS_STB_IMAGE` for image previews) and link against ImGui, GLFW, OpenGL, LMDB, stb_image and the Win32 shlwapi library:

```
cl /std:c++17 /EHsc ui_imgui.cpp imgui*.cpp glfw3.lib opengl32.lib lmdb.lib shlwapi.lib
```

Without these libraries the `run_ui` function prints a message and exits so command-line tools remain usable.

## Memory and Buffer Optimizations
- Configurable `g_sort_buffer_size` (default 256MB) controls how much data is sorted in memory before spilling to temporary files.
- Sort buffers pack variable-length values using length prefixes and include incremental 64-bit hashing helpers for composite sort keys.
- If the data set exceeds the in-memory buffer, an external sort writes sorted chunks to temp files and merges them with a k-way merge.
- Work memory is determined dynamically at runtime via `GlobalMemoryStatusEx` to adapt to available RAM.
