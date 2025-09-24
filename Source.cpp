// Win11CustomFrame_Fixed.cpp
// Build: link with d2d1.lib dwmapi.lib dwrite.lib uxtheme.lib windowscodecs.lib
// Requires a resource file (.rc) that defines IDI_ICON1 for the application icon.
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <wincodec.h> // WIC for icon conversion

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

#include <uxtheme.h>
#include <dwmapi.h>
#include <vssym32.h>

#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")

// <<< 修正: 古いSDKでもコンパイルできるようにDWM関連の定義を追加
#if !defined(DWMWA_SYSTEM_BACKDROP_TYPE)
#define DWMWA_SYSTEM_BACKDROP_TYPE 38
#endif


#include "resource.h"

// simple SafeRelease
template<class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ----------------- 設定 -----------------
constexpr int DEFAULT_TITLEBAR_HEIGHT = 34; // DPI 96でのタイトルバー高さ（px）
static int g_scaledTitlebarHeight = DEFAULT_TITLEBAR_HEIGHT; // DPIスケーリング後の高さ

static HWND g_hWnd = nullptr;
static bool g_isWindowActive = true; // <<< 追加: ウィンドウのアクティブ状態を追跡

// Direct2D
static ID2D1Factory* g_pFactory = nullptr;
static ID2D1HwndRenderTarget* g_pRT = nullptr;
static ID2D1SolidColorBrush* g_pBrushText = nullptr;
static ID2D1SolidColorBrush* g_pBrushIcon = nullptr;
static ID2D1SolidColorBrush* g_pBrushWhite = nullptr; // For close button icon on hover
static ID2D1SolidColorBrush* g_pBrushMinNormal = nullptr;
static ID2D1SolidColorBrush* g_pBrushMinHover = nullptr;
static ID2D1SolidColorBrush* g_pBrushMinPressed = nullptr;
static ID2D1SolidColorBrush* g_pBrushMaxNormal = nullptr;
static ID2D1SolidColorBrush* g_pBrushMaxHover = nullptr;
static ID2D1SolidColorBrush* g_pBrushMaxPressed = nullptr;
static ID2D1SolidColorBrush* g_pBrushCloseNormal = nullptr;
static ID2D1SolidColorBrush* g_pBrushCloseHover = nullptr;
static ID2D1SolidColorBrush* g_pBrushClosePressed = nullptr;

static IDWriteFactory* g_pDWriteFactory = nullptr;
static IDWriteTextFormat* g_pTextFormat = nullptr;
static IWICImagingFactory* g_pWICFactory = nullptr;

// ボタン状態
struct ButtonState {
    RECT rc;
    bool hover;
    bool pressed;
};
static ButtonState g_btnMin = {}, g_btnMax = {}, g_btnClose = {};

// アイコン領域
static RECT g_rcIcon = {};

// 色
COLORREF g_textColor = RGB(0, 0, 0);
COLORREF g_iconColor = RGB(0, 0, 0);

// -----------------------------------------

// Helper function to create a D2D bitmap from an HICON.
HRESULT CreateBitmapFromHICON(HICON hIcon, ID2D1RenderTarget* pRenderTarget, ID2D1Bitmap** ppBitmap)
{
    if (!g_pWICFactory)
    {
        CoCreateInstance(
            CLSID_WICImagingFactory,
            NULL,
            CLSCTX_INPROC_SERVER,
            IID_IWICImagingFactory,
            reinterpret_cast<void**>(&g_pWICFactory)
        );
    }
    if (!g_pWICFactory) return E_FAIL;

    IWICBitmap* pWICBitmap = nullptr;
    HRESULT hr = g_pWICFactory->CreateBitmapFromHICON(hIcon, &pWICBitmap);

    if (SUCCEEDED(hr))
    {
        IWICFormatConverter* pConverter = nullptr;
        hr = g_pWICFactory->CreateFormatConverter(&pConverter);
        if (SUCCEEDED(hr))
        {
            hr = pConverter->Initialize(
                pWICBitmap,
                GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone,
                NULL,
                0.f,
                WICBitmapPaletteTypeMedianCut
            );
            if (SUCCEEDED(hr))
            {
                hr = pRenderTarget->CreateBitmapFromWicBitmap(pConverter, NULL, ppBitmap);
            }
            SafeRelease(pConverter);
        }
        SafeRelease(pWICBitmap);
    }
    return hr;
}


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
    // DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (Windows 10 20H1 and later)
    // DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 = 19 (older versions)
    const DWORD attrs[] = { 20, 19 };
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

