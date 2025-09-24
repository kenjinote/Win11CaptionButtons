// Win11CustomFrame_SingleTitle.cpp
// Build: link with d2d1.lib dwmapi.lib
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <string>
#pragma comment(lib, "d2d1.lib")

#include <uxtheme.h>
#include <dwmapi.h>
#include <vssym32.h>

#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")

#include <dwrite.h>
#pragma comment(lib, "dwrite.lib")

#include "resource.h"

// simple SafeRelease
template<class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ----------------- 設定 -----------------
constexpr int TITLEBAR_HEIGHT = 34; // タイトルバー高さ（px）
#define BUTTON_WIDTH 46
static HWND g_hWnd = nullptr;

// Direct2D
static ID2D1Factory* g_pFactory = nullptr;
static ID2D1HwndRenderTarget* g_pRT = nullptr;
static ID2D1SolidColorBrush* g_pBrushText = nullptr;
static ID2D1SolidColorBrush* g_pBrushIcon = nullptr;
static ID2D1SolidColorBrush* g_pBrushHover = nullptr;
static ID2D1SolidColorBrush* g_pBrushPressed = nullptr;

static IDWriteFactory* g_pDWriteFactory = nullptr;
static IDWriteTextFormat* g_pTextFormat = nullptr;

// ボタン状態
struct ButtonState {
    RECT rc;
    bool hover;
    bool pressed;
};
static ButtonState g_btnMin = {}, g_btnMax = {}, g_btnClose = {};

// 互換用に別名RECTも用意（古いコード向け）
static RECT g_rcMinButton = {}, g_rcMaxButton = {}, g_rcCloseButton = {};

// 色
COLORREF g_textColor = RGB(0, 0, 0);
COLORREF g_iconColor = RGB(0, 0, 0);

// -----------------------------------------

bool IsAppsUseLightTheme()
{
    HKEY hKey = NULL;
    DWORD value = 1; // デフォルトはライト
    DWORD dataSize = sizeof(value);
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        RegGetValueW(hKey, NULL, L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &value, &dataSize);
        RegCloseKey(hKey);
    }
    return value != 0;
}

void EnableDarkModeForWindow(HWND hwnd, bool enable)
{
    BOOL useDark = enable ? TRUE : FALSE;
    // 属性 ID のフォールバック（環境により 20 または 19 を使う実装）
    const DWORD attrs[] = { 20, 19 }; // 20 が標準的だが環境によっては 19
    HRESULT hr = E_FAIL;
    for (int i = 0; i < _countof(attrs); ++i) {
        hr = DwmSetWindowAttribute(hwnd, attrs[i], &useDark, sizeof(useDark));
        if (SUCCEEDED(hr)) break;
    }
    // デバッグ出力（失敗するときに役立つ）
    if (FAILED(hr)) {
        wchar_t buf[128];
        swprintf_s(buf, L"EnableDarkModeForWindow: DwmSetWindowAttribute failed: 0x%08X\n", hr);
        OutputDebugStringW(buf);
    }
}

// ダークモード有効化ヘルパー
void EnableDarkMode(HWND hwnd, bool enable)
{
    BOOL useDark = enable ? TRUE : FALSE;
    HRESULT hr = DwmSetWindowAttribute(
        hwnd,
        DWMWA_USE_IMMERSIVE_DARK_MODE,
        &useDark,
        sizeof(useDark)
    );

    if (FAILED(hr)) {
        // デバッグ用: 失敗したらエラーコードを確認
        wchar_t buf[128];
        swprintf_s(buf, L"DwmSetWindowAttribute failed: 0x%08X\n", hr);
        OutputDebugStringW(buf);
    }
}

// ダークモード判定（Win11対応）
bool IsDarkMode(HWND hwnd)
{
    BOOL dark = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark)))) {
        return dark;
    }
    return false;
}

void UpdateColors(HWND hWnd)
{
    if (IsDarkMode(hWnd)) {
        g_textColor = RGB(255, 255, 255);
        g_iconColor = RGB(255, 255, 255);
    }
    else {
        g_textColor = RGB(0, 0, 0);
        g_iconColor = RGB(0, 0, 0);
    }
}

