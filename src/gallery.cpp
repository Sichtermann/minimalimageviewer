#include "viewer.h"
#include <d2d1helper.h>

extern AppContext g_ctx;

static constexpr float THUMB_W   = 160.0f;
static constexpr float THUMB_H   = 120.0f;
static constexpr float THUMB_PAD = 10.0f;
static constexpr float LABEL_H   = 22.0f;
static constexpr float CELL_W    = THUMB_W + THUMB_PAD * 2;
static constexpr float CELL_H    = THUMB_H + LABEL_H + THUMB_PAD * 2;

static int CalcCols(float winW) {
    return std::max(1, static_cast<int>(winW / CELL_W));
}

void CleanupGalleryThread() {
    g_ctx.cancelGalleryLoading = true;
    if (g_ctx.galleryLoaderThread.joinable())
        g_ctx.galleryLoaderThread.join();
}

void EnterGalleryMode() {
    if (g_ctx.isGalleryMode || g_ctx.imageFiles.empty()) return;

    int n = static_cast<int>(g_ctx.imageFiles.size());
    g_ctx.galleryThumbs.assign(n, nullptr);
    g_ctx.galleryThumbLoaded.assign(n, false);
    g_ctx.galleryThumbFailed.assign(n, false);
    g_ctx.galleryScrollOffset = 0;
    g_ctx.gallerySelectedIndex = g_ctx.currentImageIndex;
    g_ctx.cancelGalleryLoading = false;
    g_ctx.isGalleryMode = true;

    // Scroll so the current image is centred in view
    RECT cr;
    GetClientRect(g_ctx.hWnd, &cr);
    int cols = CalcCols(static_cast<float>(cr.right - cr.left));
    if (g_ctx.gallerySelectedIndex >= 0 && cols > 0) {
        int row     = g_ctx.gallerySelectedIndex / cols;
        int visRows = static_cast<int>((cr.bottom - cr.top) / CELL_H);
        int target  = std::max(0, row - visRows / 2);
        g_ctx.galleryScrollOffset = target * static_cast<int>(CELL_H);
    }

    // Snapshot file list for background loader (keeps thread independent of main-thread mutations)
    std::vector<std::wstring> files  = g_ctx.imageFiles;
    int                       selIdx = g_ctx.gallerySelectedIndex;

    g_ctx.galleryLoaderThread = std::thread([files, selIdx]() {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;

        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
            CoUninitialize();
            return;
        }

        int n = static_cast<int>(files.size());

        // Load order: current image first, then outward so visible items appear quickly
        std::vector<int> order;
        order.reserve(n);
        if (selIdx >= 0 && selIdx < n) order.push_back(selIdx);
        for (int i = 1; i < n; ++i) {
            if (selIdx + i < n) order.push_back(selIdx + i);
            if (selIdx - i >= 0) order.push_back(selIdx - i);
        }
        if (selIdx < 0) {
            for (int i = 0; i < n; ++i) order.push_back(i);
        }

        for (int idx : order) {
            if (g_ctx.cancelGalleryLoading) break;

            auto postFail = [&]() {
                PostMessage(g_ctx.hWnd, WM_APP_THUMB_READY, 0, static_cast<LPARAM>(idx));
            };

            ComPtr<IWICBitmapDecoder> decoder;
            if (FAILED(factory->CreateDecoderFromFilename(files[idx].c_str(), nullptr,
                        GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) {
                postFail(); continue;
            }
            ComPtr<IWICBitmapFrameDecode> frame;
            if (FAILED(decoder->GetFrame(0, &frame))) { postFail(); continue; }

            UINT w = 0, h = 0;
            frame->GetSize(&w, &h);
            if (w == 0 || h == 0) { postFail(); continue; }

            float scale = std::min(THUMB_W / static_cast<float>(w),
                                   THUMB_H / static_cast<float>(h));
            UINT newW = std::max(1u, static_cast<UINT>(w * scale));
            UINT newH = std::max(1u, static_cast<UINT>(h * scale));

            ComPtr<IWICBitmapScaler> scaler;
            if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
                FAILED(scaler->Initialize(frame, newW, newH, WICBitmapInterpolationModeFant))) {
                postFail(); continue;
            }

            ComPtr<IWICFormatConverter> converter;
            if (FAILED(factory->CreateFormatConverter(&converter)) ||
                FAILED(converter->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA,
                        WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom))) {
                postFail(); continue;
            }

            ComPtr<IWICBitmap> wicBmp;
            if (FAILED(factory->CreateBitmapFromSource(converter, WICBitmapCacheOnLoad, &wicBmp))) {
                postFail(); continue;
            }

            // Transfer ownership to main thread via raw pointer (AddRef here, Release in handler)
            IWICBitmap* raw = wicBmp;
            raw->AddRef();
            PostMessage(g_ctx.hWnd, WM_APP_THUMB_READY,
                        reinterpret_cast<WPARAM>(raw), static_cast<LPARAM>(idx));
        }

        CoUninitialize();
    });

    InvalidateRect(g_ctx.hWnd, nullptr, FALSE);
}

