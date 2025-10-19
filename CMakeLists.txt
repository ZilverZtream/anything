# =============================================================================
# CMake Build Script for the "Anything" Project (Corrected Linker Targets)
# =============================================================================
cmake_minimum_required(VERSION 3.16)
project(Anything C CXX)

# --- Project Settings ---
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins")

if(MSVC)
    add_compile_options(/arch:AVX2 /W4 /bigobj)
else()
    add_compile_options(-msse4.1 -mavx2 -Wextra -Wall)
endif()

# --- Find Dependencies using vcpkg ---
find_package(unofficial-lmdb CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(cJSON CONFIG REQUIRED)
find_package(libgit2 CONFIG REQUIRED)
find_package(Tesseract CONFIG REQUIRED)
find_package(unofficial-gumbo CONFIG REQUIRED)
find_package(LibArchive REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(libzip CONFIG REQUIRED)
find_package(libpst CONFIG REQUIRED)

if(UNIX AND NOT APPLE)
    find_package(e2fsprogs CONFIG REQUIRED)
endif()


# --- Define Public Include Directory ---
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# =============================================================================
# Collect Source Files
# =============================================================================
file(GLOB CORE_APP_SOURCES "src/app/*.c")
file(GLOB CORE_ENGINE_SOURCES "src/core/*.c")
file(GLOB CORE_SYSTEM_SOURCES "src/system/*.c")
file(GLOB CORE_ENTERPRISE_SOURCES "src/enterprise/*.c")
file(GLOB SERVICE_SOURCES "src/services/*.c")
file(GLOB UI_SOURCES "src/ui/*.cpp")
file(GLOB PLATFORM_WIN_SOURCES "src/platform/ntfs.c" "src/platform/scanner_win.cpp")
file(GLOB PLATFORM_APPLE_SOURCES "src/platform/apfs.c" "src/platform/macvm.c" "src/platform/scanner_macos.c")
file(GLOB PLATFORM_UNIX_SOURCES "src/platform/scanner_linux.c" "src/platform/wsl.c")

# Group shared sources used by the main executable and plugins
set(ANYTHING_CORE_SOURCES
    ${CORE_APP_SOURCES}
    ${CORE_ENGINE_SOURCES}
    ${CORE_SYSTEM_SOURCES}
    ${SERVICE_SOURCES}
    ${CORE_ENTERPRISE_SOURCES}
)

# =============================================================================
# Shared Core Library
# =============================================================================

add_library(anything_core STATIC ${ANYTHING_CORE_SOURCES})

if(WIN32)
    target_sources(anything_core PRIVATE ${PLATFORM_WIN_SOURCES})
elseif(APPLE)
    target_sources(anything_core PRIVATE ${PLATFORM_APPLE_SOURCES})
elseif(UNIX)
    target_sources(anything_core PRIVATE ${PLATFORM_UNIX_SOURCES})
endif()

target_link_libraries(anything_core PUBLIC
    unofficial::lmdb::lmdb
    LibArchive::LibArchive
    CURL::libcurl
    cjson
    libzip::zip
    libpst::libpst
)

if(UNIX AND NOT APPLE)
    target_link_libraries(anything_core PUBLIC e2fsprogs::ext2fs)
endif()

if(WIN32)
    target_link_libraries(anything_core PUBLIC Shlwapi.lib Ole32.lib Query.lib)
endif()

# =============================================================================
# Define Executable Targets
# =============================================================================

add_executable(anything)

add_executable(anything_ui)

target_sources(anything_ui PRIVATE
    ${UI_SOURCES}
    "third_party/cJSON/cJSON.c"
)

add_executable(search_test "tests/search_test.c")

# =============================================================================
# Link Libraries to Executables
# =============================================================================

target_link_libraries(anything PRIVATE
    anything_core
)

target_link_libraries(anything_ui PRIVATE
    anything_core
    glfw
    CURL::libcurl
    cjson
)

target_link_libraries(search_test PRIVATE anything_core)

if(WIN32)
    target_link_libraries(anything_ui PRIVATE gdiplus.lib d2d1.lib dwrite.lib Shlwapi.lib Shell32.lib)
endif()

# =============================================================================
# Plugins (built as shared libraries/DLLs)
# =============================================================================
macro(add_anything_plugin name sources)
    add_library(${name} MODULE ${sources})
    target_link_libraries(${name} PRIVATE
        anything_core
        cjson
        CURL::libcurl
    )
endmacro()

add_anything_plugin(code_plugin "plugins/code/code_plugin.c")
add_anything_plugin(git_plugin "plugins/code/git_plugin.c")
add_anything_plugin(icloud_plugin "plugins/data_sources/icloud_plugin.c")
add_anything_plugin(gmail_plugin "plugins/data_sources/gmail_plugin.c")
add_anything_plugin(msmail_plugin "plugins/data_sources/microsoft_mail_plugin.c")
add_anything_plugin(web_archive_plugin "plugins/data_sources/web_archive_plugin.c")
add_anything_plugin(ocr_plugin "plugins/media/ocr_plugin.c")
add_anything_plugin(duplicates_plugin "plugins/system/duplicates_plugin.c")
add_anything_plugin(registry_plugin "plugins/system/registry_plugin.c")

# --- Link plugin-specific dependencies (with corrected target names) ---
# --- FIX: Changed libgit2::git2 to libgit2::libgit2package ---
target_link_libraries(git_plugin PRIVATE libgit2::libgit2package)
target_link_libraries(ocr_plugin PRIVATE Tesseract::libtesseract)
target_link_libraries(web_archive_plugin PRIVATE unofficial::gumbo::gumbo unofficial::sqlite3::sqlite3)