// ダークモード判定（Win11対応）
bool IsDarkMode(HWND hwnd)
{
    BOOL dark = FALSE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE = 20
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, 20, &dark, sizeof(dark)))) {
        return dark;
    }
    // DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 = 19
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, 19, &dark, sizeof(dark)))) {
        return dark;
    }
    return false;
}

// <<< 修正: ウィンドウのアクティブ状態に応じて色を変更
void UpdateColors(HWND hWnd)
{
    bool isDark = IsDarkMode(hWnd);
    if (isDark) {
        // ダークモード
        if (g_isWindowActive) {
            g_textColor = RGB(255, 255, 255);
            g_iconColor = RGB(255, 255, 255);
        }
        else {
            g_textColor = RGB(0x99, 0x99, 0x99); // 非アクティブ時のグレー
            g_iconColor = RGB(0x99, 0x99, 0x99);
        }
    }
    else {
        // ライトモード
        if (g_isWindowActive) {
            g_textColor = RGB(0, 0, 0);
            g_iconColor = RGB(0, 0, 0);
        }
        else {
            g_textColor = RGB(0x99, 0x99, 0x99); // 非アクティブ時のグレー
            g_iconColor = RGB(0x99, 0x99, 0x99);
        }
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
    SafeRelease(g_pBrushWhite);
    SafeRelease(g_pBrushMinNormal);
    SafeRelease(g_pBrushMinHover);
    SafeRelease(g_pBrushMinPressed);
    SafeRelease(g_pBrushMaxNormal);
    SafeRelease(g_pBrushMaxHover);
    SafeRelease(g_pBrushMaxPressed);
    SafeRelease(g_pBrushCloseNormal);
    SafeRelease(g_pBrushCloseHover);
    SafeRelease(g_pBrushClosePressed);

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
        g_pRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &g_pBrushWhite);

        HTHEME hTheme = OpenThemeData(hwnd, L"Window");
        COLORREF cr;
        bool isDark = IsDarkMode(hwnd);

        auto create_brush_with_fallback = [&](ID2D1SolidColorBrush** brush, int part, int state, COLORREF fallback) {
            if (hTheme && SUCCEEDED(GetThemeColor(hTheme, part, state, TMT_FILLCOLORHINT, &cr))) {
                g_pRT->CreateSolidColorBrush(D2D1::ColorF(GetRValue(cr) / 255.0f, GetGValue(cr) / 255.0f, GetBValue(cr) / 255.0f), brush);
            }
            else if (brush != &g_pBrushMinNormal && brush != &g_pBrushMaxNormal && brush != &g_pBrushCloseNormal) { // Normal state can be transparent
                g_pRT->CreateSolidColorBrush(D2D1::ColorF(GetRValue(fallback) / 255.0f, GetGValue(fallback) / 255.0f, GetBValue(fallback) / 255.0f), brush);
            }
            };

        // Default colors
        COLORREF hoverColor = isDark ? RGB(0x38, 0x38, 0x38) : RGB(0xE6, 0xE6, 0xE6);
        COLORREF pressedColor = isDark ? RGB(0x50, 0x50, 0x50) : RGB(0xCC, 0xCC, 0xCC);
        COLORREF closeHoverColor = RGB(0xE8, 0x11, 0x23);
        COLORREF closePressedColor = RGB(0xB7, 0x11, 0x23);

        create_brush_with_fallback(&g_pBrushMinNormal, WP_MINBUTTON, CBS_NORMAL, 0);
        create_brush_with_fallback(&g_pBrushMinHover, WP_MINBUTTON, CBS_HOT, hoverColor);
        create_brush_with_fallback(&g_pBrushMinPressed, WP_MINBUTTON, CBS_PUSHED, pressedColor);

        create_brush_with_fallback(&g_pBrushMaxNormal, WP_MAXBUTTON, CBS_NORMAL, 0);
        create_brush_with_fallback(&g_pBrushMaxHover, WP_MAXBUTTON, CBS_HOT, hoverColor);
        create_brush_with_fallback(&g_pBrushMaxPressed, WP_MAXBUTTON, CBS_PUSHED, pressedColor);

        create_brush_with_fallback(&g_pBrushCloseNormal, WP_CLOSEBUTTON, CBS_NORMAL, 0);
        create_brush_with_fallback(&g_pBrushCloseHover, WP_CLOSEBUTTON, CBS_HOT, closeHoverColor);
        create_brush_with_fallback(&g_pBrushClosePressed, WP_CLOSEBUTTON, CBS_PUSHED, closePressedColor);

        if (hTheme) CloseThemeData(hTheme);
    }
}

