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
#include "BatteryShape.h"

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
// order = order of the glyphs in BtGlyphs.inc (generated by make_bt_glyphs.py)
enum DevType { DT_OTHER = 0, DT_KEYBOARD, DT_MOUSE, DT_HEADPHONES, DT_EARBUDS, DT_SPEAKER, DT_PHONE, DT_COMPUTER, DT_GAMEPAD, DT_WATCH, DT_TV, DT_PEN, DT_COUNT };
struct Device {
    std::wstring id;               // address, 12 upper case hex digits
    std::wstring name;
    bool connected = false;
    int battery = -1;              // 0 - 100, -1 = not reported by the device / Windows
    bool charging = false;         // Windows does not report this for most devices
    int type = DT_OTHER;
    bool from_classic = false;
    std::wstring category;         // category that Windows stores for the device (e.g. "Input.Keyboard"), for the icon
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

bool HasAny(const std::wstring& n, std::initializer_list<const wchar_t*> words) {
    for (auto w : words) if (n.find(w) != std::wstring::npos) return true;
    return false;
}

int GuessTypeFromName(const std::wstring& name) {
    std::wstring n = Lower(name);
    if (HasAny(n, { L"keyboard", L"키보드", L"kbd", L"keys" })) return DT_KEYBOARD;
    if (HasAny(n, { L"mouse", L"마우스", L"mx master", L"trackball", L"trackpad", L"mice" })) return DT_MOUSE;
    if (HasAny(n, { L"buds", L"airpods", L"earbud", L"earphone", L"이어폰", L"이어버즈", L"tws" })) return DT_EARBUDS;
    if (HasAny(n, { L"headset", L"headphone", L"헤드셋", L"헤드폰", L"wh-1000", L"wf-" })) return DT_HEADPHONES;
    if (HasAny(n, { L"speaker", L"스피커", L"soundbar", L"boom", L"charge ", L"flip", L"echo" })) return DT_SPEAKER;
    if (HasAny(n, { L"iphone", L"phone", L"galaxy s", L"galaxy a", L"galaxy z", L"pixel", L"폰", L"xperia" })) return DT_PHONE;
    if (HasAny(n, { L"watch", L"워치", L"band", L"밴드", L"fit" })) return DT_WATCH;
    if (HasAny(n, { L"controller", L"gamepad", L"xbox", L"dualsense", L"dualshock", L"컨트롤러", L"joy-con", L"joycon" })) return DT_GAMEPAD;
    if (HasAny(n, { L"laptop", L"notebook", L"macbook", L"pc", L"desktop", L"노트북" })) return DT_COMPUTER;
    if (HasAny(n, { L"tv", L"monitor", L"모니터", L"티비" })) return DT_TV;
    if (HasAny(n, { L"pen", L"stylus", L"펜" })) return DT_PEN;
    return DT_OTHER;
}

// The category that Windows itself uses for the icon of the device ("Input.Keyboard", "Audio.Headphone", "Phone.Smartphone" ...)
int TypeFromCategory(const std::wstring& category) {
    std::wstring c = Lower(category);
    if (c.empty()) return DT_OTHER;
    if (HasAny(c, { L"keyboard" })) return DT_KEYBOARD;
    if (HasAny(c, { L"mouse", L"pointing", L"trackpad", L"trackball" })) return DT_MOUSE;
    if (HasAny(c, { L"gaming", L"gamepad", L"joystick", L"controller" })) return DT_GAMEPAD;
    if (HasAny(c, { L"earbud", L"earphone" })) return DT_EARBUDS;
    if (HasAny(c, { L"headphone", L"headset", L"audio.head" })) return DT_HEADPHONES;
    if (HasAny(c, { L"speaker", L"soundbar", L"audio" })) return DT_SPEAKER;
    if (HasAny(c, { L"phone" })) return DT_PHONE;
    if (HasAny(c, { L"watch", L"wearable" })) return DT_WATCH;
    if (HasAny(c, { L"computer", L"laptop", L"desktop", L"tablet" })) return DT_COMPUTER;
    if (HasAny(c, { L"tv", L"display", L"monitor", L"video" })) return DT_TV;
    if (HasAny(c, { L"pen", L"stylus", L"digitizer" })) return DT_PEN;
    return DT_OTHER;
}

// Bluetooth class of device -> glyph (major / minor device class, see the Bluetooth assigned numbers)
int TypeFromClass(ULONG cod) {
    int major = (cod >> 8) & 0x1F;
    int minor = (cod >> 2) & 0x3F;
    switch (major) {
    case 1: return DT_COMPUTER;
    case 2: return DT_PHONE;
    case 4:
        switch (minor) {
        case 5: case 7: case 8: case 10: return DT_SPEAKER;
        case 13: case 14: case 15: return DT_TV;
        default: return DT_HEADPHONES;
        }
    case 5: {                                  // peripheral
        int sub = (cod >> 6) & 3;              // bits 7-6: 1 keyboard, 2 pointing device, 3 combo
        if (sub == 2) return DT_MOUSE;
        if (sub == 1 || sub == 3) return DT_KEYBOARD;
        int kind = (cod >> 2) & 0xF;           // 1 joystick, 2 gamepad, 7 digital pen
        if (kind == 1 || kind == 2) return DT_GAMEPAD;
        if (kind == 7) return DT_PEN;
        return DT_OTHER;
    }
    case 7: return DT_WATCH;                   // wearable
    default: return DT_OTHER;
    }
}

// A device node of the Bluetooth enumerators: BTHENUM\DEV_<address>\... (classic) or BTHLE\DEV_<address>\... (low energy)
struct Node {
    std::wstring id, name, category;
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
const DEVPROPKEY kKeyPrimaryCategory = { { 0x78C34FC8, 0x104A, 0x4ACA, { 0x9E, 0xA4, 0x52, 0x4D, 0x52, 0x99, 0x6E, 0x57 } }, 97 };
const DEVPROPKEY kKeyCategory = { { 0x78C34FC8, 0x104A, 0x4ACA, { 0x9E, 0xA4, 0x52, 0x4D, 0x52, 0x99, 0x6E, 0x57 } }, 90 };
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

