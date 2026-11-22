// KoLocalize.h - Korean texts for the settings windows of third party plugins (MIT, may be modified freely).
// Only used when Windows uses Korean. Dialog caption, static texts and buttons whose English text is in the table
// are replaced when the dialog is created; everything else is left as it is.
#pragma once
#include <map>
#include <string>

inline bool KoUi()
{
    static const bool ko = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN;
    return ko;
}

inline const std::map<std::wstring, std::wstring>& KoTable()
{
    static const std::map<std::wstring, std::wstring> table = {
        // common
        { L"OK", L"확인" }, { L"Cancel", L"취소" }, { L"Refresh", L"새로 고침" },
        // IpAddress
        { L"IP Address Settings", L"IP 주소 설정" },
        { L"Select the connection to show the local IPv4 address:", L"로컬 IPv4 주소를 표시할 네트워크 연결을 선택하세요:" },
        { L"Refresh connection list", L"연결 목록 새로 고침" },
        // PowerMon
        { L"PowerMon Settings", L"PowerMon 설정" }, { L"Revert", L"되돌리기" },
        { L"Watts", L"와트 (W)" }, { L"Watts * Hour", L"와트시 (Wh)" }, { L"Voltage", L"전압 (V)" },
        { L"Unit String", L"단위 문자열" }, { L"Hour", L"시간" }, { L"Minute", L"분" },
        { L"Unit Length", L"단위 길이" }, { L"Spacing of unit and value", L"단위와 값 사이 간격" },
        { L"Meter Display Max decimal places", L"미터 표시 최대 소수 자릿수" },
        { L"Repo URL", L"저장소 주소" }, { L"Debug Mode", L"디버그 모드" }, { L"BTR Driver", L"배터리 드라이버" },
        { L"Enable CPU Monitor", L"CPU 모니터 사용" }, { L"Enable GPU Monitor", L"GPU 모니터 사용" },
    };
    return table;
}

inline void KoSetText(HWND h)
{
    wchar_t buf[512] = {};
    if (GetWindowTextW(h, buf, 512) <= 0)
        return;
    auto it = KoTable().find(buf);
    if (it != KoTable().end())
        SetWindowTextW(h, it->second.c_str());
}

inline BOOL CALLBACK KoEnumChild(HWND h, LPARAM)
{
    wchar_t cls[32] = {};
    GetClassNameW(h, cls, 32);
    // texts of Static controls and Buttons only (never the content of edit boxes or combo boxes)
    if (_wcsicmp(cls, L"Static") == 0 || _wcsicmp(cls, L"Button") == 0)
        KoSetText(h);
    return TRUE;
}

inline void KoLocalizeDialog(HWND dlg)
{
    if (!KoUi() || dlg == nullptr)
        return;
    KoSetText(dlg);                         // caption
    EnumChildWindows(dlg, KoEnumChild, 0);  // all controls
}
