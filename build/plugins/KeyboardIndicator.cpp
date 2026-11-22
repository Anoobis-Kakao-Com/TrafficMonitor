// KeyboardIndicator plugin for TrafficMonitor
// Based on zhongyang219/TrafficMonitorPlugins (MIT License), re-implemented without MFC.
// Change: the border of an indicator that is ON is drawn with TrafficMonitor's
//         "label" colour (colour 1); the text of an ON indicator keeps the "value" colour (colour 2).
#define WIN32_LEAN_AND_MEAN
#include <algorithm>
using std::min;
using std::max;
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include "PluginInterface.h"

namespace {

HINSTANCE g_hInst = nullptr;

// ------------------------------------------------------------------ settings
struct SettingData {
    bool show_caps_lock = true;
    bool show_num_lock = true;
    bool show_scroll_lock = true;
    bool draw_round_rect = false;
    bool bold_text = false;
};

SettingData g_setting;
std::wstring g_configPath;

void LoadConfig(const std::wstring& dir) {
    std::wstring d = dir;
    if (!d.empty() && d.back() != L'\\' && d.back() != L'/') d += L'\\';
    g_configPath = d + L"KeyboardIndicator.ini";
    g_setting.show_caps_lock = GetPrivateProfileIntW(L"config", L"show_caps_lock", 1, g_configPath.c_str()) != 0;
    g_setting.show_num_lock = GetPrivateProfileIntW(L"config", L"show_num_lock", 1, g_configPath.c_str()) != 0;
    g_setting.show_scroll_lock = GetPrivateProfileIntW(L"config", L"show_scroll_lock", 1, g_configPath.c_str()) != 0;
    g_setting.draw_round_rect = GetPrivateProfileIntW(L"config", L"draw_round_rect", 0, g_configPath.c_str()) != 0;
    g_setting.bold_text = GetPrivateProfileIntW(L"config", L"bold_text", 0, g_configPath.c_str()) != 0;
}

void WriteInt(const wchar_t* key, int v) {
    wchar_t buf[16];
    swprintf(buf, 16, L"%d", v);
    WritePrivateProfileStringW(L"config", key, buf, g_configPath.c_str());
}

void SaveConfig() {
    if (g_configPath.empty()) return;
    WriteInt(L"show_caps_lock", g_setting.show_caps_lock);
    WriteInt(L"show_num_lock", g_setting.show_num_lock);
    WriteInt(L"show_scroll_lock", g_setting.show_scroll_lock);
    WriteInt(L"draw_round_rect", g_setting.draw_round_rect);
    WriteInt(L"bold_text", g_setting.bold_text);
}

// ------------------------------------------------------------------ DPI / font / GDI+
int g_dpi = 0;

int DPI(int pixel) {
    if (g_dpi == 0) {
        HDC dc = GetDC(HWND_DESKTOP);
        g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(HWND_DESKTOP, dc);
        if (g_dpi <= 0) g_dpi = 96;
    }
    return g_dpi * pixel / 96;
}
float DPIF(float pixel) { DPI(0); return g_dpi * pixel / 96.0f; }

HFONT g_font = nullptr;
int g_fontPx = 0;
bool g_fontBold = false;

// Height of the em square of the font that TrafficMonitor has selected into the DC = the font size of the
// taskbar text in pixels (must be called before the own font is selected). The boxes of the indicator are
// as high as the letters of the other items, so they follow the font size and the vertical position of
// the text instead of having a fixed size.
int HostFontEmHeight(HDC dc) {
    TEXTMETRICW tm = {};
    int h = 0;
    if (GetTextMetricsW(dc, &tm)) h = tm.tmHeight - tm.tmInternalLeading;
    if (h <= 0) h = DPI(14);          // unknown font
    if (h < DPI(8)) h = DPI(8);
    if (h > DPI(80)) h = DPI(80);
    return h;
}

// font size (pixels) of the letters inside a box: 2/3 of the box height (7pt in a 14 px box, as before)
int LetterPx(int box_height) {
    int px = (box_height * 2 + 1) / 3;
    return px < 6 ? 6 : px;
}

HFONT GetFont(int px) {
    if (g_font && (g_fontPx != px || g_fontBold != g_setting.bold_text)) { DeleteObject(g_font); g_font = nullptr; }
    if (!g_font) {
        LOGFONTW lf = {};
        lf.lfHeight = -px;
        lf.lfWeight = g_setting.bold_text ? FW_BOLD : FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy(lf.lfFaceName, L"Segoe UI");
        g_font = CreateFontIndirectW(&lf);
        g_fontPx = px;
        g_fontBold = g_setting.bold_text;
    }
    return g_font;
}

bool g_gdiplusReady = false;
ULONG_PTR g_gdiplusToken = 0;

bool EnsureGdiplus() {
    if (!g_gdiplusReady) {
        Gdiplus::GdiplusStartupInput in;
        g_gdiplusReady = (Gdiplus::GdiplusStartup(&g_gdiplusToken, &in, nullptr) == Gdiplus::Ok);
    }
    return g_gdiplusReady;
}

// ------------------------------------------------------------------ colours given by TrafficMonitor
bool g_hasLabelColor = false;
COLORREF g_labelColor = 0;    // colour 1 (label colour)

// ------------------------------------------------------------------ drawing
const wchar_t* kCaps = L"Cap";
const wchar_t* kNum = L"Num";
const wchar_t* kScroll = L"Scr";

int TextWidth(HDC dc, const wchar_t* s) {
    SIZE sz = {};
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    return sz.cx;
}

void DrawRectOutLine(HDC dc, RECT rect, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, DPI(1), color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
}

void DrawRoundRectOutLine(HDC dc, RECT rect, COLORREF color) {
    if (!EnsureGdiplus()) { DrawRectOutLine(dc, rect, color); return; }
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
    pen.SetWidth(DPIF(1));

    int x = rect.left;
    int y = rect.top;
    int width = (rect.right - rect.left) - DPI(1);
    int height = (rect.bottom - rect.top) - DPI(1);
    int r = DPI(3);

    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, r * 2, r * 2, 180, 90);
    path.AddArc(x + width - r * 2, y, r * 2, r * 2, 270, 90);
    path.AddArc(x + width - r * 2, y + height - r * 2, r * 2, r * 2, 0, 90);
    path.AddArc(x, y + height - r * 2, r * 2, r * 2, 90, 90);
    path.CloseFigure();
    graphics.DrawPath(&pen, &path);
}

