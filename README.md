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
  - Full content extraction for text and common document formats (TXT, source code, PDF, DOCX, etc.)
  - Optional content snippets and simple author metadata for text files
- Archive content indexing (ZIP/RAR/7z) via libzip.
- Pluggable scanners for cloud drives (OneDrive/Google Drive/Dropbox).
- Experimental WSL filesystem indexing.
- Bloom filters for quick name filtering.
- Command-line tools:
  - `anything.exe index` — build or update the database
  - `search.exe` — query the database with filters (size, date, path, extension, content, author, regex, etc.)

## Archive Content Indexing
Anything can open common archives and index contained filenames so a search result can point directly to a file inside a ZIP, RAR, or 7z archive.

## Cloud Drive Integration
Stub scanners exist for OneDrive, Google Drive, and Dropbox. They will use official APIs to pull file listings and merge them into the local database.

## Native WSL File System Indexing
An experimental scanner illustrates how to traverse and index files stored under the Windows Subsystem for Linux.

## Build
This project is written in **C** for Windows.

### Requirements
- **Microsoft Visual Studio** (with C compiler and Windows SDK)
- **LMDB** library (include and link `lmdb.h`, `lmdb.lib`)

### Example Build Command (Visual Studio Developer Command Prompt)
```bat
cl /O2 /MD anything.c database.c exfat.c ntfs.c search.c util.c lmdb.lib shlwapi.lib