        wchar_t cat[256] = {};
        if (SetupDiGetDevicePropertyW(set, &dd, &kKeyPrimaryCategory, &type, (PBYTE)cat, sizeof(cat) - 2, &need, 0) && cat[0])
            n.category = cat;
        else if (SetupDiGetDevicePropertyW(set, &dd, &kKeyCategory, &type, (PBYTE)cat, sizeof(cat) - 2, &need, 0) && cat[0])
            n.category = cat;          // string list: the first entry is enough

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
            {
                int by_name = GuessTypeFromName(d.name);
                if (d.type == DT_OTHER || (by_name == DT_EARBUDS && d.type == DT_HEADPHONES)) d.type = by_name;
            }
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
            if (d.category.empty()) d.category = n.category;
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
            d.category = n.category;
            list.push_back(d);
        } else {
            if (n.battery >= 0 && found->battery < 0) found->battery = n.battery;
            if (n.connected == 1) found->connected = true;
            if (found->category.empty()) found->category = n.category;
        }
    }
    // the category of Windows has the highest priority (it is what Windows uses for the icon), then the class of device, then the name
    for (auto& d : list) {
        int by_category = TypeFromCategory(d.category);
        if (by_category != DT_OTHER) d.type = by_category;
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
    case SS_EMPTY: return L"N/A";                                                   // slot not assigned
    case SS_DISCONNECTED:
    case SS_MISSING: return std::wstring(L"--") + (g_set.percent ? L"%" : L"");     // not connected / device not in the list
    case SS_NODATA: return g_set.percent ? L"%" : L"N/A";    // connected, but no battery information: only the unit,
                                                             // the host fills the digits with the fill character ("ˍˍˍ%")
    }
    return L"N/A";
}

// ------------------------------------------------------------------ drawing
ULONG_PTR g_gdiplusToken = 0;
void EnsureGdiplus() {
    if (g_gdiplusToken != 0) return;
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &in, nullptr);
}

// Colors (the same as the Battery plugin)
const COLORREF kRed = RGB(206, 23, 0);

// Icon for the plugin manager (square, 16 px at 96 DPI)
enum { IDI_PLUGIN = 104 };
HICON g_plugin_icon = nullptr;
HICON GetPluginIconHandle() {
    if (!g_plugin_icon)
        g_plugin_icon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_PLUGIN), IMAGE_ICON, DPI(16), DPI(16), 0);
    return g_plugin_icon;
}

// ---- glyphs: a list of drawing operations in a unit box (generated by make_bt_glyphs.py)
struct GOp { char t; float v[6]; };
struct GlyphDef { const GOp* ops; int count; };
#include "BtGlyphs.inc"