void DrawIndicator(HDC dc, RECT& rect, const wchar_t* text, int text_width, HFONT font, bool dark_mode, bool enable, COLORREF color_ori) {
    COLORREF color_default, color_disable;
    if (dark_mode) {
        color_default = RGB(255, 255, 255);
        color_disable = RGB(145, 145, 145);
    } else {
        color_default = RGB(0, 0, 0);
        color_disable = RGB(140, 140, 140);
    }

    // ON : text = TrafficMonitor value colour (colour 2), border = label colour (colour 1)
    // OFF: both gray
    COLORREF color_on_frame = g_hasLabelColor ? g_labelColor : color_default;
    COLORREF color_text = enable ? color_ori : color_disable;
    COLORREF color_frame = enable ? color_on_frame : color_disable;

    // the width comes from the layout font (as in GetItemWidthEx), so it never depends on the height of the box
    int width = text_width + DPI(4);
    rect.right = rect.left + width;
    if (g_setting.draw_round_rect) DrawRoundRectOutLine(dc, rect, color_frame);
    else DrawRectOutLine(dc, rect, color_frame);

    HGDIOBJ prevFont = SelectObject(dc, font);
    SetTextColor(dc, color_text);
    DrawTextW(dc, text, -1, &rect, DT_VCENTER | DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, prevFont);

    rect.left = rect.right + DPI(2);
    rect.right = rect.left + width;
}

bool IsKeyOn(int vk) { return (GetKeyState(vk) & 0x0001) != 0; }

// ------------------------------------------------------------------ plugin item
class KeyboardIndicatorItem : public IPluginItem {
public:
    const wchar_t* GetItemName() const override { return L"Keyboard indicator"; }
    const wchar_t* GetItemId() const override { return L"3db99u17"; }
    const wchar_t* GetItemLableText() const override { return L""; }
    const wchar_t* GetItemValueText() const override { return L""; }
    const wchar_t* GetItemValueSampleText() const override { return L""; }
    bool IsCustomDraw() const override { return true; }

    int GetItemWidthEx(void* hDC) const override {
        HDC dc = (HDC)hDC;
        int box_h = HostFontEmHeight(dc);
        HGDIOBJ oldFont = SelectObject(dc, GetFont(LetterPx(box_h)));
        // exact width of the drawn boxes (box = text + 4px, 2px between boxes) + 3px trailing gap
        int count = 0;
        int width = 0;
        if (g_setting.show_caps_lock) { width += TextWidth(dc, kCaps) + DPI(4); ++count; }
        if (g_setting.show_num_lock) { width += TextWidth(dc, kNum) + DPI(4); ++count; }
        if (g_setting.show_scroll_lock) { width += TextWidth(dc, kScroll) + DPI(4); ++count; }
        if (count > 1) width += (count - 1) * DPI(2);
        width += DPI(3);
        SelectObject(dc, oldFont);
        return width;
    }