void CreateDeviceResources(HWND hwnd)
{
    if (!g_pFactory) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_pFactory);
    }
    if (!g_pRT) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
        D2D1_HWND_RENDER_TARGET_PROPERTIES props = D2D1::HwndRenderTargetProperties(hwnd, size);
        g_pFactory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(), props, &g_pRT);
    }
    SafeRelease(g_pBrushText);
    SafeRelease(g_pBrushIcon);
    SafeRelease(g_pBrushHover);
    SafeRelease(g_pBrushPressed);
    UpdateColors(hwnd);
    if (g_pRT) {
        g_pRT->CreateSolidColorBrush(D2D1::ColorF(
            GetRValue(g_textColor) / 255.0f,
            GetGValue(g_textColor) / 255.0f,
            GetBValue(g_textColor) / 255.0f), &g_pBrushText);
        g_pRT->CreateSolidColorBrush(D2D1::ColorF(
            GetRValue(g_iconColor) / 255.0f,
            GetGValue(g_iconColor) / 255.0f,
            GetBValue(g_iconColor) / 255.0f), &g_pBrushIcon);

        // hover/pressed (translucent blue)
        g_pRT->CreateSolidColorBrush(D2D1::ColorF(0.22f, 0.49f, 0.80f, 0.12f), &g_pBrushHover);
        g_pRT->CreateSolidColorBrush(D2D1::ColorF(0.22f, 0.49f, 0.80f, 0.18f), &g_pBrushPressed);
    }
}

void DiscardDeviceResources()
{
    SafeRelease(g_pRT);
    SafeRelease(g_pBrushText);
    SafeRelease(g_pBrushIcon);
    SafeRelease(g_pBrushHover);
    SafeRelease(g_pBrushPressed);
}

// ボタンの配置
void LayoutButtons(int width)
{
    int btnW = TITLEBAR_HEIGHT; // 正方形
    int marginRight = 6;
    int x = width - marginRight;
    // close
    g_btnClose.rc = { x - btnW, 0, x, TITLEBAR_HEIGHT };
    x -= (btnW + 6);
    // max/restore
    g_btnMax.rc = { x - btnW, 0, x, TITLEBAR_HEIGHT };
    x -= (btnW + 6);
    // min
    g_btnMin.rc = { x - btnW, 0, x, TITLEBAR_HEIGHT };

    // 互換用RECTも更新
    g_rcCloseButton = g_btnClose.rc;
    g_rcMaxButton = g_btnMax.rc;
    g_rcMinButton = g_btnMin.rc;
}

enum BtnId { BTN_NONE = 0, BTN_MIN, BTN_MAX, BTN_CLOSE };

BtnId HitTestButtons(POINT pt)
{
    if (PtInRect(&g_btnClose.rc, pt)) return BTN_CLOSE;
    if (PtInRect(&g_btnMax.rc, pt)) return BTN_MAX;
    if (PtInRect(&g_btnMin.rc, pt)) return BTN_MIN;
    return BTN_NONE;
}

// アイコン描画補助（Direct2D）
void DrawMinIcon(ID2D1RenderTarget* rt, D2D1_POINT_2F center, float size, ID2D1Brush* brush, float strokeWidth)
{
    D2D1_POINT_2F p1 = D2D1::Point2F(center.x - size / 2, center.y + size / 6);
    D2D1_POINT_2F p2 = D2D1::Point2F(center.x + size / 2, center.y + size / 6);
    rt->DrawLine(p1, p2, brush, strokeWidth);
}

void DrawMaxIcon(ID2D1RenderTarget* rt, D2D1_POINT_2F center, float size, ID2D1Brush* brush, float strokeWidth)
{
    D2D1_RECT_F r = D2D1::RectF(center.x - size / 2, center.y - size / 2, center.x + size / 2, center.y + size / 2);
    rt->DrawRectangle(r, brush, strokeWidth);
}

