// TrafficMonitor plugin: GPU memory usage (auto-detect, supports integrated GPUs with shared memory)
// Data source : Windows performance counters "\GPU Adapter Memory(*)\Dedicated Usage / Shared Usage"
// Capacity    : DXGI adapter description (DedicatedVideoMemory / SharedSystemMemory)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cwchar>
#include "PluginInterface.h"

namespace {

HINSTANCE g_hInst = nullptr;

// ---------------------------------------------------------------- localisation
struct Strings {
    const wchar_t* itemName;
    const wchar_t* description;
    const wchar_t* dlgTitle;
    const wchar_t* lblFormat;
    const wchar_t* lblCalc;
    const wchar_t* lblGpu;
    const wchar_t* fmtPercent;
    const wchar_t* fmtUsage;
    const wchar_t* calcAuto;
    const wchar_t* calcDedicated;
    const wchar_t* calcShared;
    const wchar_t* gpuAuto;
    const wchar_t* ok;
    const wchar_t* cancel;
    const wchar_t* ttGpu;
    const wchar_t* ttDedicated;
    const wchar_t* ttShared;
    const wchar_t* ttTotal;
    const wchar_t* ttNoGpu;
    const wchar_t* cmdSettings;
    const wchar_t* lblLabel;
};

const Strings kKo = {
    L"GPU 메모리 사용률", L"그래픽카드(내장/외장) 메모리 사용률을 자동으로 감지하여 표시합니다.",
    L"GPU 메모리 설정", L"표시 형식:", L"계산 방식:", L"대상 GPU:",
    L"사용률 (42%)", L"사용량 (2.1/8.0GB)",
    L"자동", L"전용 메모리만", L"전용 + 공유 메모리",
    L"자동 선택", L"확인", L"취소",
    L"GPU", L"전용", L"공유", L"전체", L"GPU를 찾을 수 없음", L"GPU 메모리 설정...", L"라벨:"};
const Strings kEn = {
    L"GPU Memory Usage", L"Shows GPU memory usage (auto-detects integrated / discrete GPU).",
    L"GPU Memory Options", L"Display format:", L"Calculation:", L"Target GPU:",
    L"Percentage (42%)", L"Used / Total (2.1/8.0GB)",
    L"Auto", L"Dedicated memory only", L"Dedicated + Shared memory",
    L"Auto select", L"OK", L"Cancel",
    L"GPU", L"Dedicated", L"Shared", L"Total", L"No GPU found", L"GPU Memory options...", L"Label:"};

const Strings& Tr() {
    static const bool ko = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN;
    return ko ? kKo : kEn;
}

// ---------------------------------------------------------------- config
struct Config {
    int format = 0;           // 0 = percent, 1 = used/total
    int calc = 0;             // 0 = auto, 1 = dedicated only, 2 = dedicated + shared
    std::wstring gpu = L"auto";
    std::wstring label = L"VRAM:";
};

Config g_cfg;
std::wstring g_cfgDir;
bool g_cfgLoaded = false;

std::wstring ModuleDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_hInst, path, MAX_PATH);
    std::wstring p(path);
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"" : p.substr(0, pos + 1);
}

std::wstring IniPath() {
    std::wstring dir = g_cfgDir.empty() ? ModuleDir() : g_cfgDir;
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    return dir + L"GpuMemory.ini";
}

void LoadConfig() {
    std::wstring ini = IniPath();
    // legacy section/keys (v1.0.0) are used as the default so old settings are kept
    int oldFormat = (int)GetPrivateProfileIntW(L"GpuMemory", L"Format", 0, ini.c_str());
    int oldCalc = (int)GetPrivateProfileIntW(L"GpuMemory", L"Calc", 0, ini.c_str());
    wchar_t oldGpu[64] = {};
    GetPrivateProfileStringW(L"GpuMemory", L"Gpu", L"auto", oldGpu, 64, ini.c_str());

    g_cfg.format = (int)GetPrivateProfileIntW(L"config", L"display_format", oldFormat, ini.c_str());
    g_cfg.calc = (int)GetPrivateProfileIntW(L"config", L"calc_mode", oldCalc, ini.c_str());
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(L"config", L"gpu", oldGpu, buf, 64, ini.c_str());
    g_cfg.gpu = buf;
    wchar_t lbl[32] = {};
    GetPrivateProfileStringW(L"config", L"label", L"VRAM:", lbl, 32, ini.c_str());
    g_cfg.label = lbl;
    if (g_cfg.label.empty()) g_cfg.label = L"VRAM:";
    if (g_cfg.format < 0 || g_cfg.format > 1) g_cfg.format = 0;
    if (g_cfg.calc < 0 || g_cfg.calc > 2) g_cfg.calc = 0;
    if (g_cfg.gpu.empty()) g_cfg.gpu = L"auto";
    g_cfgLoaded = true;
}

