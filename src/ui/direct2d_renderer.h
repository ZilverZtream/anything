#ifndef DIRECT2D_RENDERER_H
#define DIRECT2D_RENDERER_H

#include <stddef.h>
#include <stdbool.h>
#include <wchar.h>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VirtualListHit {
    const wchar_t* name;
    const wchar_t* subtext;
} VirtualListHit;

typedef struct VirtualListView {
    const VirtualListHit* const* hits;
    size_t hit_count;
    size_t visible_start;
    size_t visible_end;
    float  row_height;
    float  fractional_offset;
    size_t selected_index;
    bool   has_selection;
} VirtualListView;

#ifdef _WIN32
typedef struct Direct2DRenderer Direct2DRenderer;

Direct2DRenderer* direct2d_renderer_create(HWND hwnd);
void direct2d_renderer_destroy(Direct2DRenderer* renderer);
bool direct2d_renderer_resize(Direct2DRenderer* renderer, unsigned int width, unsigned int height);
void direct2d_renderer_paint(Direct2DRenderer* renderer, const VirtualListView* view);
#else

typedef struct Direct2DRenderer { int unused; } Direct2DRenderer;

static inline Direct2DRenderer* direct2d_renderer_create(void* hwnd){ (void)hwnd; return NULL; }
static inline void direct2d_renderer_destroy(Direct2DRenderer* renderer){ (void)renderer; }
static inline bool direct2d_renderer_resize(Direct2DRenderer* renderer, unsigned int width, unsigned int height){ (void)renderer; (void)width; (void)height; return false; }
static inline void direct2d_renderer_paint(Direct2DRenderer* renderer, const VirtualListView* view){ (void)renderer; (void)view; }

#endif

#ifdef __cplusplus
}
#endif

#endif /* DIRECT2D_RENDERER_H */
