// Win11CustomFrame_SingleTitle.cpp
// Build: link with d2d1.lib dwmapi.lib
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <string>
#pragma comment(lib, "d2d1.lib")

// simple SafeRelease
template<class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ----------------- 設定 -----------------
constexpr int TITLEBAR_HEIGHT = 34; // タイトルバー高さ（px）
static HWND g_hWnd = nullptr;

// Direct2D
static ID2D1Factory* g_pFactory = nullptr;
static ID2D1HwndRenderTarget* g_pRT = nullptr;
static ID2D1SolidColorBrush* g_pBrushText = nullptr;
static ID2D1SolidColorBrush* g_pBrushIcon = nullptr;
static ID2D1SolidColorBrush* g_pBrushHover = nullptr;
static ID2D1SolidColorBrush* g_pBrushPressed = nullptr;

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

bool IsDarkMode()
{
    COLORREF clr = GetSysColor(COLOR_WINDOWTEXT);
    int lum = (GetRValue(clr) * 299 + GetGValue(clr) * 587 + GetBValue(clr) * 114) / 1000;
    return lum < 128;
}

void UpdateColors()
{
    if (IsDarkMode()) {
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
    UpdateColors();
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

void OnPaint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    CreateDeviceResources(hwnd);
    if (!g_pRT) { EndPaint(hwnd, &ps); return; }

    g_pRT->BeginDraw();
    // 背景: クライアント全体を背景色で塗る（タイトルは自前で塗る）
    RECT rcWnd; GetClientRect(hwnd, &rcWnd);
    D2D1_RECT_F full = D2D1::RectF(0.0f, 0.0f, (FLOAT)rcWnd.right, (FLOAT)rcWnd.bottom);
    // ここでは背景をウィンドウのクライアント色で塗る（透明にする場合は変更）
    g_pRT->Clear(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));

    // タイトル領域は自前で描く（背景の有無は好みで調整）
    D2D1_RECT_F titleRect = D2D1::RectF(0.0f, 0.0f, (FLOAT)rcWnd.right, (FLOAT)TITLEBAR_HEIGHT);
    // 例: タイトル背景を透明にしてDWMの影を活かしたい場合は Fill をしない
    // g_pRT->FillRectangle(titleRect, g_pBrushText); // 使わない

    // タイトル文字（ここでは簡易に GDI で描画）
    std::wstring title = L"My App - Custom Win11 Style Titlebar";
    SetTextColor(hdc, (DWORD)g_textColor);
    RECT textRc = { 8, 0, rcWnd.right - 150, TITLEBAR_HEIGHT };
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, title.c_str(), (int)title.length(), &textRc, DT_VCENTER | DT_SINGLELINE | DT_LEFT);

    // Direct2D でボタンを描画
    float btnIconSize = (float)TITLEBAR_HEIGHT * 0.35f;
    float stroke = 1.6f;

    // Min
    if (g_btnMin.hover) {
        ID2D1Brush* b = g_btnMin.pressed ? g_pBrushPressed : g_pBrushHover;
        g_pRT->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF((FLOAT)g_btnMin.rc.left, (FLOAT)g_btnMin.rc.top, (FLOAT)g_btnMin.rc.right, (FLOAT)g_btnMin.rc.bottom), 6.0f, 6.0f), b);
    }
    DrawMinIcon(g_pRT, D2D1::Point2F((g_btnMin.rc.left + g_btnMin.rc.right) / 2.0f, (g_btnMin.rc.top + g_btnMin.rc.bottom) / 2.0f), btnIconSize, g_pBrushIcon, stroke);

    // Max/Restore
    if (g_btnMax.hover) {
        ID2D1Brush* b = g_btnMax.pressed ? g_pBrushPressed : g_pBrushHover;
        g_pRT->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF((FLOAT)g_btnMax.rc.left, (FLOAT)g_btnMax.rc.top, (FLOAT)g_btnMax.rc.right, (FLOAT)g_btnMax.rc.bottom), 6.0f, 6.0f), b);
    }
    DrawRestoreIcon(g_pRT, D2D1::Point2F((g_btnMax.rc.left + g_btnMax.rc.right) / 2.0f, (g_btnMax.rc.top + g_btnMax.rc.bottom) / 2.0f), btnIconSize, g_pBrushIcon, stroke);

    // Close (赤系)
    if (g_btnClose.hover) {
        ID2D1SolidColorBrush* rb = nullptr;
        g_pRT->CreateSolidColorBrush(D2D1::ColorF(0.89f, 0.22f, 0.21f, g_btnClose.pressed ? 0.18f : 0.12f), &rb);
        g_pRT->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF((FLOAT)g_btnClose.rc.left, (FLOAT)g_btnClose.rc.top, (FLOAT)g_btnClose.rc.right, (FLOAT)g_btnClose.rc.bottom), 6.0f, 6.0f), rb);
        SafeRelease(rb);
    }
    DrawCloseIcon(g_pRT, D2D1::Point2F((g_btnClose.rc.left + g_btnClose.rc.right) / 2.0f, (g_btnClose.rc.top + g_btnClose.rc.bottom) / 2.0f), btnIconSize, g_pBrushIcon, stroke);

    HRESULT hr = g_pRT->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) DiscardDeviceResources();

    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
    {
        g_hWnd = hwnd;

        // --- 重要: 標準のタイトルバーを削除して完全自作にする ---
        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
        style &= ~WS_CAPTION; // タイトルバーを消す
        SetWindowLongPtr(hwnd, GWL_STYLE, style);
        // フレーム変更を即反映
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

        CreateDeviceResources(hwnd);
        RECT rc; GetClientRect(hwnd, &rc);
        LayoutButtons(rc.right);
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
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"Win11CustomFrameClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    // CreateWindowEx で通常ウィンドウを作る（後で WS_CAPTION を消す）
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Custom Win11-Style Window", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 600, NULL, NULL, hInst, NULL);

    if (!hwnd) return 0;

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
