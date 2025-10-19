#pragma once

// 1. Core System Headers
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <synchapi.h>
#include <wincrypt.h>
#include <winioctl.h>
#include <winreg.h>
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// 2. Third-Party Library Headers
#if defined(__has_include)
#if __has_include(<lmdb.h>)
#include <lmdb.h>
#endif
#if __has_include(<zip.h>)
#include <zip.h>
#endif
#if !defined(_WIN32) && __has_include(<libpst/libpst.h>)
#include <libpst/libpst.h>
#endif
#ifdef _WIN32
#if __has_include(<filter.h>)
#include <filter.h>
#endif
#if __has_include(<filterr.h>)
#include <filterr.h>
#endif
#endif
#else
#include <lmdb.h>
#include <zip.h>
#ifndef _WIN32
#include <libpst/libpst.h>
#endif
#ifdef _WIN32
#include <filter.h>
#endif
#endif

// 3. Project Core Headers
#ifdef __cplusplus
extern "C" {
#endif
#include "anything/anything.h"
#include "anything/archive.h"
#include "anything/cloud.h"
#include "anything/config.h"
#include "anything/database.h"
#include "anything/enterprise.h"
#include "anything/metadata.h"
#include "anything/plugin.h"
#include "anything/result.h"
#include "anything/scanner.h"
#include "anything/util.h"
#ifdef __cplusplus
}
#endif

#if !defined(HAS_IMGUI)
#if defined(__has_include)
#if __has_include(<imgui.h>) && __has_include(<imgui_impl_glfw.h>) && __has_include(<imgui_impl_opengl3.h>)
#define HAS_IMGUI 1
#endif
#endif
#endif