void SaveConfig() {
    std::wstring ini = IniPath();
    wchar_t num[16];
    swprintf(num, 16, L"%d", g_cfg.format);
    WritePrivateProfileStringW(L"config", L"display_format", num, ini.c_str());
    swprintf(num, 16, L"%d", g_cfg.calc);
    WritePrivateProfileStringW(L"config", L"calc_mode", num, ini.c_str());
    WritePrivateProfileStringW(L"config", L"gpu", g_cfg.gpu.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"config", L"label", g_cfg.label.c_str(), ini.c_str());
    // remove the legacy section
    WritePrivateProfileStringW(L"GpuMemory", nullptr, nullptr, ini.c_str());
}

// ---------------------------------------------------------------- GPU adapters (DXGI)
struct AdapterInfo {
    LUID luid{};
    std::wstring name;
    unsigned long long dedicated = 0;
    unsigned long long shared = 0;
};

std::vector<AdapterInfo> g_adapters;
bool g_adaptersLoaded = false;

std::wstring LuidToId(const LUID& l) {
    wchar_t buf[40];
    swprintf(buf, 40, L"%08lX-%08lX", (unsigned long)l.HighPart, (unsigned long)l.LowPart);
    return buf;
}

void EnumerateAdapters() {
    g_adapters.clear();
    g_adaptersLoaded = true;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) || !factory) return;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* ad = nullptr;
        if (factory->EnumAdapters1(i, &ad) == DXGI_ERROR_NOT_FOUND) break;
        if (!ad) continue;
        DXGI_ADAPTER_DESC1 d = {};
        if (SUCCEEDED(ad->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            && d.VendorId != 0x1414 /* Microsoft Basic Render Driver */) {
            AdapterInfo a;
            a.luid = d.AdapterLuid;
            a.name = d.Description;
            a.dedicated = (unsigned long long)d.DedicatedVideoMemory;
            a.shared = (unsigned long long)d.SharedSystemMemory;
            g_adapters.push_back(a);
        }
        ad->Release();
    }
    factory->Release();
}

// ---------------------------------------------------------------- PDH counters
PDH_HQUERY g_query = nullptr;
PDH_HCOUNTER g_cDedicated = nullptr;
PDH_HCOUNTER g_cShared = nullptr;
bool g_pdhFailed = false;

bool InitPdh() {
    if (g_query) return true;
    if (g_pdhFailed) return false;
    if (PdhOpenQueryW(nullptr, 0, &g_query) != ERROR_SUCCESS) { g_query = nullptr; g_pdhFailed = true; return false; }
    if (PdhAddEnglishCounterW(g_query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &g_cDedicated) != ERROR_SUCCESS ||
        PdhAddEnglishCounterW(g_query, L"\\GPU Adapter Memory(*)\\Shared Usage", 0, &g_cShared) != ERROR_SUCCESS) {
        PdhCloseQuery(g_query);
        g_query = nullptr;
        g_cDedicated = g_cShared = nullptr;
        g_pdhFailed = true;
        return false;
    }
    PdhCollectQueryData(g_query);  // prime
    return true;
}

typedef std::vector<std::pair<std::wstring, long long> > InstanceValues;