    void DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode) override {
        HDC dc = (HDC)hDC;
        RECT rect = { x, y, x + w, y + h };
        COLORREF color_ori = GetTextColor(dc);   // colour 2 (value colour) set by TrafficMonitor
        int box_h = HostFontEmHeight(dc);
        // widths of the texts with the layout font (the same as in GetItemWidthEx)
        HGDIOBJ oldFont = SelectObject(dc, GetFont(LetterPx(box_h)));
        int w_caps = TextWidth(dc, kCaps);
        int w_num = TextWidth(dc, kNum);
        int w_scr = TextWidth(dc, kScroll);
        SelectObject(dc, oldFont);

        int row_height = rect.bottom - rect.top;
        int item_height = box_h;           // as high as the letters of the other items
        // the drawing area can be smaller than the boxes: the boxes then fit into it
        if (item_height > row_height)
            item_height = row_height;
        // keep the boxes exactly centered: the box height gets the same parity as the row height, otherwise
        // the box sits half a pixel off the text line and "jumps" separately when the vertical margin changes
        if (((row_height - item_height) & 1) != 0)
            item_height += 1;
        RECT ri = rect;
        ri.top += ((rect.bottom - rect.top) - item_height) / 2;
        ri.bottom = ri.top + item_height;

        // the letters scale with the real height of the box: when the vertical margin pushes the row out of the
        // window the boxes get lower, and letters of the old size would be cut off instead of staying centered
        HFONT text_font = GetFont(LetterPx(item_height));

        if (g_setting.show_caps_lock) DrawIndicator(dc, ri, kCaps, w_caps, text_font, dark_mode, IsKeyOn(VK_CAPITAL), color_ori);
        if (g_setting.show_num_lock) DrawIndicator(dc, ri, kNum, w_num, text_font, dark_mode, IsKeyOn(VK_NUMLOCK), color_ori);
        if (g_setting.show_scroll_lock) DrawIndicator(dc, ri, kScroll, w_scr, text_font, dark_mode, IsKeyOn(VK_SCROLL), color_ori);
    }
};

// ------------------------------------------------------------------ options dialog
void Align4(std::vector<BYTE>& b) { while (b.size() % 4) b.push_back(0); }
void AddW(std::vector<BYTE>& b, WORD w) { b.push_back((BYTE)(w & 0xFF)); b.push_back((BYTE)(w >> 8)); }
void AddDW(std::vector<BYTE>& b, DWORD d) { AddW(b, (WORD)(d & 0xFFFF)); AddW(b, (WORD)(d >> 16)); }
void AddStr(std::vector<BYTE>& b, const wchar_t* s) { for (;;) { AddW(b, (WORD)*s); if (!*s) break; ++s; } }
void AddItem(std::vector<BYTE>& b, DWORD style, int x, int y, int cx, int cy, WORD id, WORD cls, const wchar_t* text) {
    Align4(b);
    AddDW(b, style | WS_CHILD | WS_VISIBLE);
    AddDW(b, 0);
    AddW(b, (WORD)x); AddW(b, (WORD)y); AddW(b, (WORD)cx); AddW(b, (WORD)cy);
    AddW(b, id);
    AddW(b, 0xFFFF); AddW(b, cls);
    AddStr(b, text);
    AddW(b, 0);
}

enum { IDC_CAPS = 201, IDC_NUM = 202, IDC_SCROLL = 203, IDC_ROUND = 204, IDC_BOLD = 205 };
// Korean UI texts when Windows uses Korean (same rule as the other plugins)
bool IsKo() {
    static const bool ko = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN;
    return ko;
}
const wchar_t* kDlgTitle = IsKo() ? L"키보드 표시기 설정" : L"Keyboard Indicator Settings";

SettingData g_dlgData;

INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG:
        CheckDlgButton(hDlg, IDC_CAPS, g_dlgData.show_caps_lock ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_NUM, g_dlgData.show_num_lock ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_SCROLL, g_dlgData.show_scroll_lock ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_ROUND, g_dlgData.draw_round_rect ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_BOLD, g_dlgData.bold_text ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            SettingData d;
            d.show_caps_lock = IsDlgButtonChecked(hDlg, IDC_CAPS) != 0;
            d.show_num_lock = IsDlgButtonChecked(hDlg, IDC_NUM) != 0;
            d.show_scroll_lock = IsDlgButtonChecked(hDlg, IDC_SCROLL) != 0;
            d.draw_round_rect = IsDlgButtonChecked(hDlg, IDC_ROUND) != 0;
            d.bold_text = IsDlgButtonChecked(hDlg, IDC_BOLD) != 0;
            if (!d.show_caps_lock && !d.show_num_lock && !d.show_scroll_lock) {
                MessageBoxW(hDlg, IsKo() ? L"항목을 하나 이상 선택해 주세요!" : L"Please select at least one item!", kDlgTitle, MB_ICONWARNING | MB_OK);
                return TRUE;
            }
            g_dlgData = d;
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) { EndDialog(hDlg, IDCANCEL); return TRUE; }
        break;
    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

