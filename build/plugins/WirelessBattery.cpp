// WirelessBattery - Bluetooth device battery plugin for TrafficMonitor (Win32 + GDI+, built with MinGW-w64).
//
// Based on the Battery plugin of TrafficMonitorPlugins (MIT, zhongyang219): same settings, same battery icon
// (taller body, see make_battery_icons.py), same colors, same "host draws the number" cooperation with the patched
// TrafficMonitor (exported TMHostTextIconWidth). Differences:
//   - 10 fixed slots (items); every slot is assigned to a Bluetooth device in the options dialog (saved in the ini)
//   - the battery body contains two small glyphs: the Bluetooth mark and the device type (keyboard, mouse, headset)
//   - the device list is refreshed every 10 seconds in a background thread
// Value text: "NN%" | "N/A" (not connected / no battery information) | "--" (slot empty or device not in the list)
//
// License: MIT (derived from the MIT licensed Battery plugin). Modifying this file is allowed.
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <algorithm>
using std::min;
using std::max;
#include <winsock2.h>
#include <windows.h>
#include <setupapi.h>
#include <devpropdef.h>
#include <bluetoothapis.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include "PluginInterface.h"

namespace {

HINSTANCE g_hInst = nullptr;
const int kSlots = 10;
const DWORD kRefreshMs = 10000;       // device list / battery refresh interval

bool IsKo() {
    static const bool ko = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN;
    return ko;
}

// ------------------------------------------------------------------ DPI
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

// ------------------------------------------------------------------ settings (same keys as the Battery plugin)
enum { TYPE_NUMBER = 0, TYPE_ICON = 1, TYPE_NUMBER_BESIDE_ICON = 2 };
struct Settings {
    int type = TYPE_NUMBER_BESIDE_ICON;
    bool tooltip = true;
    bool percent = true;
    bool animation = false;        // kept for the identical settings; only used when a device reports "charging"
};
struct Slot {
    std::wstring id;               // Bluetooth address (12 hex digits), empty = not used
    std::wstring name;             // last known name (shown in the options when the device is gone)
};
Settings g_set;
Slot g_slots[kSlots];
std::wstring g_ini;
bool g_host_draws_text = false;    // one-shot request of the patched host: draw only the icon

std::wstring ModuleFileName() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_hInst, path, MAX_PATH);
    std::wstring p = path;
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

std::wstring ReadString(const wchar_t* key) {
    wchar_t buf[256] = {};
    GetPrivateProfileStringW(L"config", key, L"", buf, 256, g_ini.c_str());
    return buf;
}
void WriteString(const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(L"config", key, v.c_str(), g_ini.c_str());
}
void WriteInt(const wchar_t* key, int v) {
    wchar_t buf[16];
    swprintf(buf, 16, L"%d", v);
    WritePrivateProfileStringW(L"config", key, buf, g_ini.c_str());
}

void LoadConfig(const std::wstring& dir) {
    std::wstring d = dir;
    if (!d.empty() && d.back() != L'\\' && d.back() != L'/') d += L'\\';
    g_ini = d + ModuleFileName() + L".ini";
    g_set.type = (int)GetPrivateProfileIntW(L"config", L"battery_type", TYPE_NUMBER_BESIDE_ICON, g_ini.c_str());
    if (g_set.type < 0 || g_set.type > 2) g_set.type = TYPE_NUMBER_BESIDE_ICON;
    g_set.tooltip = GetPrivateProfileIntW(L"config", L"show_battery_in_tooltip", 1, g_ini.c_str()) != 0;
    g_set.percent = GetPrivateProfileIntW(L"config", L"show_percent", 1, g_ini.c_str()) != 0;
    g_set.animation = GetPrivateProfileIntW(L"config", L"show_charging_animation", 0, g_ini.c_str()) != 0;
    for (int i = 0; i < kSlots; ++i) {
        wchar_t k1[32], k2[32];
        swprintf(k1, 32, L"slot%d_id", i + 1);
        swprintf(k2, 32, L"slot%d_name", i + 1);
        g_slots[i].id = ReadString(k1);
        g_slots[i].name = ReadString(k2);
    }
}

void SaveConfig() {
    if (g_ini.empty()) return;
    WriteInt(L"battery_type", g_set.type);
    WriteInt(L"show_battery_in_tooltip", g_set.tooltip);
    WriteInt(L"show_percent", g_set.percent);
    WriteInt(L"show_charging_animation", g_set.animation);
    for (int i = 0; i < kSlots; ++i) {
        wchar_t k1[32], k2[32];
        swprintf(k1, 32, L"slot%d_id", i + 1);
        swprintf(k2, 32, L"slot%d_name", i + 1);
        WriteString(k1, g_slots[i].id);
        WriteString(k2, g_slots[i].name);
    }
}