bool ReadArray(PDH_HCOUNTER h, InstanceValues& out) {
    out.clear();
    DWORD size = 0, count = 0;
    PDH_STATUS s = PdhGetFormattedCounterArrayW(h, PDH_FMT_LARGE, &size, &count, nullptr);
    if (s != (PDH_STATUS)PDH_MORE_DATA || size == 0) return false;
    std::vector<BYTE> buf(size);
    PDH_FMT_COUNTERVALUE_ITEM_W* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    s = PdhGetFormattedCounterArrayW(h, PDH_FMT_LARGE, &size, &count, items);
    if (s != ERROR_SUCCESS) return false;
    for (DWORD i = 0; i < count; ++i) {
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        if (!items[i].szName) continue;
        out.push_back(std::make_pair(std::wstring(items[i].szName), (long long)items[i].FmtValue.largeValue));
    }
    return true;
}

// Sum all instances belonging to the adapter, e.g. "luid_0x00000000_0x0000d4f2_phys_0"
unsigned long long SumForLuid(const InstanceValues& v, const LUID& l) {
    wchar_t prefix[64];
    swprintf(prefix, 64, L"luid_0x%08lx_0x%08lx_", (unsigned long)l.HighPart, (unsigned long)l.LowPart);
    size_t n = wcslen(prefix);
    unsigned long long sum = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        if (_wcsnicmp(v[i].first.c_str(), prefix, n) == 0 && v[i].second > 0) sum += (unsigned long long)v[i].second;
    }
    return sum;
}

// ---------------------------------------------------------------- selection & calculation
const unsigned long long kGiB = 1024ull * 1024ull * 1024ull;

const AdapterInfo* SelectAdapter(bool& isDiscrete) {
    isDiscrete = false;
    if (g_adapters.empty()) return nullptr;
    const AdapterInfo* chosen = nullptr;
    if (g_cfg.gpu != L"auto") {
        for (size_t i = 0; i < g_adapters.size(); ++i)
            if (LuidToId(g_adapters[i].luid) == g_cfg.gpu) { chosen = &g_adapters[i]; break; }
    }
    if (!chosen) {
        chosen = &g_adapters[0];
        for (size_t i = 1; i < g_adapters.size(); ++i)
            if (g_adapters[i].dedicated > chosen->dedicated) chosen = &g_adapters[i];
    }
    // Heuristic: discrete GPU = large dedicated VRAM, or clearly bigger than every other adapter
    unsigned long long other = 0;
    for (size_t i = 0; i < g_adapters.size(); ++i)
        if (&g_adapters[i] != chosen && g_adapters[i].dedicated > other) other = g_adapters[i].dedicated;
    isDiscrete = chosen->dedicated >= 4 * kGiB || (g_adapters.size() > 1 && chosen->dedicated >= 2 * other && chosen->dedicated >= kGiB);
    return chosen;
}

// ---------------------------------------------------------------- published state
std::wstring g_valueText = L"--";
std::wstring g_tooltip;
float g_fraction = 0.0f;
unsigned g_tick = 0;

std::wstring FmtGB(unsigned long long bytes) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.1f", (double)bytes / (double)kGiB);
    return buf;
}

void SetUnavailable() {
    g_valueText = L"--";
    g_fraction = 0.0f;
    g_tooltip = Tr().ttNoGpu;
}