bool ShowOptions(HWND parent) {
    g_dlgData = g_setting;
    std::vector<BYTE> b;
    AddDW(b, DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    AddDW(b, 0);
    AddW(b, 7);
    AddW(b, 0); AddW(b, 0); AddW(b, 190); AddW(b, 112);
    AddW(b, 0); AddW(b, 0);
    AddStr(b, kDlgTitle);
    AddW(b, 9); AddStr(b, L"Segoe UI");

    const DWORD chk = BS_AUTOCHECKBOX | WS_TABSTOP;
    AddItem(b, chk, 12, 10, 166, 10, IDC_CAPS, 0x0080, IsKo() ? L"Caps Lock 표시" : L"Show Caps Lock");
    AddItem(b, chk, 12, 24, 166, 10, IDC_NUM, 0x0080, IsKo() ? L"Num Lock 표시" : L"Show Num Lock");
    AddItem(b, chk, 12, 38, 166, 10, IDC_SCROLL, 0x0080, IsKo() ? L"Scroll Lock 표시" : L"Show Scroll Lock");
    AddItem(b, chk, 12, 52, 166, 10, IDC_ROUND, 0x0080, IsKo() ? L"둥근 사각형 테두리 사용" : L"Use rounded rectangle borders");
    AddItem(b, chk, 12, 66, 166, 10, IDC_BOLD, 0x0080, IsKo() ? L"굵은 글씨" : L"Bold text");
    AddItem(b, BS_DEFPUSHBUTTON | WS_TABSTOP, 70, 90, 50, 14, IDOK, 0x0080, IsKo() ? L"확인" : L"OK");
    AddItem(b, BS_PUSHBUTTON | WS_TABSTOP, 128, 90, 50, 14, IDCANCEL, 0x0080, IsKo() ? L"취소" : L"Cancel");

    INT_PTR r = DialogBoxIndirectParamW(g_hInst, reinterpret_cast<LPCDLGTEMPLATEW>(b.data()), parent, DlgProc, 0);
    if (r != IDOK) return false;
    g_setting = g_dlgData;
    SaveConfig();
    return true;
}

// ------------------------------------------------------------------ plugin
HICON g_icon = nullptr;

class KeyboardIndicatorPlugin : public ITMPlugin {
public:
    IPluginItem* GetItem(int index) override { return index == 0 ? &m_item : nullptr; }
    const wchar_t* GetTooltipInfo() override { return L""; }
    void DataRequired() override {}

    // Right-click menu entry -> opens the same options dialog (like the GPU memory plugin)
    int GetCommandCount() override { return 1; }
    const wchar_t* GetCommandName(int index) override {
        return index == 0 ? (IsKo() ? L"키보드 표시기 설정..." : L"Keyboard indicator options...") : nullptr;
    }
    void OnPluginCommand(int index, void* hWnd, void*) override {
        if (index != 0) return;
        try { ShowOptions((HWND)hWnd); } catch (...) {}
    }

    OptionReturn ShowOptionsDialog(void* hParent) override {
        try { return ShowOptions((HWND)hParent) ? OR_OPTION_CHANGED : OR_OPTION_UNCHANGED; }
        catch (...) { return OR_OPTION_UNCHANGED; }
    }

    const wchar_t* GetInfo(PluginInfoIndex index) override {
        switch (index) {
        case TMI_NAME: return L"Keyboard indicator";
        case TMI_DESCRIPTION: return L"A plugin for indicating the status of keyboard Caps Lock, Num Lock, Scroll Lock, etc.";
        case TMI_AUTHOR: return L"zhongyang219";
        case TMI_COPYRIGHT: return L"Copyright (C) 2025 By ZhongYang";
        case TMI_URL: return L"https://github.com/zhongyang219/TrafficMonitorPlugins";
        case TMI_VERSION: return L"1.08";
        default: return L"";
        }
    }

    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override {
        if (!data) return;
        switch (index) {
        case EI_CONFIG_DIR:
            LoadConfig(std::wstring(data));
            break;
        case EI_LABEL_TEXT_COLOR:
            g_labelColor = (COLORREF)wcstoul(data, nullptr, 10);
            g_hasLabelColor = true;
            break;
        default:
            break;
        }
    }

    void* GetPluginIcon() override {
        if (!g_icon) g_icon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, DPI(16), DPI(16), 0);
        return g_icon;
    }

private:
    KeyboardIndicatorItem m_item;
};

KeyboardIndicatorPlugin g_plugin;

}  // namespace

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance() { return &g_plugin; }

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hInst;
        DisableThreadLibraryCalls(hInst);
    }
    return TRUE;
}