void DrawRestoreIcon(ID2D1RenderTarget* rt, D2D1_POINT_2F center, float size, ID2D1Brush* brush, float strokeWidth)
{
    float w = size, h = size;
    D2D1_RECT_F r2 = D2D1::RectF(center.x - w / 2 + 2.0f, center.y - h / 2 - 1.0f, center.x + w / 2 + 2.0f, center.y + h / 2 - 1.0f);
    D2D1_RECT_F r1 = D2D1::RectF(center.x - w / 2 - 2.0f, center.y - h / 2 + 2.0f, center.x + w / 2 - 2.0f, center.y + h / 2 + 2.0f);

    ID2D1PathGeometry* pg = nullptr;
    ID2D1GeometrySink* sink = nullptr;

    if (SUCCEEDED(g_pFactory->CreatePathGeometry(&pg))) {
        if (SUCCEEDED(pg->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(r2.left + 1, r2.top + 3), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(r2.right - 3, r2.top + 3));
            sink->AddLine(D2D1::Point2F(r2.right - 3, r2.bottom - 3));
            sink->AddLine(D2D1::Point2F(r2.left + 1, r2.bottom - 3));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
            rt->DrawGeometry(pg, brush, strokeWidth);
        }
        SafeRelease(pg);
    }

    if (SUCCEEDED(g_pFactory->CreatePathGeometry(&pg))) {
        if (SUCCEEDED(pg->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(r1.left + 1, r1.top + 3), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(r1.right - 3, r1.top + 3));
            sink->AddLine(D2D1::Point2F(r1.right - 3, r1.bottom - 3));
            sink->AddLine(D2D1::Point2F(r1.left + 1, r1.bottom - 3));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
            rt->DrawGeometry(pg, brush, strokeWidth);
        }
        SafeRelease(pg);
    }
}

void DrawCloseIcon(ID2D1RenderTarget* rt, D2D1_POINT_2F center, float size, ID2D1Brush* brush, float strokeWidth)
{
    float s = size / 2;
    D2D1_POINT_2F p1 = D2D1::Point2F(center.x - s, center.y - s);
    D2D1_POINT_2F p2 = D2D1::Point2F(center.x + s, center.y + s);
    D2D1_POINT_2F p3 = D2D1::Point2F(center.x + s, center.y - s);
    D2D1_POINT_2F p4 = D2D1::Point2F(center.x - s, center.y + s);
    rt->DrawLine(p1, p2, brush, strokeWidth);
    rt->DrawLine(p3, p4, brush, strokeWidth);
}

void CreateTextFormat()
{
    if (!g_pDWriteFactory) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory), (IUnknown**)&g_pDWriteFactory);
    }
    SafeRelease(g_pTextFormat);
    if (g_pDWriteFactory) {
        g_pDWriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"ja-jp", &g_pTextFormat);
    }
}