void DiscardDeviceResources()
{
    SafeRelease(g_pRT);
    SafeRelease(g_pBrushText);
    SafeRelease(g_pBrushIcon);
    SafeRelease(g_pBrushWhite);
    SafeRelease(g_pBrushMinNormal);
    SafeRelease(g_pBrushMinHover);
    SafeRelease(g_pBrushMinPressed);
    SafeRelease(g_pBrushMaxNormal);
    SafeRelease(g_pBrushMaxHover);
    SafeRelease(g_pBrushMaxPressed);
    SafeRelease(g_pBrushCloseNormal);
    SafeRelease(g_pBrushCloseHover);
    SafeRelease(g_pBrushClosePressed);
}

void LayoutButtons(int width)
{
    int btnW = g_scaledTitlebarHeight; // ボタンは正方形
    int marginRight = 0; // Win11スタイルでは右端に隙間はない
    int x = width - marginRight;

    g_btnClose.rc = { x - btnW, 0, x, g_scaledTitlebarHeight };
    x -= btnW;

    g_btnMax.rc = { x - btnW, 0, x, g_scaledTitlebarHeight };
    x -= btnW;

    g_btnMin.rc = { x - btnW, 0, x, g_scaledTitlebarHeight };

    // アイコン領域もDPIスケールに合わせて計算
    g_rcIcon.left = 8;
    g_rcIcon.top = (g_scaledTitlebarHeight - (g_scaledTitlebarHeight - 12)) / 2;
    g_rcIcon.right = g_rcIcon.left + (g_scaledTitlebarHeight - 12); // 少し小さめに
    g_rcIcon.bottom = g_rcIcon.top + (g_scaledTitlebarHeight - 12);
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
    D2D1_POINT_2F p1 = D2D1::Point2F(center.x - size / 2, center.y);
    D2D1_POINT_2F p2 = D2D1::Point2F(center.x + size / 2, center.y);
    rt->DrawLine(p1, p2, brush, strokeWidth);
}

void DrawMaxIcon(ID2D1RenderTarget* rt, D2D1_POINT_2F center, float size, ID2D1Brush* brush, float strokeWidth)
{
    D2D1_RECT_F r = D2D1::RectF(center.x - size / 2, center.y - size / 2, center.x + size / 2, center.y + size / 2);
    rt->DrawRectangle(r, brush, strokeWidth);
}

void DrawRestoreIcon(ID2D1RenderTarget* rt, D2D1_POINT_2F center, float size, ID2D1Brush* brush, float strokeWidth)
{
    float s = size / 2;
    // slightly smaller rectangles for restore icon
    D2D1_RECT_F r_back = D2D1::RectF(center.x - s + 3, center.y - s, center.x + s, center.y + s - 3);
    D2D1_RECT_F r_front = D2D1::RectF(center.x - s, center.y - s + 3, center.x + s - 3, center.y + s);

    // To prevent the background from showing through, we fill the front rectangle first
    // then draw both outlines.
    rt->FillRectangle(r_front, brush); // Fills with the icon brush color
    rt->DrawRectangle(r_back, brush, strokeWidth);
    rt->DrawRectangle(r_front, brush, strokeWidth);
}

void DrawCloseIcon(ID2D1RenderTarget* rt, D2D1_POINT_2F center, float size, ID2D1Brush* brush, float strokeWidth)
{
    float s = size / 2;
    rt->DrawLine(D2D1::Point2F(center.x - s, center.y - s), D2D1::Point2F(center.x + s, center.y + s), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(center.x + s, center.y - s), D2D1::Point2F(center.x - s, center.y + s), brush, strokeWidth);
}