// ------------------------------------------------------------------ Bluetooth devices
enum DevType { DT_OTHER = 0, DT_KEYBOARD = 1, DT_MOUSE = 2, DT_HEADSET = 3 };
struct Device {
    std::wstring id;               // address, 12 upper case hex digits
    std::wstring name;
    bool connected = false;
    int battery = -1;              // 0 - 100, -1 = not reported by the device / Windows
    bool charging = false;         // Windows does not report this for most devices
    int type = DT_OTHER;
    bool from_classic = false;
};

CRITICAL_SECTION g_cs;
std::vector<Device> g_devices;
bool g_scanned = false;
HANDLE g_thread = nullptr;
HANDLE g_stop = nullptr;

std::wstring HexAddr(ULONGLONG a) {
    static const wchar_t* digits = L"0123456789ABCDEF";
    std::wstring s(12, L'0');
    for (int i = 11; i >= 0; --i) { s[i] = digits[a & 0xF]; a >>= 4; }
    return s;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

int GuessTypeFromName(const std::wstring& name) {
    std::wstring n = Lower(name);
    static const wchar_t* kb[] = { L"keyboard", L"키보드", L"kbd", L"keys" };
    static const wchar_t* ms[] = { L"mouse", L"마우스", L"mx master", L"trackball", L"trackpad" };
    static const wchar_t* hs[] = { L"headset", L"headphone", L"earbud", L"buds", L"airpods", L"이어폰", L"헤드셋", L"헤드폰", L"earphone" };
    for (auto s : kb) if (n.find(s) != std::wstring::npos) return DT_KEYBOARD;
    for (auto s : ms) if (n.find(s) != std::wstring::npos) return DT_MOUSE;
    for (auto s : hs) if (n.find(s) != std::wstring::npos) return DT_HEADSET;
    return DT_OTHER;
}

int TypeFromClass(ULONG cod) {
    int major = (cod >> 8) & 0x1F;
    if (major == 5) {                         // peripheral: bits 7-6 of the class: 1 keyboard, 2 pointing device, 3 combo
        int sub = (cod >> 6) & 3;
        if (sub == 2) return DT_MOUSE;
        if (sub == 1 || sub == 3) return DT_KEYBOARD;
        return DT_OTHER;
    }
    if (major == 4) return DT_HEADSET;        // audio / video (headset, hands-free, headphones, ...)
    return DT_OTHER;
}

// A device node of the Bluetooth enumerators: BTHENUM\DEV_<address>\... (classic) or BTHLE\DEV_<address>\... (low energy)
struct Node {
    std::wstring id, name;
    int battery = -1;
    int connected = -1;            // -1 unknown, 0 no, 1 yes
};

bool ParseAddressFromInstanceId(const std::wstring& inst, std::wstring& addr) {
    if (inst.find(L'{') != std::wstring::npos) return false;      // service nodes
    size_t p = inst.find(L"\\DEV_");
    if (p == std::wstring::npos) return false;
    p += 5;
    if (inst.size() < p + 12) return false;
    addr.clear();
    for (size_t i = 0; i < 12; ++i) {
        wchar_t c = (wchar_t)towupper(inst[p + i]);
        if (!((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F'))) return false;
        addr.push_back(c);
    }
    return true;
}

const DEVPROPKEY kKeyFriendlyName = { { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14 };
const DEVPROPKEY kKeyName = { { 0xB725F130, 0x47EF, 0x101A, { 0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC } }, 10 };
const DEVPROPKEY kKeyBattery = { { 0x104EA319, 0x6EE2, 0x4701, { 0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5 } }, 2 };
const DEVPROPKEY kKeyConnected = { { 0x83DA6326, 0x97A6, 0x4088, { 0x94, 0x53, 0xA1, 0x92, 0x3F, 0x57, 0x3B, 0x29 } }, 15 };

void ScanNodes(const wchar_t* enumerator, std::vector<Node>& out) {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, enumerator, nullptr, DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA dd;
    dd.cbSize = sizeof(dd);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dd); ++i) {
        wchar_t inst[512] = {};
        if (!SetupDiGetDeviceInstanceIdW(set, &dd, inst, 512, nullptr)) continue;
        Node n;
        if (!ParseAddressFromInstanceId(inst, n.id)) continue;

        DEVPROPTYPE type = 0;
        DWORD need = 0;
        wchar_t nm[256] = {};
        if (SetupDiGetDevicePropertyW(set, &dd, &kKeyFriendlyName, &type, (PBYTE)nm, sizeof(nm) - 2, &need, 0) && nm[0])
            n.name = nm;
        else if (SetupDiGetDevicePropertyW(set, &dd, &kKeyName, &type, (PBYTE)nm, sizeof(nm) - 2, &need, 0) && nm[0])
            n.name = nm;

        BYTE b = 0;
        if (SetupDiGetDevicePropertyW(set, &dd, &kKeyBattery, &type, &b, sizeof(b), &need, 0) && b <= 100)
            n.battery = b;
        BYTE c = 0;
        if (SetupDiGetDevicePropertyW(set, &dd, &kKeyConnected, &type, &c, sizeof(c), &need, 0))
            n.connected = c ? 1 : 0;
        out.push_back(n);
    }
    SetupDiDestroyDeviceInfoList(set);
}

std::vector<Device> ScanDevices() {
    std::vector<Device> list;

    // 1) classic Bluetooth devices that are paired (remembered / authenticated)
    BLUETOOTH_DEVICE_SEARCH_PARAMS params;
    ZeroMemory(&params, sizeof(params));
    params.dwSize = sizeof(params);
    params.fReturnAuthenticated = TRUE;
    params.fReturnRemembered = TRUE;
    params.fReturnUnknown = FALSE;
    params.fReturnConnected = TRUE;
    params.fIssueInquiry = FALSE;       // never start a (slow) radio inquiry
    params.cTimeoutMultiplier = 0;
    params.hRadio = nullptr;
    BLUETOOTH_DEVICE_INFO info;
    ZeroMemory(&info, sizeof(info));
    info.dwSize = sizeof(info);
    HBLUETOOTH_DEVICE_FIND hFind = BluetoothFindFirstDevice(&params, &info);
    if (hFind != nullptr) {
        do {
            Device d;
            d.id = HexAddr(info.Address.ullLong);
            d.name = info.szName;
            d.connected = info.fConnected != FALSE;
            d.type = TypeFromClass(info.ulClassofDevice);
            d.from_classic = true;
            if (d.name.empty()) d.name = d.id;
            if (d.type == DT_OTHER) d.type = GuessTypeFromName(d.name);
            list.push_back(d);
            ZeroMemory(&info, sizeof(info));
            info.dwSize = sizeof(info);
        } while (BluetoothFindNextDevice(hFind, &info));
        BluetoothFindDeviceClose(hFind);
    }

    // 2) device nodes: battery level of classic devices, and the low energy devices (HID mice / keyboards, ...)
    std::vector<Node> classic_nodes, le_nodes;
    ScanNodes(L"BTHENUM", classic_nodes);
    ScanNodes(L"BTHLE", le_nodes);

    for (const Node& n : classic_nodes) {
        for (auto& d : list) {
            if (d.id != n.id) continue;
            if (n.battery >= 0) d.battery = n.battery;
            if (n.connected == 1) d.connected = true;
        }
    }
    for (const Node& n : le_nodes) {
        Device* found = nullptr;
        for (auto& d : list) if (d.id == n.id) { found = &d; break; }
        if (found == nullptr) {
            Device d;
            d.id = n.id;
            d.name = n.name.empty() ? n.id : n.name;
            d.connected = n.connected == 1;
            d.type = GuessTypeFromName(d.name);
            d.battery = n.battery;
            list.push_back(d);
        } else {
            if (n.battery >= 0 && found->battery < 0) found->battery = n.battery;
            if (n.connected == 1) found->connected = true;
        }
    }
    for (auto& d : list) if (!d.connected) d.battery = -1;     // a value of a device that is not connected is not valid
    std::sort(list.begin(), list.end(), [](const Device& a, const Device& b) { return Lower(a.name) < Lower(b.name); });
    return list;
}

void ScanNow() {
    std::vector<Device> list;
    try { list = ScanDevices(); } catch (...) { return; }
    EnterCriticalSection(&g_cs);
    g_devices.swap(list);
    g_scanned = true;
    // remember the latest names of the assigned devices
    for (int i = 0; i < kSlots; ++i) {
        if (g_slots[i].id.empty()) continue;
        for (const auto& d : g_devices) if (d.id == g_slots[i].id) { g_slots[i].name = d.name; break; }
    }
    LeaveCriticalSection(&g_cs);
}

DWORD WINAPI ScanThread(LPVOID) {
    for (;;) {
        ScanNow();
        if (WaitForSingleObject(g_stop, kRefreshMs) == WAIT_OBJECT_0) break;
    }
    return 0;
}

void EnsureThread() {
    if (g_thread != nullptr) return;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
}

std::vector<Device> Snapshot() {
    EnterCriticalSection(&g_cs);
    std::vector<Device> copy = g_devices;
    LeaveCriticalSection(&g_cs);
    return copy;
}

// ------------------------------------------------------------------ state of a slot
enum SlotState { SS_EMPTY, SS_MISSING, SS_DISCONNECTED, SS_NODATA, SS_OK };
struct SlotInfo {
    SlotState state = SS_EMPTY;
    Device dev;
};

SlotInfo GetSlotInfo(int slot) {
    SlotInfo si;
    if (slot < 0 || slot >= kSlots) return si;
    EnterCriticalSection(&g_cs);
    std::wstring id = g_slots[slot].id;
    bool scanned = g_scanned;
    if (id.empty() || !scanned) {
        si.state = SS_EMPTY;
    } else {
        si.state = SS_MISSING;
        for (const auto& d : g_devices) {
            if (d.id != id) continue;
            si.dev = d;
            if (!d.connected) si.state = SS_DISCONNECTED;
            else if (d.battery < 0) si.state = SS_NODATA;
            else si.state = SS_OK;
            break;
        }
    }
    LeaveCriticalSection(&g_cs);
    return si;
}

std::wstring ValueText(const SlotInfo& si) {
    switch (si.state) {
    case SS_OK: return std::to_wstring(si.dev.battery) + (g_set.percent ? L"%" : L"");
    case SS_DISCONNECTED:
    case SS_NODATA: return L"N/A";
    default: return std::wstring(L"--") + (g_set.percent ? L"%" : L"");
    }
}

// ------------------------------------------------------------------ drawing
ULONG_PTR g_gdiplusToken = 0;
void EnsureGdiplus() {
    if (g_gdiplusToken != 0) return;
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &in, nullptr);
}

enum { IDI_BATTERY_DARK = 104, IDI_BATTERY_LIGHT = 105, IDI_FILL_LOW = 109 };
HICON g_icons[3] = {};
HICON GetIcon(int id) {
    int idx = id == IDI_BATTERY_DARK ? 0 : (id == IDI_BATTERY_LIGHT ? 1 : 2);
    if (!g_icons[idx])
        g_icons[idx] = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(id), IMAGE_ICON, DPI(16), DPI(16), 0);
    return g_icons[idx];
}

// Colors (the same as the Battery plugin; gray = the off color of the keyboard indicator plugin)
const COLORREF kRed = RGB(206, 23, 0);

// All glyph coordinates are on the 16 px grid of the battery icon (ix, iy = top left of the icon, k = icon size / 16).
void DrawGlyphs(Gdiplus::Graphics& g, float ix, float iy, float k, COLORREF color, int dev_type) {
    Gdiplus::Color col(255, GetRValue(color), GetGValue(color), GetBValue(color));
    const float pen_w = (0.9f * k < 1.0f) ? 1.0f : 0.9f * k;
    Gdiplus::Pen pen(col, pen_w);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    Gdiplus::SolidBrush brush(col);

    const float gw = 4.2f, gh = 6.2f;                 // glyph box
    const float gap = (13.0f - 2 * gw) / 3.0f;       // equal gaps: left | glyph | gap | glyph | right
    const float y0 = 8.0f - gh / 2;
    const float x1 = 1.0f + gap;
    const float x2 = x1 + gw + gap;
    auto P = [&](float bx, float by, float rx, float ry) {
        return Gdiplus::PointF(ix + (bx + gw * rx) * k, iy + (by + gh * ry) * k);
    };

    // left glyph: Bluetooth mark
    {
        Gdiplus::PointF pts[6] = { P(x1, y0, 0.0f, 0.27f), P(x1, y0, 1.0f, 0.73f), P(x1, y0, 0.5f, 1.0f),
                                   P(x1, y0, 0.5f, 0.0f), P(x1, y0, 1.0f, 0.27f), P(x1, y0, 0.0f, 0.73f) };
        g.DrawLines(&pen, pts, 6);
    }
    // right glyph: device type
    switch (dev_type) {
    case DT_KEYBOARD: {
        Gdiplus::RectF r(ix + x2 * k, iy + (y0 + gh * 0.26f) * k, gw * k, gh * 0.48f * k);
        g.DrawRectangle(&pen, r);
        g.DrawLine(&pen, Gdiplus::PointF(r.X + r.Width * 0.22f, r.Y + r.Height * 0.68f), Gdiplus::PointF(r.X + r.Width * 0.78f, r.Y + r.Height * 0.68f));
        break;
    }
    case DT_MOUSE: {
        float mw = gw * 0.66f;
        Gdiplus::RectF r(ix + (x2 + (gw - mw) / 2) * k, iy + y0 * k, mw * k, gh * k);
        g.DrawRectangle(&pen, r);
        g.DrawLine(&pen, Gdiplus::PointF(r.X + r.Width / 2, r.Y), Gdiplus::PointF(r.X + r.Width / 2, r.Y + r.Height * 0.4f));
        break;
    }
    case DT_HEADSET: {
        Gdiplus::RectF arc(ix + (x2 + 0.3f) * k, iy + y0 * k, (gw - 0.6f) * k, gh * 0.95f * k);
        g.DrawArc(&pen, arc, 180.0f, 180.0f);
        g.FillRectangle(&brush, ix + x2 * k, iy + (y0 + gh * 0.45f) * k, gw * 0.28f * k, gh * 0.42f * k);
        g.FillRectangle(&brush, ix + (x2 + gw * 0.72f) * k, iy + (y0 + gh * 0.45f) * k, gw * 0.28f * k, gh * 0.42f * k);
        break;
    }
    default: {
        float d = gw * 0.7f;
        g.FillEllipse(&brush, ix + (x2 + (gw - d) / 2) * k, iy + (8.0f - d / 2) * k, d * k, d * k);
        break;
    }
    }
}

void FillRoundRect(Gdiplus::Graphics& g, Gdiplus::RectF r, float radius, COLORREF color) {
    Gdiplus::GraphicsPath path;
    float d = radius * 2;
    if (d > r.Height) d = r.Height;
    if (d > r.Width) d = r.Width;
    if (d <= 0.5f) {
        Gdiplus::SolidBrush b0(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
        g.FillRectangle(&b0, r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
    Gdiplus::SolidBrush b(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
    g.FillPath(&b, &path);
}

class BtItem : public IPluginItem {
public:
    explicit BtItem(int slot) : m_slot(slot) {
        swprintf(m_id, 16, L"BtBat%02d", slot + 1);
    }
    const wchar_t* GetItemName() const override {
        m_name = (IsKo() ? L"블루투스 배터리 " : L"Bluetooth battery ") + std::to_wstring(m_slot + 1);
        EnterCriticalSection(&g_cs);
        std::wstring dev = g_slots[m_slot].name;
        bool used = !g_slots[m_slot].id.empty();
        LeaveCriticalSection(&g_cs);
        if (used && !dev.empty()) m_name += L" (" + dev + L")";
        return m_name.c_str();
    }
    const wchar_t* GetItemId() const override { return m_id; }
    const wchar_t* GetItemLableText() const override { return L""; }     // the label is set in TrafficMonitor (like the Battery plugin)
    const wchar_t* GetItemValueText() const override {
        m_value = ValueText(GetSlotInfo(m_slot));
        return m_value.c_str();
    }
    const wchar_t* GetItemValueSampleText() const override { return g_set.percent ? L"100%" : L"100"; }
    bool IsCustomDraw() const override { return g_set.type != TYPE_NUMBER; }

    int GetItemWidthEx(void* hDC) const override {
        HDC dc = (HDC)hDC;
        const wchar_t* sample = GetItemValueSampleText();
        SIZE sz = {};
        GetTextExtentPoint32W(dc, sample, (int)wcslen(sample), &sz);
        switch (g_set.type) {
        case TYPE_ICON: return DPI(20);
        default: return DPI(20) + sz.cx;
        }
    }

    void DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode) override {
        HDC dc = (HDC)hDC;
        EnsureGdiplus();
        SlotInfo si = GetSlotInfo(m_slot);
        std::wstring text = ValueText(si);

        const int icon_size = DPI(16);
        bool host_draws_text = g_host_draws_text && g_set.type == TYPE_NUMBER_BESIDE_ICON;
        g_host_draws_text = false;
        int content_w = icon_size;
        if (!host_draws_text && g_set.type == TYPE_NUMBER_BESIDE_ICON) {
            SIZE sz = {};
            GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
            content_w += DPI(2) + sz.cx;
        }
        int block_offset = host_draws_text ? 0 : (w - content_w) / 2;
        if (block_offset < 0) block_offset = 0;
        const int ix = x + block_offset;
        const int iy = y + (h - icon_size) / 2;

        // frame: same icons as the Battery plugin (no bolt)
        DrawIconEx(dc, ix, iy, GetIcon(dark_mode ? IDI_BATTERY_LIGHT : IDI_BATTERY_DARK), icon_size, icon_size, 0, nullptr, DI_NORMAL);

        const COLORREF frame_color = dark_mode ? RGB(255, 255, 255) : RGB(64, 64, 64);
        const COLORREF off_color = dark_mode ? RGB(145, 145, 145) : RGB(140, 140, 140);   // = keyboard indicator "off"
        const float k = icon_size / 16.0f;
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        COLORREF glyph_color = frame_color;
        bool draw_glyphs = true;
        switch (si.state) {
        case SS_OK: {
            const int pct = si.dev.battery;
            if (pct >= 100) {
                // 100 %: yellow, no matter what (same rule as the Battery plugin)
                DrawIconEx(dc, ix, iy, GetIcon(IDI_FILL_LOW), icon_size, icon_size, 0, nullptr, DI_NORMAL);
                glyph_color = RGB(50, 50, 50);
            } else if (pct > 0) {
                // not charging / charging state unknown: red fill, same geometry as the Battery plugin
                Gdiplus::RectF rc((float)ix + DPIF(1.0f), (float)iy + DPIF(4.2f), DPIF(11.7f) * pct / 100.0f, DPIF(7.6f));
                FillRoundRect(g, rc, (float)DPI(1), kRed);
            }
            break;
        }
        case SS_NODATA: glyph_color = kRed; break;                 // connected, but no battery information
        case SS_DISCONNECTED: glyph_color = off_color; break;
        default: draw_glyphs = false; break;                        // slot empty / device missing: empty battery
        }
        if (draw_glyphs)
            DrawGlyphs(g, (float)ix, (float)iy, k, glyph_color, si.dev.type);

        if (g_set.type == TYPE_NUMBER_BESIDE_ICON && !host_draws_text) {
            g.Flush(Gdiplus::FlushIntentionSync);
            RECT rc = { ix + icon_size + DPI(2), y, x + w, y + h };
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, text.c_str(), -1, &rc, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }

private:
    int m_slot;
    wchar_t m_id[16] = {};
    mutable std::wstring m_name;
    mutable std::wstring m_value;
};

// ------------------------------------------------------------------ options dialog (built in memory)
void AddW(std::vector<BYTE>& b, WORD v) { b.push_back((BYTE)(v & 0xFF)); b.push_back((BYTE)(v >> 8)); }
void AddDW(std::vector<BYTE>& b, DWORD v) { AddW(b, (WORD)(v & 0xFFFF)); AddW(b, (WORD)(v >> 16)); }
void AddStr(std::vector<BYTE>& b, const wchar_t* s) { do { AddW(b, *s); } while (*s++); }
void Align4(std::vector<BYTE>& b) { while (b.size() % 4) b.push_back(0); }
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

enum { IDC_TYPE = 200, IDC_TIP = 201, IDC_PCT = 202, IDC_ANIM = 203, IDC_SLOT0 = 300 };
std::vector<Device> g_dlgDevices;
Settings g_dlgSet;
Slot g_dlgSlots[kSlots];
std::vector<int> g_dlgExtra(kSlots, 0);      // 1 if the slot has an extra "not in the list" entry

INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG: {
        HWND ct = GetDlgItem(hDlg, IDC_TYPE);
        SendMessageW(ct, CB_ADDSTRING, 0, (LPARAM)(IsKo() ? L"숫자" : L"Number"));
        SendMessageW(ct, CB_ADDSTRING, 0, (LPARAM)(IsKo() ? L"아이콘" : L"Icon"));
        SendMessageW(ct, CB_ADDSTRING, 0, (LPARAM)(IsKo() ? L"아이콘 옆에 숫자" : L"Number beside icon"));
        SendMessageW(ct, CB_SETCURSEL, g_dlgSet.type, 0);
        CheckDlgButton(hDlg, IDC_TIP, g_dlgSet.tooltip ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_PCT, g_dlgSet.percent ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_ANIM, g_dlgSet.animation ? BST_CHECKED : BST_UNCHECKED);
        for (int i = 0; i < kSlots; ++i) {
            HWND cs = GetDlgItem(hDlg, IDC_SLOT0 + i);
            SendMessageW(cs, CB_ADDSTRING, 0, (LPARAM)(IsKo() ? L"(사용 안 함)" : L"(not used)"));
            int sel = 0;
            for (size_t j = 0; j < g_dlgDevices.size(); ++j) {
                const Device& d = g_dlgDevices[j];
                std::wstring t = d.name + (IsKo() ? (d.connected ? L"  [연결됨]" : L"  [연결 안 됨]") : (d.connected ? L"  [connected]" : L"  [not connected]"));
                SendMessageW(cs, CB_ADDSTRING, 0, (LPARAM)t.c_str());
                if (d.id == g_dlgSlots[i].id) sel = (int)j + 1;
            }
            g_dlgExtra[i] = 0;
            if (sel == 0 && !g_dlgSlots[i].id.empty()) {
                std::wstring t = (g_dlgSlots[i].name.empty() ? g_dlgSlots[i].id : g_dlgSlots[i].name) + (IsKo() ? L"  [목록에 없음]" : L"  [not in the list]");
                SendMessageW(cs, CB_ADDSTRING, 0, (LPARAM)t.c_str());
                sel = (int)g_dlgDevices.size() + 1;
                g_dlgExtra[i] = 1;
            }
            SendMessageW(cs, CB_SETCURSEL, sel, 0);
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            int t = (int)SendDlgItemMessageW(hDlg, IDC_TYPE, CB_GETCURSEL, 0, 0);
            g_dlgSet.type = t < 0 ? TYPE_NUMBER_BESIDE_ICON : t;
            g_dlgSet.tooltip = IsDlgButtonChecked(hDlg, IDC_TIP) != 0;
            g_dlgSet.percent = IsDlgButtonChecked(hDlg, IDC_PCT) != 0;
            g_dlgSet.animation = IsDlgButtonChecked(hDlg, IDC_ANIM) != 0;
            for (int i = 0; i < kSlots; ++i) {
                int sel = (int)SendDlgItemMessageW(hDlg, IDC_SLOT0 + i, CB_GETCURSEL, 0, 0);
                if (sel <= 0) { g_dlgSlots[i].id.clear(); g_dlgSlots[i].name.clear(); }
                else if ((size_t)sel <= g_dlgDevices.size()) { g_dlgSlots[i].id = g_dlgDevices[sel - 1].id; g_dlgSlots[i].name = g_dlgDevices[sel - 1].name; }
                // else: the "not in the list" entry: keep the old assignment
            }
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
    ScanNow();                                  // fresh list for the combo boxes
    g_dlgDevices = Snapshot();
    g_dlgSet = g_set;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < kSlots; ++i) g_dlgSlots[i] = g_slots[i];
    LeaveCriticalSection(&g_cs);

    const bool ko = IsKo();
    std::vector<BYTE> b;
    AddDW(b, DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    AddDW(b, 0);
    AddW(b, 2 + 3 + 1 + kSlots * 2 + 2);        // item count
    AddW(b, 0); AddW(b, 0); AddW(b, 290); AddW(b, 282);
    AddW(b, 0); AddW(b, 0);
    AddStr(b, ko ? L"블루투스 배터리 설정" : L"Bluetooth Battery Options");
    AddW(b, 9); AddStr(b, ko ? L"Malgun Gothic" : L"Segoe UI");

    const DWORD combo = CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP;
    const DWORD chk = BS_AUTOCHECKBOX | WS_TABSTOP;
    AddItem(b, SS_LEFT, 10, 12, 70, 10, 0xFFFF, 0x0082, ko ? L"표시 형식:" : L"Display type:");
    AddItem(b, combo, 85, 10, 195, 80, IDC_TYPE, 0x0085, L"");
    AddItem(b, chk, 10, 28, 270, 10, IDC_TIP, 0x0080, ko ? L"마우스 툴팁에 배터리 정보 표시" : L"Show battery information in the tooltip");
    AddItem(b, chk, 10, 41, 270, 10, IDC_PCT, 0x0080, ko ? L"퍼센트(%) 표시" : L"Show percent sign");
    AddItem(b, chk, 10, 54, 270, 10, IDC_ANIM, 0x0080, ko ? L"충전 애니메이션 표시" : L"Show charging animation");
    AddItem(b, SS_LEFT, 10, 72, 270, 10, 0xFFFF, 0x0082, ko ? L"블루투스 기기 슬롯 (기기를 선택하세요):" : L"Bluetooth device slots (select a device):");
    for (int i = 0; i < kSlots; ++i) {
        std::wstring lbl = (ko ? L"슬롯 " : L"Slot ") + std::to_wstring(i + 1) + L":";
        AddItem(b, SS_LEFT, 10, 88 + i * 16 + 2, 48, 10, 0xFFFF, 0x0082, lbl.c_str());
        AddItem(b, combo, 60, 88 + i * 16, 220, 100, (WORD)(IDC_SLOT0 + i), 0x0085, L"");
    }
    AddItem(b, BS_DEFPUSHBUTTON | WS_TABSTOP, 170, 262, 50, 14, IDOK, 0x0080, ko ? L"확인" : L"OK");
    AddItem(b, BS_PUSHBUTTON | WS_TABSTOP, 230, 262, 50, 14, IDCANCEL, 0x0080, ko ? L"취소" : L"Cancel");

    INT_PTR r = DialogBoxIndirectParamW(g_hInst, reinterpret_cast<LPCDLGTEMPLATEW>(b.data()), parent, DlgProc, 0);
    if (r != IDOK) return false;
    EnterCriticalSection(&g_cs);
    g_set = g_dlgSet;
    for (int i = 0; i < kSlots; ++i) g_slots[i] = g_dlgSlots[i];
    LeaveCriticalSection(&g_cs);
    SaveConfig();
    return true;
}

// ------------------------------------------------------------------ plugin
class WirelessBatteryPlugin : public ITMPlugin {
public:
    WirelessBatteryPlugin() {
        for (int i = 0; i < kSlots; ++i) m_items[i] = new BtItem(i);
    }
    IPluginItem* GetItem(int index) override { return (index >= 0 && index < kSlots) ? m_items[index] : nullptr; }

    const wchar_t* GetTooltipInfo() override {
        if (!g_set.tooltip) return L"";
        std::wstring s;
        for (int i = 0; i < kSlots; ++i) {
            SlotInfo si = GetSlotInfo(i);
            if (si.state == SS_EMPTY) continue;
            if (!s.empty()) s += L"\r\n";
            EnterCriticalSection(&g_cs);
            std::wstring nm = g_slots[i].name.empty() ? g_slots[i].id : g_slots[i].name;
            LeaveCriticalSection(&g_cs);
            s += nm + L": " + ValueText(si);
        }
        m_tooltip = s;
        return m_tooltip.c_str();
    }

    void DataRequired() override { EnsureThread(); }

    OptionReturn ShowOptionsDialog(void* hParent) override {
        try { return ShowOptions((HWND)hParent) ? OR_OPTION_CHANGED : OR_OPTION_UNCHANGED; }
        catch (...) { return OR_OPTION_UNCHANGED; }
    }

    const wchar_t* GetInfo(PluginInfoIndex index) override {
        switch (index) {
        case TMI_NAME: return IsKo() ? L"블루투스 배터리" : L"Bluetooth battery";
        case TMI_DESCRIPTION: return IsKo() ? L"Windows에 등록된 블루투스 기기의 배터리 잔량을 표시합니다 (슬롯 10개)."
                                            : L"Shows the battery level of Bluetooth devices registered in Windows (10 slots).";
        case TMI_AUTHOR: return L"zhongyang219 (Battery plugin), modified";
        case TMI_COPYRIGHT: return L"Copyright (C) by Zhong Yang 2025 (MIT)";
        case TMI_URL: return L"https://github.com/zhongyang219/TrafficMonitorPlugins";
        case TMI_VERSION: return L"1.00";
        default: return L"";
        }
    }

    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override {
        switch ((int)index) {
        case 900:       // the patched host will draw the number
            g_host_draws_text = (data != nullptr && wcscmp(data, L"1") == 0);
            break;
        case EI_CONFIG_DIR:
            if (data) { LoadConfig(std::wstring(data)); EnsureThread(); }
            break;
        default:
            break;
        }
    }

    void* GetPluginIcon() override { return GetIcon(IDI_BATTERY_DARK); }

private:
    BtItem* m_items[kSlots] = {};
    std::wstring m_tooltip;
};

WirelessBatteryPlugin* g_plugin = nullptr;

}  // namespace

// Cooperation with the patched TrafficMonitor: with the number beside the icon the host draws the number itself.
// Returns the width of the icon block (icon + gap), 0 if the host has nothing to do.
extern "C" __declspec(dllexport) int TMHostTextIconWidth() {
    if (g_set.type != TYPE_NUMBER_BESIDE_ICON) return 0;
    return DPI(16) + DPI(2);
}

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance() {
    if (!g_plugin) g_plugin = new WirelessBatteryPlugin();
    return g_plugin;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hInst;
        InitializeCriticalSection(&g_cs);
        DisableThreadLibraryCalls(hInst);
    }
    return TRUE;
}