void OnPaint(HWND hwnd)
{
    PAINTSTRUCT ps;
    BeginPaint(hwnd, &ps);

    CreateDeviceResources(hwnd);
    CreateTextFormat();
    if (!g_pRT) { EndPaint(hwnd, &ps); return; }

    g_pRT->BeginDraw();
    RECT rcWnd; GetClientRect(hwnd, &rcWnd);

    // 背景クリア
    if (IsDarkMode(hwnd)) {
        g_pRT->Clear(D2D1::ColorF(0.11f, 0.11f, 0.11f, 1.0f)); // ダーク背景
    }
    else {
        g_pRT->Clear(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));   // ライト背景
    }

    float btnIconSize = (float)TITLEBAR_HEIGHT * 0.35f;
    float stroke = 1.6f;

    // ==== 左端にアプリアイコンを描画 ====
    HICON hIcon = (HICON)GetClassLongPtr(hwnd, GCLP_HICONSM);
    if (!hIcon) hIcon = (HICON)GetClassLongPtr(hwnd, GCLP_HICON);
    if (hIcon) {
        ICONINFO ii;
        GetIconInfo(hIcon, &ii);
        HBITMAP hbmp = ii.hbmColor;
        BITMAP bmp;
        if (hbmp && GetObject(hbmp, sizeof(BITMAP), &bmp)) {
            ID2D1Bitmap* d2dBmp = nullptr;
            D2D1_SIZE_U size = D2D1::SizeU(bmp.bmWidth, bmp.bmHeight);

            HDC hdc = CreateCompatibleDC(NULL);
            SelectObject(hdc, hbmp);
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = bmp.bmWidth;
            bmi.bmiHeader.biHeight = -bmp.bmHeight;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            void* bits = nullptr;
            HBITMAP hbmDIB = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
            HDC hdcDIB = CreateCompatibleDC(NULL);
            SelectObject(hdcDIB, hbmDIB);
            BitBlt(hdcDIB, 0, 0, bmp.bmWidth, bmp.bmHeight, hdc, 0, 0, SRCCOPY);

            D2D1_BITMAP_PROPERTIES bp = {};
            bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
            bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;

            g_pRT->CreateBitmap(D2D1::SizeU(bmp.bmWidth, bmp.bmHeight), bits, bmp.bmWidth * 4, bp, &d2dBmp);
            if (d2dBmp) {
                D2D1_RECT_F iconRect = D2D1::RectF(4.0f, 2.0f, 4.0f + TITLEBAR_HEIGHT - 4, 2.0f + TITLEBAR_HEIGHT - 4);
                g_pRT->DrawBitmap(d2dBmp, iconRect);
                SafeRelease(d2dBmp);
            }

            DeleteDC(hdcDIB);
            DeleteObject(hbmDIB);
            DeleteDC(hdc);
        }
        DeleteObject(ii.hbmColor);
        DeleteObject(ii.hbmMask);
    }

    // ==== タイトル文字描画（上下中央） ====
    std::wstring title = L"My App - Custom Win11 Style Titlebar";

    if (g_pTextFormat && g_pBrushText)
    {
        IDWriteTextLayout* pTextLayout = nullptr;
        g_pDWriteFactory->CreateTextLayout(
            title.c_str(),
            (UINT32)title.size(),
            g_pTextFormat,
            (FLOAT)(rcWnd.right - TITLEBAR_HEIGHT - 150), // 最大幅
            (FLOAT)TITLEBAR_HEIGHT,                        // 最大高さ
            &pTextLayout
        );

        if (pTextLayout)
        {
            DWRITE_TEXT_METRICS metrics;
            pTextLayout->GetMetrics(&metrics);

            float textTop = (TITLEBAR_HEIGHT - metrics.height) / 2.0f;

            g_pRT->DrawTextLayout(D2D1::Point2F((FLOAT)TITLEBAR_HEIGHT, textTop), pTextLayout, g_pBrushText);

            SafeRelease(pTextLayout);
        }
    }

    // ==== ボタン背景描画（Windows11風矩形） ====
    auto DrawBtnBG = [&](ButtonState& btn, bool isClose) {
        if (isClose) {
            if (btn.hover) {
                ID2D1SolidColorBrush* hoverBrush = nullptr;
                g_pRT->CreateSolidColorBrush(
                    D2D1::ColorF(1.0f, 0.0f, 0.0f, btn.pressed ? 0.5f : 0.25f),
                    &hoverBrush);
                if (hoverBrush) {
                    g_pRT->FillRectangle(D2D1::RectF(
                        (FLOAT)btn.rc.left, (FLOAT)btn.rc.top,
                        (FLOAT)btn.rc.right, (FLOAT)btn.rc.bottom),
                        hoverBrush);
                    SafeRelease(hoverBrush);
                }
            }
        }
        else {
            if (btn.hover) {
                ID2D1Brush* b = btn.pressed ? g_pBrushPressed : g_pBrushHover;
                g_pRT->FillRectangle(D2D1::RectF(
                    (FLOAT)btn.rc.left, (FLOAT)btn.rc.top,
                    (FLOAT)btn.rc.right, (FLOAT)btn.rc.bottom),
                    b);
            }
        }
        };

    // Min
    DrawBtnBG(g_btnMin, false);
    DrawMinIcon(g_pRT, D2D1::Point2F((g_btnMin.rc.left + g_btnMin.rc.right) / 2.0f,
        (g_btnMin.rc.top + g_btnMin.rc.bottom) / 2.0f),
        btnIconSize, g_pBrushIcon, stroke);

    // Max / Restore
    DrawBtnBG(g_btnMax, false);
    if (IsZoomed(hwnd)) {
        DrawRestoreIcon(g_pRT, D2D1::Point2F((g_btnMax.rc.left + g_btnMax.rc.right) / 2.0f,
            (g_btnMax.rc.top + g_btnMax.rc.bottom) / 2.0f),
            btnIconSize, g_pBrushIcon, stroke);
    }
    else {
        DrawMaxIcon(g_pRT, D2D1::Point2F((g_btnMax.rc.left + g_btnMax.rc.right) / 2.0f,
            (g_btnMax.rc.top + g_btnMax.rc.bottom) / 2.0f),
            btnIconSize, g_pBrushIcon, stroke);
    }

    // Close
    DrawBtnBG(g_btnClose, true);
    DrawCloseIcon(g_pRT, D2D1::Point2F((g_btnClose.rc.left + g_btnClose.rc.right) / 2.0f,
        (g_btnClose.rc.top + g_btnClose.rc.bottom) / 2.0f),
        btnIconSize,
        g_btnClose.hover ? g_pBrushText : g_pBrushIcon, stroke);

    // ==== 描画完了 ====
    HRESULT hr = g_pRT->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
    }

    EndPaint(hwnd, &ps);
}

