# Anything Project

**Anything** is a high-performance Windows file system indexer and search engine.  
It scans drives (via NTFS USN journal or generic directory walking), stores file metadata in an LMDB database, and allows fast full-text search using trigram indices and filters.

## Features
- Multi-threaded file system scanning (NTFS and generic).
- Lock-free MPMC queue for fast inter-thread communication.
- Efficient LMDB storage with multiple indices:
  - Filename
  - Parent path
  - File size
  - Modified date
  - Extension
  - Path hierarchy
  - Trigram-based full-text search
- Bloom filters for quick name filtering.
- Command-line tools:
  - `anything.exe index` — build or update the database
  - `search.exe` — query the database with filters (size, date, path, extension, regex, etc.)

## Build
This project is written in **C** for Windows.

### Requirements
- **Microsoft Visual Studio** (with C compiler and Windows SDK)
- **LMDB** library (include and link `lmdb.h`, `lmdb.lib`)

### Example Build Command (Visual Studio Developer Command Prompt)
```bat
cl /O2 /MD anything.c database.c exfat.c ntfs.c search.c util.c lmdb.lib shlwapi.lib