void ExitGalleryMode() {
    if (!g_ctx.isGalleryMode) return;
    g_ctx.isGalleryMode = false;       // checked by WM_APP_THUMB_READY handler before storing
    CleanupGalleryThread();             // join: no more posts after this
    g_ctx.galleryThumbs.clear();
    g_ctx.galleryThumbLoaded.clear();
    g_ctx.galleryThumbFailed.clear();
    g_ctx.gallerySelBrush = nullptr;
    g_ctx.galleryBgBrush  = nullptr;
    InvalidateRect(g_ctx.hWnd, nullptr, FALSE);
}

// Called from DiscardDeviceResources when the render target is torn down while gallery is active.
// Invalidates all D2D bitmaps without joining the loader thread.
void GalleryDiscardBitmaps() {
    for (auto& t : g_ctx.galleryThumbs) t = nullptr;
    std::fill(g_ctx.galleryThumbLoaded.begin(), g_ctx.galleryThumbLoaded.end(), false);
    std::fill(g_ctx.galleryThumbFailed.begin(), g_ctx.galleryThumbFailed.end(), false);
    g_ctx.gallerySelBrush = nullptr;
    g_ctx.galleryBgBrush  = nullptr;
}

void RenderGallery() {
    if (!g_ctx.renderTarget || !g_ctx.textFormat || !g_ctx.textBrush) return;

    RECT cr;
    GetClientRect(g_ctx.hWnd, &cr);
    float winW  = static_cast<float>(cr.right  - cr.left);
    float winH  = static_cast<float>(cr.bottom - cr.top);
    int   cols  = CalcCols(winW);
    int   n     = static_cast<int>(g_ctx.imageFiles.size());

    g_ctx.renderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
    g_ctx.renderTarget->Clear(D2D1::ColorF(0.117f, 0.117f, 0.117f));

    if (n == 0) {
        g_ctx.textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
        g_ctx.textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_ctx.textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_ctx.renderTarget->DrawTextW(L"No images in folder", 19,
            g_ctx.textFormat, D2D1::RectF(0, 0, winW, winH), g_ctx.textBrush);
        g_ctx.textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_ctx.textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        return;
    }

    // Lazy-create persistent brushes
    if (!g_ctx.gallerySelBrush)
        g_ctx.renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.5f, 1.0f, 0.85f), &g_ctx.gallerySelBrush);
    if (!g_ctx.galleryBgBrush)
        g_ctx.renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.22f, 0.22f, 0.22f), &g_ctx.galleryBgBrush);

    float scroll   = static_cast<float>(g_ctx.galleryScrollOffset);
    int   startRow = static_cast<int>(scroll / CELL_H);
    int   visRows  = static_cast<int>(winH / CELL_H) + 2;

    for (int row = startRow; row < startRow + visRows; ++row) {
        for (int col = 0; col < cols; ++col) {
            int idx = row * cols + col;
            if (idx >= n) break;

            float cellX = col  * CELL_W;
            float cellY = row  * CELL_H - scroll;
            if (cellY + CELL_H < 0 || cellY > winH) continue;

            bool selected = (idx == g_ctx.gallerySelectedIndex);

            // Cell background
            D2D1_RECT_F cellRect = D2D1::RectF(cellX + 2, cellY + 2,
                                                cellX + CELL_W - 2, cellY + CELL_H - 2);
            if (selected && g_ctx.gallerySelBrush)
                g_ctx.renderTarget->FillRectangle(cellRect, g_ctx.gallerySelBrush);
            else if (g_ctx.galleryBgBrush)
                g_ctx.renderTarget->FillRectangle(cellRect, g_ctx.galleryBgBrush);

            // Thumbnail area
            D2D1_RECT_F thumbArea = D2D1::RectF(
                cellX + THUMB_PAD, cellY + THUMB_PAD,
                cellX + THUMB_PAD + THUMB_W, cellY + THUMB_PAD + THUMB_H);

            ComPtr<ID2D1Bitmap> thumb;
            bool loaded = false, failed = false;
            if (idx < static_cast<int>(g_ctx.galleryThumbs.size())) {
                thumb  = g_ctx.galleryThumbs[idx];
                loaded = g_ctx.galleryThumbLoaded[idx];
                failed = g_ctx.galleryThumbFailed[idx];
            }

            if (thumb) {
                D2D1_SIZE_F ts   = thumb->GetSize();
                float       drawX = cellX + THUMB_PAD + (THUMB_W - ts.width)  / 2.0f;
                float       drawY = cellY + THUMB_PAD + (THUMB_H - ts.height) / 2.0f;
                g_ctx.renderTarget->DrawBitmap(thumb,
                    D2D1::RectF(drawX, drawY, drawX + ts.width, drawY + ts.height),
                    1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                ComPtr<ID2D1SolidColorBrush> phBrush;
                D2D1_COLOR_F phColor = failed
                    ? D2D1::ColorF(0.5f, 0.1f, 0.1f, 0.4f)
                    : D2D1::ColorF(0.3f, 0.3f, 0.3f, 0.4f);
                if (SUCCEEDED(g_ctx.renderTarget->CreateSolidColorBrush(phColor, &phBrush)))
                    g_ctx.renderTarget->FillRectangle(thumbArea, phBrush);
            }

            // Filename label (truncated naturally by rect)
            const wchar_t* name = PathFindFileNameW(g_ctx.imageFiles[idx].c_str());
            if (name) {
                D2D1_RECT_F labelRect = D2D1::RectF(
                    cellX + 4,
                    cellY + THUMB_PAD + THUMB_H + 3,
                    cellX + CELL_W - 4,
                    cellY + CELL_H - 2);
                g_ctx.textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
                g_ctx.textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                g_ctx.textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                g_ctx.renderTarget->DrawTextW(name, static_cast<UINT32>(wcslen(name)),
                    g_ctx.textFormat, labelRect, g_ctx.textBrush);
                g_ctx.textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                g_ctx.textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
        }
    }

    // Status bar
    const wchar_t* hint  = L"Gallery  ·  Click or Enter to open  ·  G / Esc to exit";
    D2D1_RECT_F    hintR = D2D1::RectF(0, winH - 24, winW, winH);
    ComPtr<ID2D1SolidColorBrush> hintBg;
    if (SUCCEEDED(g_ctx.renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.6f), &hintBg)))
        g_ctx.renderTarget->FillRectangle(hintR, hintBg);
    g_ctx.textBrush->SetColor(D2D1::ColorF(0.75f, 0.75f, 0.75f));
    g_ctx.textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_ctx.textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_ctx.renderTarget->DrawTextW(hint, static_cast<UINT32>(wcslen(hint)),
        g_ctx.textFormat, hintR, g_ctx.textBrush);
    g_ctx.textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g_ctx.textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}