void RoundRectPath(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    float d = r * 2;
    if (d > w) d = w;
    if (d > h) d = h;
    if (d < 0.01f) { path.AddRectangle(Gdiplus::RectF(x, y, w, h)); return; }
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

// size = edge length of the box in pixels
void DrawGlyph(Gdiplus::Graphics& g, int type, float x, float y, float size, COLORREF color) {
    if (type < 0 || type >= DT_COUNT) type = DT_OTHER;
    Gdiplus::Color col(255, GetRValue(color), GetGValue(color), GetBValue(color));
    Gdiplus::SolidBrush brush(col);
    float pw = size * 0.13f;
    if (pw < 0.8f) pw = 0.8f;
    Gdiplus::Pen pen(col, pw);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    const float half = pw / 2;     // outlines are drawn inside of their box
    const GlyphDef& gd = kGlyphs[type];
    for (int i = 0; i < gd.count; ++i) {
        const GOp& o = gd.ops[i];
        const float* v = o.v;
        switch (o.t) {
        case 'R': {
            Gdiplus::GraphicsPath p;
            RoundRectPath(p, x + v[0] * size, y + v[1] * size, v[2] * size, v[3] * size, v[4] * size);
            g.FillPath(&brush, &p);
            break;
        }
        case 'O': {
            Gdiplus::GraphicsPath p;
            RoundRectPath(p, x + v[0] * size + half, y + v[1] * size + half, v[2] * size - pw, v[3] * size - pw, v[4] * size);
            g.DrawPath(&pen, &p);
            break;
        }
        case 'L': g.DrawLine(&pen, x + v[0] * size, y + v[1] * size, x + v[2] * size, y + v[3] * size); break;
        case 'A': g.DrawArc(&pen, x + v[0] * size + half, y + v[1] * size + half, v[2] * size - pw, v[3] * size - pw, v[4], v[5]); break;
        case 'E': g.FillEllipse(&brush, x + v[0] * size, y + v[1] * size, v[2] * size, v[3] * size); break;
        case 'C': g.DrawEllipse(&pen, x + v[0] * size + half, y + v[1] * size + half, v[2] * size - pw, v[3] * size - pw); break;
        case 'T': {
            Gdiplus::PointF pts[3] = { Gdiplus::PointF(x + v[0] * size, y + v[1] * size), Gdiplus::PointF(x + v[2] * size, y + v[3] * size),
                                       Gdiplus::PointF(x + v[4] * size, y + v[5] * size) };
            g.FillPolygon(&brush, pts, 3);
            break;
        }
        }
    }
}

void DrawBluetoothMark(Gdiplus::Graphics& g, float x, float y, float w, float h, COLORREF color) {
    Gdiplus::Color col(255, GetRValue(color), GetGValue(color), GetBValue(color));
    float pw = h * 0.13f;
    if (pw < 0.8f) pw = 0.8f;
    Gdiplus::Pen pen(col, pw);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    Gdiplus::PointF pts[6];
    for (int i = 0; i < 6; ++i) pts[i] = Gdiplus::PointF(x + w * kBtMark[i][0], y + h * kBtMark[i][1]);
    g.DrawLines(&pen, pts, 6);
}

// Equal space left of the first glyph, between the glyphs and right of the last glyph.
// (ix, iy) = top left of the battery; the glyphs are as high as 62 % of the body.
void DrawGlyphRow(Gdiplus::Graphics& g, int ix, int iy, const BatteryShape& s, COLORREF color, int dev_type, bool bolt) {
    const float gh = s.body_h * 0.62f;
    const float bolt_w = gh * 0.56f, bt_w = gh * 0.667f, dev_w = gh;
    const float inner_w = (float)s.inner_w();
    int n = bolt ? 3 : 2;
    float sum = bt_w + dev_w + (bolt ? bolt_w : 0.0f);
    float gap = (inner_w - sum) / (n + 1);
    if (gap < 0) gap = 0;
    float x = ix + s.border + gap;
    const float cy = iy + s.body_h / 2.0f;
    if (bolt) {
        static const float kBolt[7][2] = { { 0.55f, 0.0f }, { 0.05f, 0.56f }, { 0.40f, 0.56f }, { 0.30f, 1.0f }, { 0.95f, 0.38f }, { 0.58f, 0.38f }, { 0.80f, 0.0f } };
        Gdiplus::PointF pts[7];
        for (int i = 0; i < 7; ++i) pts[i] = Gdiplus::PointF(x + bolt_w * kBolt[i][0], cy - gh / 2 + gh * kBolt[i][1]);
        Gdiplus::SolidBrush brush(BatteryColor(color));
        g.FillPolygon(&brush, pts, 7);
        x += bolt_w + gap;
    }
    DrawBluetoothMark(g, x, cy - gh / 2, bt_w, gh, color);
    x += bt_w + gap;
    DrawGlyph(g, dev_type, x, cy - gh / 2, dev_w, color);
}

int g_last_icon_w = 0;       // width of the battery that was used last (for TMHostTextIconWidth)

int HostBoxHeight(HDC dc) { return BatteryHostEmHeight(dc, DPI(8), DPI(80), DPI(14)); }

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
        BatteryShape shape = MakeBatteryShape(HostBoxHeight(dc));
        g_last_icon_w = shape.total_w();
        if (g_set.type == TYPE_ICON) return shape.total_w() + DPI(4);
        return shape.total_w() + DPI(4) + sz.cx;
    }

    void DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode) override {
        HDC dc = (HDC)hDC;
        EnsureGdiplus();
        SlotInfo si = GetSlotInfo(m_slot);
        std::wstring text = ValueText(si);

        // as high as the letters (em height of the host font = height of the boxes of the keyboard indicator),
        // never higher than the row, exactly centered
        int box_h = HostBoxHeight(dc);
        if (box_h > h) box_h = h;
        if (((h - box_h) & 1) != 0) box_h += (box_h < h ? 1 : -1);
        BatteryShape shape = MakeBatteryShape(box_h);
        g_last_icon_w = shape.total_w();
        const int icon_w = shape.total_w();

        bool host_draws_text = g_host_draws_text && g_set.type == TYPE_NUMBER_BESIDE_ICON;
        g_host_draws_text = false;
        int content_w = icon_w;
        if (!host_draws_text && g_set.type == TYPE_NUMBER_BESIDE_ICON) {
            SIZE sz = {};
            GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
            content_w += DPI(2) + sz.cx;
        }
        int block_offset = host_draws_text ? 0 : (w - content_w) / 2;
        if (block_offset < 0) block_offset = 0;
        const int ix = x + block_offset;
        const int iy = y + (h - shape.body_h) / 2;

        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const COLORREF frame_color = dark_mode ? RGB(255, 255, 255) : RGB(64, 64, 64);
        const COLORREF off_color = dark_mode ? RGB(145, 145, 145) : RGB(140, 140, 140);   // = keyboard indicator "off"

        // 1) fill - drawn first (the frame is drawn over it)
        //    100 % yellow | charging green (only if Windows reports it) | otherwise red | no information: gray
        const bool charging = si.state == SS_OK && si.dev.charging;
        COLORREF glyph_color = frame_color;
        bool draw_glyphs = true;
        switch (si.state) {
        case SS_OK: {
            const int pct = si.dev.battery;
            if (pct >= 100) {
                DrawBatteryFill(g, ix, iy, shape, RGB(241, 197, 0), 100);
                glyph_color = RGB(50, 50, 50);
            } else if (pct > 0) {
                double percent = pct;
                if (g_set.animation && charging) percent = pct + (GetTickCount() % 2000) / 2000.0 * (100 - pct);
                DrawBatteryFill(g, ix, iy, shape, charging ? RGB(128, 194, 105) : kRed, percent);
            }
            break;
        }
        case SS_NODATA:                                            // connected, but no battery information:
            DrawBatteryFill(g, ix, iy, shape, RGB(128, 128, 128), 100);     // gray background, white icons
            glyph_color = RGB(255, 255, 255);
            break;
        case SS_DISCONNECTED: glyph_color = off_color; break;
        default: draw_glyphs = false; break;                       // slot empty / device missing: empty battery
        }

        // 2) frame
        DrawBatteryFrame(g, ix, iy, shape, frame_color);

        // 3) glyphs: [bolt if Windows reports "charging"] Bluetooth mark, device type
        if (draw_glyphs)
            DrawGlyphRow(g, ix, iy, shape, glyph_color, si.dev.type, charging);
        g.Flush(Gdiplus::FlushIntentionSync);

        if (g_set.type == TYPE_NUMBER_BESIDE_ICON && !host_draws_text) {
            RECT rc = { ix + icon_w + DPI(2), y, x + w, y + h };
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
                static const wchar_t* kTypeKo[] = { L"기타", L"키보드", L"마우스", L"헤드폰", L"이어버즈", L"스피커", L"휴대폰", L"컴퓨터", L"게임패드", L"시계", L"TV", L"펜" };
                static const wchar_t* kTypeEn[] = { L"other", L"keyboard", L"mouse", L"headphones", L"earbuds", L"speaker", L"phone", L"computer", L"gamepad", L"watch", L"TV", L"pen" };
                std::wstring kind = std::wstring(L" \u00B7 ") + (IsKo() ? kTypeKo[d.type] : kTypeEn[d.type]);
                if (d.type == DT_OTHER && !d.category.empty()) kind += L" (" + d.category + L")";
                std::wstring t = d.name + kind + (IsKo() ? (d.connected ? L"  [연결됨]" : L"  [연결 안 됨]") : (d.connected ? L"  [connected]" : L"  [not connected]"));
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
        case TMI_VERSION: return L"1.21";
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

    void* GetPluginIcon() override { return GetPluginIconHandle(); }

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
    return (g_last_icon_w > 0 ? g_last_icon_w : DPI(32)) + DPI(2);
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