void CreateTextFormat()
{
    if (!g_pDWriteFactory) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory), (IUnknown**)&g_pDWriteFactory);
    }
    SafeRelease(g_pTextFormat);
    if (g_pDWriteFactory) {
        float fontSize = 9.0f * (g_scaledTitlebarHeight / (float)DEFAULT_TITLEBAR_HEIGHT);
        g_pDWriteFactory->CreateTextFormat(
            L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, fontSize, L"ja-jp", &g_pTextFormat);

        if (g_pTextFormat)
        {
            g_pTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            g_pTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
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

    // 背景はDWMに任せるため、ここではクリアしない。
    // 代わりに、透明色(alpha=0)でクリアして、背景が透けて見えるようにする。
    // 背景クリア
    if (IsDarkMode(hwnd)) {
        g_pRT->Clear(D2D1::ColorF(0.11f, 0.11f, 0.11f, 1.0f)); // ダーク背景
    }
    else {
        g_pRT->Clear(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));   // ライト背景
    }

    float btnIconSize = (float)g_scaledTitlebarHeight * 0.28f;
    float stroke = 1.0f * (g_scaledTitlebarHeight / (float)DEFAULT_TITLEBAR_HEIGHT);

    // ウィンドウアイコンの描画
    HICON hIcon = (HICON)GetClassLongPtr(hwnd, GCLP_HICONSM);
    if (!hIcon) hIcon = (HICON)GetClassLongPtr(hwnd, GCLP_HICON);
    if (hIcon) {
        ID2D1Bitmap* pBitmap = nullptr;
        if (SUCCEEDED(CreateBitmapFromHICON(hIcon, g_pRT, &pBitmap)))
        {
            D2D1_RECT_F iconRect = D2D1::RectF(
                (FLOAT)g_rcIcon.left,
                (FLOAT)g_rcIcon.top,
                (FLOAT)g_rcIcon.right,
                (FLOAT)g_rcIcon.bottom);
            g_pRT->DrawBitmap(pBitmap, iconRect);
            SafeRelease(pBitmap);
        }
    }

    // ウィンドウタイトルの描画
    int len = GetWindowTextLengthW(hwnd);
    std::wstring title;
    if (len > 0) {
        std::vector<wchar_t> buf(len + 1);
        GetWindowTextW(hwnd, buf.data(), len + 1);
        title = buf.data();
    }

    if (g_pTextFormat && g_pBrushText && !title.empty())
    {
        D2D1_RECT_F textRect = D2D1::RectF(
            (FLOAT)(g_rcIcon.right + 8),
            0.0f,
            (FLOAT)(g_btnMin.rc.left - 8),
            (FLOAT)g_scaledTitlebarHeight
        );
        g_pRT->DrawTextW(title.c_str(), (UINT32)title.length(), g_pTextFormat, textRect, g_pBrushText);
    }

    // ボタン背景の描画
    auto DrawBtnBG = [&](ButtonState& btn, ID2D1SolidColorBrush* normal, ID2D1SolidColorBrush* hover, ID2D1SolidColorBrush* pressed) {
        ID2D1SolidColorBrush* pBrush = normal;
        // <<< 修正: 非アクティブ時はホバー/押下状態を無視
        if (g_isWindowActive) {
            if (btn.pressed) pBrush = pressed;
            else if (btn.hover) pBrush = hover;
        }

        if (pBrush) {
            g_pRT->FillRectangle(D2D1::RectF((FLOAT)btn.rc.left, (FLOAT)btn.rc.top, (FLOAT)btn.rc.right, (FLOAT)btn.rc.bottom), pBrush);
        }
        };

    DrawBtnBG(g_btnMin, g_pBrushMinNormal, g_pBrushMinHover, g_pBrushMinPressed);
    DrawMinIcon(g_pRT, D2D1::Point2F((g_btnMin.rc.left + g_btnMin.rc.right) / 2.0f, (g_btnMin.rc.top + g_btnMin.rc.bottom) / 2.0f), btnIconSize, g_pBrushIcon, stroke);

    DrawBtnBG(g_btnMax, g_pBrushMaxNormal, g_pBrushMaxHover, g_pBrushMaxPressed);
    if (IsZoomed(hwnd)) {
        DrawRestoreIcon(g_pRT, D2D1::Point2F((g_btnMax.rc.left + g_btnMax.rc.right) / 2.0f, (g_btnMax.rc.top + g_btnMax.rc.bottom) / 2.0f), btnIconSize, g_pBrushIcon, stroke);
    }
    else {
        DrawMaxIcon(g_pRT, D2D1::Point2F((g_btnMax.rc.left + g_btnMax.rc.right) / 2.0f, (g_btnMax.rc.top + g_btnMax.rc.bottom) / 2.0f), btnIconSize, g_pBrushIcon, stroke);
    }

    // クローズボタンはホバー/押下時にアイコンが白になる
    ID2D1SolidColorBrush* closeIconBrush = g_pBrushIcon;
    // <<< 修正: 非アクティブ時はホバー/押下状態を無視
    if (g_isWindowActive && (g_btnClose.hover || g_btnClose.pressed) && g_pBrushWhite) {
        closeIconBrush = g_pBrushWhite;
    }

    DrawBtnBG(g_btnClose, g_pBrushCloseNormal, g_pBrushCloseHover, g_pBrushClosePressed);
    DrawCloseIcon(g_pRT, D2D1::Point2F((g_btnClose.rc.left + g_btnClose.rc.right) / 2.0f, (g_btnClose.rc.top + g_btnClose.rc.bottom) / 2.0f), btnIconSize, closeIconBrush, stroke);

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

        RECT rcClient;
        GetWindowRect(hwnd, &rcClient);
        SetWindowPos(hwnd, NULL, rcClient.left, rcClient.top, rcClient.right - rcClient.left, rcClient.bottom - rcClient.top, SWP_FRAMECHANGED);

        // <<< 修正: Windows 11のMica背景と影を有効にする
        DWM_SYSTEMBACKDROP_TYPE backdrop_type = DWMSBT_MAINWINDOW;
        DwmSetWindowAttribute(hwnd, DWMWA_SYSTEM_BACKDROP_TYPE, &backdrop_type, sizeof(backdrop_type));

        MARGINS margins = { -1 };
        DwmExtendFrameIntoClientArea(hwnd, &margins);

        UINT dpi = GetDpiForWindow(hwnd);
        g_scaledTitlebarHeight = MulDiv(DEFAULT_TITLEBAR_HEIGHT, dpi, 96);

        bool isLight = IsAppsUseLightTheme();
        EnableDarkModeForWindow(hwnd, !isLight);

        RECT rc; GetClientRect(hwnd, &rc);
        LayoutButtons(rc.right);
        break;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    {
        bool isLight = IsAppsUseLightTheme();
        EnableDarkModeForWindow(hwnd, !isLight);
        DiscardDeviceResources();
        InvalidateRect(hwnd, NULL, TRUE);
        break;
    }
    case WM_SIZE:
    {
        if (g_pRT) {
            g_pRT->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        }
        LayoutButtons(LOWORD(lParam));
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    case WM_DPICHANGED:
    {
        UINT dpi = HIWORD(wParam);
        g_scaledTitlebarHeight = MulDiv(DEFAULT_TITLEBAR_HEIGHT, dpi, 96);

        RECT* const prcNewWindow = (RECT*)lParam;
        SetWindowPos(hwnd, NULL,
            prcNewWindow->left, prcNewWindow->top,
            prcNewWindow->right - prcNewWindow->left,
            prcNewWindow->bottom - prcNewWindow->top,
            SWP_NOZORDER | SWP_NOACTIVATE);

        LayoutButtons(prcNewWindow->right - prcNewWindow->left);
        DiscardDeviceResources();
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    case WM_DISPLAYCHANGE:
        DiscardDeviceResources();
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_SETTEXT:
    {
        // デフォルトの処理を呼び出した後、再描画
        LRESULT res = DefWindowProc(hwnd, msg, wParam, lParam);
        InvalidateRect(hwnd, NULL, FALSE);
        return res;
    }
    // <<< 追加: 非クライアント領域のアクティブ化メッセージをハンドル
    case WM_NCACTIVATE:
    {
        // wParamがFALSEの場合、非アクティブ状態になる
        // デフォルトの処理をさせると標準フレームが描画されてしまう
        g_isWindowActive = (BOOL)wParam;
        DiscardDeviceResources(); // 色が変わるためリソースを再作成
        InvalidateRect(hwnd, NULL, FALSE);
        return 1; // デフォルトの処理を抑制
    }
    // <<< 追加: WM_ACTIVATEでも状態を更新
    case WM_ACTIVATE:
    {
        g_isWindowActive = (LOWORD(wParam) != WA_INACTIVE);
        DiscardDeviceResources();
        InvalidateRect(hwnd, NULL, FALSE);
        break; // このメッセージはDefWindowProcにも渡す
    }
    case WM_NCHITTEST:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);

        if (HitTestButtons(pt) != BTN_NONE) return HTCLIENT;
        if (PtInRect(&g_rcIcon, pt)) return HTSYSMENU;

        // ウィンドウのリサイズ処理
        // 最大化されている場合はリサイズを無効化
        if (!IsZoomed(hwnd)) {
            RECT rcWindow;
            GetClientRect(hwnd, &rcWindow); // クライアント領域で判定
            int border_width = 8; // リサイズハンドルの幅 (DPIスケーリングを考慮しても良い)

            bool onLeft = pt.x >= rcWindow.left && pt.x < rcWindow.left + border_width;
            bool onRight = pt.x < rcWindow.right && pt.x >= rcWindow.right - border_width;
            bool onTop = pt.y >= rcWindow.top && pt.y < rcWindow.top + border_width;
            bool onBottom = pt.y < rcWindow.bottom && pt.y >= rcWindow.bottom - border_width;

            if (onLeft && onTop) return HTTOPLEFT;
            if (onRight && onTop) return HTTOPRIGHT;
            if (onLeft && onBottom) return HTBOTTOMLEFT;
            if (onRight && onBottom) return HTBOTTOMRIGHT;
            if (onLeft) return HTLEFT;
            if (onRight) return HTRIGHT;
            if (onTop) return HTTOP;
            if (onBottom) return HTBOTTOM;
        }

        if (pt.y >= 0 && pt.y < g_scaledTitlebarHeight) return HTCAPTION;

        return HTCLIENT;
    }
    case WM_NCLBUTTONDBLCLK:
    {
        if (wParam == HTCAPTION) {
            if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            else ShowWindow(hwnd, SW_MAXIMIZE);
            return 0;
        }
        break;
    }
    case WM_NCRBUTTONUP:
    {
        if (wParam == HTCAPTION || wParam == HTSYSMENU)
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ShowSystemMenu(hwnd, pt);
            return 0;
        }
        break;
    }
    case WM_NCPAINT:
        return 0; // 非クライアント領域の描画を抑制
    case WM_NCCALCSIZE:
        if (wParam == TRUE) {
            return 0;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    case WM_RBUTTONUP:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (PtInRect(&g_rcIcon, pt)) {
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

        auto update_hover = [&](ButtonState& btn, BtnId id) {
            bool is_hover = (h == id);
            if (btn.hover != is_hover) {
                btn.hover = is_hover;
                if (!is_hover) btn.pressed = false;
                needInvalidate = true;
            }
            };

        update_hover(g_btnMin, BTN_MIN);
        update_hover(g_btnMax, BTN_MAX);
        update_hover(g_btnClose, BTN_CLOSE);

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

        if (g_btnMin.pressed && h == BTN_MIN) ShowWindow(hwnd, SW_MINIMIZE);
        else if (g_btnMax.pressed && h == BTN_MAX) {
            if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE); else ShowWindow(hwnd, SW_MAXIMIZE);
        }
        else if (g_btnClose.pressed && h == BTN_CLOSE) PostMessage(hwnd, WM_CLOSE, 0, 0);

        g_btnMin.pressed = g_btnMax.pressed = g_btnClose.pressed = false;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    case WM_PAINT:
        OnPaint(hwnd);
        break;
    case WM_ERASEBKGND:
        return 1; // フリッカー防止
    case WM_DESTROY:
        DiscardDeviceResources();
        SafeRelease(g_pDWriteFactory);
        SafeRelease(g_pFactory);
        SafeRelease(g_pWICFactory);
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmd)
{
    CoInitialize(NULL);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"Win11CustomFrameClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_ICON1));
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"Custom Win11-Style Window",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 600,
        NULL, NULL, hInst, NULL
    );

    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmd);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CoUninitialize();
    return 0;
}