void GalleryOnClick(POINT pt) {
    RECT cr;
    GetClientRect(g_ctx.hWnd, &cr);
    int cols = CalcCols(static_cast<float>(cr.right - cr.left));
    int n    = static_cast<int>(g_ctx.imageFiles.size());

    int col = static_cast<int>(pt.x / CELL_W);
    int row = static_cast<int>((pt.y + g_ctx.galleryScrollOffset) / CELL_H);
    int idx = row * cols + col;

    if (col < 0 || col >= cols || idx < 0 || idx >= n) return;

    ExitGalleryMode();
    g_ctx.currentImageIndex = idx;
    LoadImageFromFile(g_ctx.imageFiles[idx]);
}

void GalleryOnScroll(int delta) {
    RECT cr;
    GetClientRect(g_ctx.hWnd, &cr);
    int cols      = CalcCols(static_cast<float>(cr.right - cr.left));
    int n         = static_cast<int>(g_ctx.imageFiles.size());
    int totalRows = (n + cols - 1) / cols;
    int maxScroll = std::max(0, static_cast<int>(totalRows * CELL_H) - static_cast<int>(cr.bottom - cr.top));

    // 3 rows per notch
    g_ctx.galleryScrollOffset -= (delta / WHEEL_DELTA) * static_cast<int>(CELL_H) * 3;
    g_ctx.galleryScrollOffset  = std::max(0, std::min(g_ctx.galleryScrollOffset, maxScroll));
    InvalidateRect(g_ctx.hWnd, nullptr, FALSE);
}

void GalleryOnKeyDown(WPARAM key) {
    int n = static_cast<int>(g_ctx.imageFiles.size());
    if (n == 0) { ExitGalleryMode(); return; }

    RECT cr;
    GetClientRect(g_ctx.hWnd, &cr);
    int cols = CalcCols(static_cast<float>(cr.right - cr.left));
    int sel  = std::max(0, g_ctx.gallerySelectedIndex);

    switch (key) {
    case VK_LEFT:   sel = std::max(0,     sel - 1);    break;
    case VK_RIGHT:  sel = std::min(n - 1, sel + 1);    break;
    case VK_UP:     sel = std::max(0,     sel - cols);  break;
    case VK_DOWN:   sel = std::min(n - 1, sel + cols);  break;
    case VK_RETURN:
        ExitGalleryMode();
        g_ctx.currentImageIndex = sel;
        LoadImageFromFile(g_ctx.imageFiles[sel]);
        return;
    case VK_ESCAPE:
    case 'G':
        ExitGalleryMode();
        return;
    default:
        return;
    }

    g_ctx.gallerySelectedIndex = sel;

    // Scroll to keep selection visible
    if (cols > 0) {
        int row      = sel / cols;
        int scrollRow = g_ctx.galleryScrollOffset / static_cast<int>(CELL_H);
        int visRows   = (cr.bottom - cr.top) / static_cast<int>(CELL_H);
        if (row < scrollRow)
            g_ctx.galleryScrollOffset = row * static_cast<int>(CELL_H);
        else if (row >= scrollRow + visRows)
            g_ctx.galleryScrollOffset = (row - visRows + 1) * static_cast<int>(CELL_H);
    }

    InvalidateRect(g_ctx.hWnd, nullptr, FALSE);
}