void Update() {
    ++g_tick;
    if (!g_cfgLoaded) LoadConfig();
    if (!g_adaptersLoaded || (g_adapters.empty() && g_tick % 10 == 0)) EnumerateAdapters();

    bool discrete = false;
    const AdapterInfo* ad = SelectAdapter(discrete);
    if (!ad || !InitPdh()) { SetUnavailable(); return; }

    if (PdhCollectQueryData(g_query) != ERROR_SUCCESS) { SetUnavailable(); return; }
    InstanceValues dv, sv;
    bool okD = ReadArray(g_cDedicated, dv);
    bool okS = ReadArray(g_cShared, sv);
    if (!okD && !okS) { SetUnavailable(); return; }

    unsigned long long usedDed = SumForLuid(dv, ad->luid);
    unsigned long long usedShr = SumForLuid(sv, ad->luid);

    bool includeShared = (g_cfg.calc == 2) || (g_cfg.calc == 0 && !discrete);
    unsigned long long used = usedDed + (includeShared ? usedShr : 0);
    unsigned long long total = ad->dedicated + (includeShared ? ad->shared : 0);
    if (total == 0) { SetUnavailable(); return; }
    if (used > total) used = total;

    double frac = (double)used / (double)total;
    g_fraction = (float)frac;

    wchar_t buf[64];
    if (g_cfg.format == 1) {
        g_valueText = FmtGB(used) + L"/" + FmtGB(total) + L"GB";
    } else {
        swprintf(buf, 64, L"%.0f%%", frac * 100.0);
        g_valueText = buf;
    }

    const Strings& t = Tr();
    g_tooltip = std::wstring(t.ttGpu) + L": " + ad->name + L"\n" +
                t.ttDedicated + L": " + FmtGB(usedDed) + L" / " + FmtGB(ad->dedicated) + L" GB\n" +
                t.ttShared + L": " + FmtGB(usedShr) + L" / " + FmtGB(ad->shared) + L" GB\n" +
                t.ttTotal + L": " + FmtGB(used) + L" / " + FmtGB(total) + L" GB (" +
                (includeShared ? L"Dedicated + Shared" : L"Dedicated") + L")";
}

// ---------------------------------------------------------------- options dialog
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

enum { IDC_FORMAT = 101, IDC_CALC = 102, IDC_GPU = 103, IDC_LABEL = 104 };

Config g_dlgCfg;

INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG: {
        const Strings& t = Tr();
        HWND cf = GetDlgItem(hDlg, IDC_FORMAT);
        SendMessageW(cf, CB_ADDSTRING, 0, (LPARAM)t.fmtPercent);
        SendMessageW(cf, CB_ADDSTRING, 0, (LPARAM)t.fmtUsage);
        SendMessageW(cf, CB_SETCURSEL, g_dlgCfg.format, 0);

        HWND cc = GetDlgItem(hDlg, IDC_CALC);
        SendMessageW(cc, CB_ADDSTRING, 0, (LPARAM)t.calcAuto);
        SendMessageW(cc, CB_ADDSTRING, 0, (LPARAM)t.calcDedicated);
        SendMessageW(cc, CB_ADDSTRING, 0, (LPARAM)t.calcShared);
        SendMessageW(cc, CB_SETCURSEL, g_dlgCfg.calc, 0);

        HWND cg = GetDlgItem(hDlg, IDC_GPU);
        SendMessageW(cg, CB_ADDSTRING, 0, (LPARAM)t.gpuAuto);
        int sel = 0;
        for (size_t i = 0; i < g_adapters.size(); ++i) {
            SendMessageW(cg, CB_ADDSTRING, 0, (LPARAM)g_adapters[i].name.c_str());
            if (LuidToId(g_adapters[i].luid) == g_dlgCfg.gpu) sel = (int)i + 1;
        }
        SendMessageW(cg, CB_SETCURSEL, sel, 0);

        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            int f = (int)SendDlgItemMessageW(hDlg, IDC_FORMAT, CB_GETCURSEL, 0, 0);
            int c = (int)SendDlgItemMessageW(hDlg, IDC_CALC, CB_GETCURSEL, 0, 0);
            int g = (int)SendDlgItemMessageW(hDlg, IDC_GPU, CB_GETCURSEL, 0, 0);
            g_dlgCfg.format = f < 0 ? 0 : f;
            g_dlgCfg.calc = c < 0 ? 0 : c;
            if (g <= 0 || (size_t)g > g_adapters.size()) g_dlgCfg.gpu = L"auto";
            else g_dlgCfg.gpu = LuidToId(g_adapters[g - 1].luid);
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
    if (!g_cfgLoaded) LoadConfig();
    EnumerateAdapters();
    g_dlgCfg = g_cfg;
    const Strings& t = Tr();

    std::vector<BYTE> b;
    AddDW(b, DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    AddDW(b, 0);
    AddW(b, 8);  // item count
    AddW(b, 0); AddW(b, 0); AddW(b, 240); AddW(b, 112);
    AddW(b, 0); AddW(b, 0);
    AddStr(b, t.dlgTitle);
    AddW(b, 9); AddStr(b, L"Segoe UI");

    const DWORD combo = CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP;
    AddItem(b, SS_LEFT, 10, 14, 60, 10, 0xFFFF, 0x0082, t.lblFormat);
    AddItem(b, combo, 75, 12, 155, 80, IDC_FORMAT, 0x0085, L"");
    AddItem(b, SS_LEFT, 10, 38, 60, 10, 0xFFFF, 0x0082, t.lblCalc);
    AddItem(b, combo, 75, 36, 155, 80, IDC_CALC, 0x0085, L"");
    AddItem(b, SS_LEFT, 10, 62, 60, 10, 0xFFFF, 0x0082, t.lblGpu);
    AddItem(b, combo, 75, 60, 155, 80, IDC_GPU, 0x0085, L"");
    AddItem(b, BS_DEFPUSHBUTTON | WS_TABSTOP, 120, 90, 50, 14, IDOK, 0x0080, t.ok);
    AddItem(b, BS_PUSHBUTTON | WS_TABSTOP, 180, 90, 50, 14, IDCANCEL, 0x0080, t.cancel);

    INT_PTR r = DialogBoxIndirectParamW(g_hInst, reinterpret_cast<LPCDLGTEMPLATEW>(b.data()), parent, DlgProc, 0);
    if (r != IDOK) return false;
    bool changed = g_dlgCfg.format != g_cfg.format || g_dlgCfg.calc != g_cfg.calc || g_dlgCfg.gpu != g_cfg.gpu;
    g_cfg = g_dlgCfg;
    SaveConfig();
    Update();
    return changed;
}

// ---------------------------------------------------------------- plugin objects
class GpuMemItem : public IPluginItem {
public:
    const wchar_t* GetItemName() const override { return Tr().itemName; }
    const wchar_t* GetItemId() const override { return L"GpuMemoryUsage_A1"; }
    const wchar_t* GetItemLableText() const override { return g_cfg.label.c_str(); }
    const wchar_t* GetItemValueText() const override { return g_valueText.c_str(); }
    const wchar_t* GetItemValueSampleText() const override { return g_cfg.format == 1 ? L"00.0/00.0GB" : L"100%"; }
    int IsDrawResourceUsageGraph() const override { return 1; }
    float GetResourceUsageGraphValue() const override { return g_fraction; }
};

class GpuMemPlugin : public ITMPlugin {
public:
    IPluginItem* GetItem(int index) override { return index == 0 ? &m_item : nullptr; }

    void DataRequired() override {
        try { Update(); } catch (...) { SetUnavailable(); }
    }

    OptionReturn ShowOptionsDialog(void* hParent) override {
        try {
            return ShowOptions((HWND)hParent) ? OR_OPTION_CHANGED : OR_OPTION_UNCHANGED;
        } catch (...) {
            return OR_OPTION_UNCHANGED;
        }
    }

    const wchar_t* GetInfo(PluginInfoIndex index) override {
        switch (index) {
        case TMI_NAME: return Tr().itemName;
        case TMI_DESCRIPTION: return Tr().description;
        case TMI_AUTHOR: return L"Claude";
        case TMI_COPYRIGHT: return L"Copyright (C) 2026";
        case TMI_VERSION: return L"1.0.1";
        case TMI_URL: return L"";
        default: return L"";
        }
    }

    const wchar_t* GetTooltipInfo() override { return g_tooltip.c_str(); }

    // Right-click menu entry -> opens the same options dialog
    int GetCommandCount() override { return 1; }
    const wchar_t* GetCommandName(int index) override { return index == 0 ? Tr().cmdSettings : nullptr; }
    void OnPluginCommand(int index, void* hWnd, void*) override {
        if (index != 0) return;
        try { ShowOptions((HWND)hWnd); } catch (...) {}
    }

    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override {
        if (index == EI_CONFIG_DIR && data && *data) {
            g_cfgDir = data;
            LoadConfig();
        }
    }

private:
    GpuMemItem m_item;
};

GpuMemPlugin g_plugin;

}  // namespace

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance() { return &g_plugin; }

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hInst;
        DisableThreadLibraryCalls(hInst);
    }
    return TRUE;
}
