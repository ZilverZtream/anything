#include "core/pch.h"
#include "direct2d_renderer.h"

#ifdef _WIN32
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <new>
#include <string>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

struct Direct2DRenderer {
    HWND hwnd = nullptr;
    bool com_initialized = false;
    D2D1_SIZE_U pixel_size = {0, 0};
    float font_size = 15.0f;
    float background_color[4] = {0.05f, 0.05f, 0.05f, 1.0f};
    float selection_color[4]  = {0.20f, 0.37f, 0.64f, 0.35f};

    ComPtr<ID2D1Factory> factory;
    ComPtr<IDWriteFactory> dwrite_factory;
    ComPtr<IDWriteTextFormat> text_format;
    ComPtr<ID2D1HwndRenderTarget> render_target;
    ComPtr<ID2D1SolidColorBrush> text_brush;
    ComPtr<ID2D1SolidColorBrush> highlight_brush;
};

static HRESULT direct2d_renderer_create_device_resources(Direct2DRenderer* renderer) {
    if (!renderer || renderer->render_target) {
        return S_OK;
    }
    if (!renderer->factory) {
        return E_FAIL;
    }

    if (renderer->pixel_size.width == 0 || renderer->pixel_size.height == 0) {
        RECT rc = {0};
        GetClientRect(renderer->hwnd, &rc);
        UINT width = rc.right > rc.left ? static_cast<UINT>(rc.right - rc.left) : 1u;
        UINT height = rc.bottom > rc.top ? static_cast<UINT>(rc.bottom - rc.top) : 1u;
        renderer->pixel_size = D2D1::SizeU(width, height);
    }

    HRESULT hr = renderer->factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(renderer->hwnd, renderer->pixel_size),
        renderer->render_target.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }

    renderer->render_target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    hr = renderer->render_target->CreateSolidColorBrush(
        D2D1::ColorF(0.92f, 0.92f, 0.92f, 1.0f),
        renderer->text_brush.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }

    hr = renderer->render_target->CreateSolidColorBrush(
        D2D1::ColorF(renderer->selection_color[0], renderer->selection_color[1],
                     renderer->selection_color[2], renderer->selection_color[3]),
        renderer->highlight_brush.ReleaseAndGetAddressOf());
    return hr;
}

Direct2DRenderer* direct2d_renderer_create(HWND hwnd) {
    if (!hwnd) {
        return nullptr;
    }

    Direct2DRenderer* renderer = new (std::nothrow) Direct2DRenderer();
    if (!renderer) {
        return nullptr;
    }
    renderer->hwnd = hwnd;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        renderer->com_initialized = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        renderer->com_initialized = false;
    } else {
        delete renderer;
        return nullptr;
    }

    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, renderer->factory.GetAddressOf());
    if (FAILED(hr)) {
        direct2d_renderer_destroy(renderer);
        return nullptr;
    }

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(renderer->dwrite_factory.GetAddressOf()));
    if (FAILED(hr)) {
        direct2d_renderer_destroy(renderer);
        return nullptr;
    }

    hr = renderer->dwrite_factory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        renderer->font_size, L"en-us", renderer->text_format.GetAddressOf());
    if (FAILED(hr)) {
        direct2d_renderer_destroy(renderer);
        return nullptr;
    }
    renderer->text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    renderer->text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    RECT rc = {0};
    GetClientRect(hwnd, &rc);
    renderer->pixel_size = D2D1::SizeU(
        rc.right > rc.left ? static_cast<UINT>(rc.right - rc.left) : 1u,
        rc.bottom > rc.top ? static_cast<UINT>(rc.bottom - rc.top) : 1u);

    if (FAILED(direct2d_renderer_create_device_resources(renderer))) {
        direct2d_renderer_destroy(renderer);
        return nullptr;
    }

    return renderer;
}

void direct2d_renderer_destroy(Direct2DRenderer* renderer) {
    if (!renderer) {
        return;
    }
    renderer->highlight_brush.Reset();
    renderer->text_brush.Reset();
    renderer->render_target.Reset();
    renderer->text_format.Reset();
    renderer->dwrite_factory.Reset();
    renderer->factory.Reset();
    if (renderer->com_initialized) {
        CoUninitialize();
    }
    delete renderer;
}

bool direct2d_renderer_resize(Direct2DRenderer* renderer, unsigned int width, unsigned int height) {
    if (!renderer) {
        return false;
    }
    renderer->pixel_size = D2D1::SizeU(width ? width : 1u, height ? height : 1u);
    if (renderer->render_target) {
        HRESULT hr = renderer->render_target->Resize(renderer->pixel_size);
        if (FAILED(hr)) {
            renderer->render_target.Reset();
            renderer->text_brush.Reset();
            renderer->highlight_brush.Reset();
            return false;
        }
    }
    return true;
}

void direct2d_renderer_paint(Direct2DRenderer* renderer, const VirtualListView* view) {
    if (!renderer) {
        return;
    }
    if (FAILED(direct2d_renderer_create_device_resources(renderer))) {
        return;
    }
    if (!renderer->render_target) {
        return;
    }

    renderer->render_target->BeginDraw();
    renderer->render_target->Clear(D2D1::ColorF(renderer->background_color[0], renderer->background_color[1],
                                                renderer->background_color[2], renderer->background_color[3]));

    if (view && view->hits && view->row_height > 0.0f) {
        size_t start = view->visible_start < view->hit_count ? view->visible_start : view->hit_count;
        size_t end = view->visible_end < view->hit_count ? view->visible_end : view->hit_count;
        if (end > start) {
            std::wstring batch;
            batch.reserve((end - start) * 64);
            std::vector<D2D1_RECT_F> selection_rects;
            selection_rects.reserve(view->has_selection ? 1u : 0u);

            float fractional = view->fractional_offset;
            if (fractional < 0.0f) fractional = 0.0f;
            float row_height = view->row_height;
            float translation = -fractional;

            for (size_t idx = start; idx < end; ++idx) {
                const VirtualListHit* hit = view->hits[idx];
                if (hit && hit->name) {
                    batch.append(hit->name);
                }
                if (idx + 1 < end) {
                    batch.push_back(L'\n');
                }
                if (view->has_selection && idx == view->selected_index) {
                    float y0 = static_cast<float>(idx - start) * row_height - fractional;
                    float y1 = y0 + row_height;
                    selection_rects.emplace_back(D2D1::RectF(0.0f, y0,
                        static_cast<FLOAT>(renderer->pixel_size.width), y1));
                }
            }

            if (!batch.empty()) {
                ComPtr<IDWriteTextLayout> layout;
                HRESULT hr = renderer->dwrite_factory->CreateTextLayout(
                    batch.c_str(), static_cast<UINT32>(batch.size()), renderer->text_format.Get(),
                    static_cast<FLOAT>(renderer->pixel_size.width),
                    static_cast<FLOAT>((end - start) * row_height + row_height),
                    layout.GetAddressOf());
                if (SUCCEEDED(hr)) {
                    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, row_height, row_height);

                    renderer->render_target->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, translation));
                    for (const auto& rect : selection_rects) {
                        renderer->render_target->FillRectangle(rect, renderer->highlight_brush.Get());
                    }
                    renderer->render_target->DrawTextLayout(D2D1::Point2F(0.0f, 0.0f), layout.Get(),
                                                             renderer->text_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    renderer->render_target->SetTransform(D2D1::Matrix3x2F::Identity());
                }
            }
        }
    }

    HRESULT hr = renderer->render_target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        renderer->render_target.Reset();
        renderer->text_brush.Reset();
        renderer->highlight_brush.Reset();
    }
}

#endif /* _WIN32 */