void ShowSystemMenu(HWND hwnd, POINT pt)
{
    HMENU hMenu = GetSystemMenu(hwnd, FALSE);
    if (!hMenu) return;

    // メニューを表示
    int cmd = TrackPopupMenu(
        hMenu,
        TPM_RETURNCMD | TPM_LEFTBUTTON | TPM_TOPALIGN | TPM_LEFTALIGN,
        pt.x, pt.y,
        0,
        hwnd,
        NULL
    );
    if (cmd) PostMessage(hwnd, WM_SYSCOMMAND, cmd, 0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
    {
        g_hWnd = hwnd;

        // 標準のタイトルバー削除
        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
        style &= ~WS_CAPTION;
        SetWindowLongPtr(hwnd, GWL_STYLE, style);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

        // 初期テーマを適用
        bool isLight = IsAppsUseLightTheme();
        EnableDarkModeForWindow(hwnd, !isLight);
        
        CreateDeviceResources(hwnd);
        RECT rc; GetClientRect(hwnd, &rc);
        LayoutButtons(rc.right);
        break;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    {
        // OS の設定を読みなおして DWM 属性を再適用
        bool isLight = IsAppsUseLightTheme();
        EnableDarkModeForWindow(hwnd, !isLight);

        // ブラシ等の色を更新して再作成 -> 再描画
        UpdateColors(hwnd);
        DiscardDeviceResources();
        CreateDeviceResources(hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
        break;
    }
    case WM_SIZE:
    {
        DiscardDeviceResources();
        RECT rc; GetClientRect(hwnd, &rc);
        LayoutButtons(rc.right);
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        DiscardDeviceResources();
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_NCHITTEST:
    {
        // 画面座標 -> クライアント座標
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);

        // ボタン領域ならクライアント扱い（クリックはWM_系で処理）
        if (HitTestButtons(pt) != BTN_NONE) return HTCLIENT;

        // タイトルバー領域（上部）：ドラッグ可能にする
        if (pt.y >= 0 && pt.y < TITLEBAR_HEIGHT) return HTCAPTION;

        // それ以外はデフォルト（枠やクライアント）
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    case WM_NCRBUTTONUP: // タイトルバー右クリック
    {
        if (wParam == HTCAPTION) // タイトルバー
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ShowSystemMenu(hwnd, pt);
            return 0;
        }
        break;
    }

    case WM_NCPAINT:
        // 非クライアント領域の描画を抑制
        return 0;

    case WM_RBUTTONUP: // クライアント領域でもアイコン右クリックでメニュー
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        // 左上のアイコン領域
        RECT rcIcon = { 4, 2, TITLEBAR_HEIGHT, TITLEBAR_HEIGHT };
        if (PtInRect(&rcIcon, pt)) {
            ClientToScreen(hwnd, &pt);
            ShowSystemMenu(hwnd, pt);
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        BtnId h = HitTestButtons(pt);
        bool needInvalidate = false;
        if (h == BTN_MIN) { if (!g_btnMin.hover) { g_btnMin.hover = true; needInvalidate = true; } }
        else { if (g_btnMin.hover) { g_btnMin.hover = false; needInvalidate = true; g_btnMin.pressed = false; } }
        if (h == BTN_MAX) { if (!g_btnMax.hover) { g_btnMax.hover = true; needInvalidate = true; } }
        else { if (g_btnMax.hover) { g_btnMax.hover = false; needInvalidate = true; g_btnMax.pressed = false; } }
        if (h == BTN_CLOSE) { if (!g_btnClose.hover) { g_btnClose.hover = true; needInvalidate = true; } }
        else { if (g_btnClose.hover) { g_btnClose.hover = false; needInvalidate = true; g_btnClose.pressed = false; } }

        if (needInvalidate) InvalidateRect(hwnd, NULL, FALSE);

        TRACKMOUSEEVENT tme; ZeroMemory(&tme, sizeof(tme));
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        break;
    }
    case WM_MOUSELEAVE:
    {
        bool need = false;
        if (g_btnMin.hover || g_btnMax.hover || g_btnClose.hover) {
            g_btnMin.hover = g_btnMax.hover = g_btnClose.hover = false;
            g_btnMin.pressed = g_btnMax.pressed = g_btnClose.pressed = false;
            need = true;
        }
        if (need) InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    case WM_LBUTTONDOWN:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        BtnId h = HitTestButtons(pt);
        if (h == BTN_MIN) { g_btnMin.pressed = true; SetCapture(hwnd); InvalidateRect(hwnd, NULL, FALSE); }
        else if (h == BTN_MAX) { g_btnMax.pressed = true; SetCapture(hwnd); InvalidateRect(hwnd, NULL, FALSE); }
        else if (h == BTN_CLOSE) { g_btnClose.pressed = true; SetCapture(hwnd); InvalidateRect(hwnd, NULL, FALSE); }
        break;
    }
    case WM_LBUTTONUP:
    {
        if (GetCapture() == hwnd) ReleaseCapture();
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        BtnId h = HitTestButtons(pt);
        if (g_btnMin.pressed && h == BTN_MIN) {
            ShowWindow(hwnd, SW_MINIMIZE);
        }
        else if (g_btnMax.pressed && h == BTN_MAX) {
            if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE); else ShowWindow(hwnd, SW_MAXIMIZE);
        }
        else if (g_btnClose.pressed && h == BTN_CLOSE) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
        g_btnMin.pressed = g_btnMax.pressed = g_btnClose.pressed = false;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    case WM_PAINT:
        OnPaint(hwnd);
        break;
    case WM_ERASEBKGND:
        // フリッカー防止
        return 1;
    case WM_DESTROY:
        DiscardDeviceResources();
        SafeRelease(g_pFactory);
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmd)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"Win11CustomFrameClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_ICON1));
    RegisterClassW(&wc);

    // CreateWindowEx で通常ウィンドウを作る（後で WS_CAPTION を消す）
    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"Custom Win11-Style Window",
        WS_OVERLAPPEDWINDOW,  // ← WS_OVERLAPPEDWINDOW ではなく WS_POPUP
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 600,
        NULL, NULL, hInst, NULL
    );

    if (!hwnd) return 0;

    MARGINS margins = { 0,0,0,0 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    ShowWindow(hwnd, nCmd);
    UpdateWindow(hwnd);

    RECT rc; GetClientRect(hwnd, &rc);
    LayoutButtons(rc.right);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
