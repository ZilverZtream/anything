# =============================================================================
# CMake Build Script for the "Anything" Project
# =============================================================================
cmake_minimum_required(VERSION 3.16)
project(Anything C CXX)

# --- Project Settings ---
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Enable SIMD instruction sets where available
if(MSVC)
    add_compile_options(/arch:AVX2)
else()
    add_compile_options(-msse4.1 -mavx2)
endif()

# --- Find Dependencies using vcpkg ---
find_package(glfw3 REQUIRED)
find_package(CURL REQUIRED)
find_package(cjson REQUIRED)
find_package(libgit2 REQUIRED)
find_package(Tesseract REQUIRED)
find_package(Gumbo REQUIRED)
find_package(LibArchive REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(unofficial-lmdb REQUIRED)
find_package(e2fsprogs REQUIRED)
find_package(libzip REQUIRED)

# --- Define Public Include Directory ---
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cJSON
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
)

# =============================================================================
# Core Application & UI Executables
# =============================================================================

# --- Main Indexer Executable ---
add_executable(anything
    src/app/anything.c
    src/app/config.c
    src/core/database.c
    src/core/util.c
    src/services/archive.c
    src/services/cloud.c
    src/services/exfat.c
    src/services/metadata.c
    src/services/network.c
    src/system/plugin.c
)

# --- UI Executable ---
add_executable(ui_imgui
    src/ui/ui_imgui.cpp
    src/ui/TextEditor.cpp
    src/core/database.c # Share implementation
    src/core/util.c
    third_party/cJSON/cJSON.c
)

# --- Search CLI Executable ---
add_executable(search_cli
    src/core/search.c
    src/core/database.c
    src/core/util.c
    src/enterprise/enterprise.c
)

# --- Platform-Specific Source Files ---
if(WIN32)
    target_sources(anything PRIVATE
        src/platform/ntfs.c
        src/platform/scanner_win.cpp
        src/platform/wsl.c
        src/ui/direct2d_renderer.cpp
    )
    target_sources(search_cli PRIVATE src/platform/ntfs.c)
elseif(APPLE)
    target_sources(anything PRIVATE
        src/platform/apfs.c
        src/platform/macvm.c
        src/platform/scanner_macos.c
    )
elseif(UNIX)
    target_sources(anything PRIVATE src/platform/scanner_linux.c)
endif()

# --- Link Libraries to Executables ---
target_link_libraries(anything PRIVATE
    unofficial::lmdb::lmdb
    LibArchive::LibArchive
    ${CMAKE_THREAD_LIBS_INIT}
    e2fsprogs::ext2fs
    libzip::zip
)

target_link_libraries(ui_imgui PRIVATE
    unofficial::lmdb::lmdb
    glfw
    ${CMAKE_DL_LIBS}
    CURL::libcurl
    cjson
)

target_link_libraries(search_cli PRIVATE
    unofficial::lmdb::lmdb
    e2fsprogs::ext2fs
    libzip::zip
)

if(WIN32)
    target_link_libraries(anything PRIVATE Shlwapi.lib Ole32.lib Query.lib)
    target_link_libraries(ui_imgui PRIVATE gdiplus.lib d2d1.lib dwrite.lib)
    target_link_libraries(search_cli PRIVATE Shlwapi.lib)
endif()

# =============================================================================
# Plugins (built as shared libraries/DLLs)
# =============================================================================
macro(add_anything_plugin name sources)
    add_library(${name} MODULE ${sources})
    target_link_libraries(${name} PRIVATE
        unofficial::lmdb::lmdb
        cjson
        CURL::libcurl
    )
endmacro()

add_anything_plugin(code_plugin plugins/code/code_plugin.c)
add_anything_plugin(git_plugin plugins/code/git_plugin.c)
add_anything_plugin(icloud_plugin plugins/data_sources/icloud_plugin.c)
add_anything_plugin(gmail_plugin plugins/data_sources/gmail_plugin.c)
add_anything_plugin(msmail_plugin plugins/data_sources/microsoft_mail_plugin.c)
add_anything_plugin(ocr_plugin plugins/media/ocr_plugin.c)
add_anything_plugin(duplicates_plugin plugins/system/duplicates_plugin.c)
add_anything_plugin(registry_plugin plugins/system/registry_plugin.c)
add_anything_plugin(web_archive_plugin plugins/system/web_archive_plugin.c)

target_link_libraries(git_plugin PRIVATE Git2::git2)
target_link_libraries(ocr_plugin PRIVATE Tesseract::libtesseract)
target_link_libraries(web_archive_plugin PRIVATE Gumbo::gumbo SQLite::SQLite3)

# =============================================================================
# Test Executable
# =============================================================================
add_executable(search_test tests/search_test.c src/core/util.c)
