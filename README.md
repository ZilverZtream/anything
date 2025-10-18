**Anything***: Instant Desktop Search for Everything

Anything is a high-performance desktop search engine designed to find not only your files by name but also the content within them—instantly. Built with a focus on speed, efficiency, and extensibility, it combines the filename-finding velocity of tools like Voidtools Everything with the deep content indexing capabilities of a personal knowledge base.

It achieves this through a highly optimized, multi-stage indexing pipeline, a sophisticated query engine, and a fluid, GPU-accelerated user interface.

✨ **Key Features**
Instant File & Folder Search: Utilizes the NTFS USN Journal on Windows for real-time file change detection, providing instantaneous name-based search results. For other filesystems, it employs a highly parallel, work-stealing directory scanner.

Full-Text Content Search: The core of "Anything". It indexes the text content of a vast array of file types, allowing you to find code snippets, document phrases, or any string within your files.

Source Code: Indexes plain text from .c, .cpp, .h, .py, .java, .cs, and many more.

Documents & Emails: Uses native OS interfaces (IFilter on Windows) to extract text from .pdf, .docx, .pptx, and email files like .eml and .pst.

Rich, GPU-Accelerated UI: The interface is built with ImGui and features a Direct2D backend on Windows for an exceptionally smooth, high-framerate experience that remains fluid even when scrolling through millions of results.

Result Virtualization: Only renders visible rows, ensuring minimal resource usage regardless of the result count.

Lazy-Loaded Previews: Generates and displays thumbnails and content previews on demand, preventing UI lag.

Integrated Code & Markdown Viewer: Previews source code with syntax highlighting and renders Markdown files directly in the preview pane.

Extensible Plugin System: "Anything" can be expanded to index virtually any data source through a simple plugin architecture. Included plugins turn it into a unified search hub for your entire digital life:

Git History: Indexes commit messages, authors, and code diffs across all local Git repositories.

Web Archives: Fetches and indexes the full text of your browser history and bookmarks from Chrome, Firefox, Edge, and more.

Image Content (OCR): Uses Tesseract OCR to find text inside images and scanned documents.

Cloud Mail: Indexes recent emails from Gmail, iCloud, and Microsoft Mail accounts.

System Registry: Makes the entire Windows Registry instantly searchable.

Advanced Query Syntax: Combine keywords with boolean operators (AND, OR, NOT) and powerful filters like ext:, content:, author:, and size:> to precisely target your search.

High Performance by Design:

Persistent Index: Unlike some tools, "Anything" creates a persistent database and only indexes changes on subsequent startups, leading to near-instant launch times after the initial scan.

Advanced Indexing: Utilizes Trigram indexing and Bloom filters to dramatically accelerate substring and content searches.

SIMD Optimization: Critical code paths like trigram extraction and hash calculation are accelerated with AVX2 and SSE4.1 intrinsics.

Efficient Storage: Leverages LMDB, a high-performance, memory-mapped key-value store, as its database backend.

🚀 **Building From Source**
"Anything" is a C/C++ project that uses CMake for building.

Dependencies
You will need to install the following libraries before compiling:

LMDB: Core database engine.

GLFW: Windowing and input for the UI.

libcurl: For cloud plugins.

cJSON: For parsing JSON in plugins.

libgit2: For the Git plugin.

Tesseract: For the OCR plugin.

gumbo-parser: For parsing HTML in the web archive plugin.

libarchive: For indexing archive file contents.

SQLite3: For reading browser history databases.

Windows (using vcpkg):

Bash

vcpkg install lmdb glfw3 libcurl cjson libgit2 tesseract gumbo-parser libarchive sqlite3
macOS (using Homebrew):

Bash

brew install lmdb glfw libcurl cjson libgit2 tesseract gumbo-parser libarchive sqlite
Ubuntu/Debian (using apt):

Bash

sudo apt-get install liblmdb-dev libglfw3-dev libcurl4-openssl-dev libcjson-dev libgit2-dev libleptonica-dev libtesseract-dev libgumbo-dev libarchive-dev libsqlite3-dev
Compilation Steps
Clone the repository:

Bash

git clone https://github.com/your-username/anything.git
cd anything
Configure with CMake:

On Windows, if using vcpkg, specify the toolchain file.

Bash

cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[path-to-vcpkg]/scripts/buildsystems/vcpkg.cmake
On Linux/macOS:

Bash

cmake -B build -S .
Build the project:

Bash

cmake --build build --config Release
Run: The executables (anything.exe, search.exe) will be located in the build/Release or build directory.

💻 **Usage**
"Anything" consists of two main executables: anything.exe for indexing and ui_imgui.exe (or similar) for the search interface.

1. Indexing
First, you must build the database. You can index a specific folder or all drives.

Index a specific drive or folder:

Bash

anything.exe index --db anything.mdb --root C:\
Index all fixed drives on Windows:

Bash

anything.exe index --db anything.mdb --all-drives
The first run will take some time as it builds the initial index. Subsequent runs will be much faster as they only process changes.

2. Searching
Launch the UI executable. It will automatically load the anything.mdb database from its directory and provide an interactive search window.

🤝 **Contributing**
Contributions are welcome! If you have a feature idea, a bug fix, or a new plugin you'd like to build, please open an issue or submit a pull request.

📜 **License**
This project is licensed under the MIT License. See the LICENSE file for details.
