# Korean texts for the Battery, IpAddress, PowerMon and Ping plugins (+ the 10 second refresh of the IpAddress plugin).
# usage:  python patch_third_party_plugins.py <battery|ipaddress|powermon|ping> <project folder> <folder with KoLocalize.h>
# All patched C++ files are saved as UTF-8 WITH BOM (MSVC would otherwise read the Korean texts as ANSI).
import sys, shutil, os

def _load(path):
    raw = open(path, 'rb').read()
    text = raw.decode('utf-8-sig')
    crlf = '\r\n' in text
    return text.replace('\r\n', '\n'), crlf

def _save(path, text, crlf):
    if crlf:
        text = text.replace('\n', '\r\n')
    open(path, 'wb').write(b'\xef\xbb\xbf' + text.encode('utf-8'))

def patch(path, edits):
    text, crlf = _load(path)
    for old, new in edits:
        n = text.count(old)
        if n != 1:
            raise SystemExit('PATCH FAILED: %r found %d times in %s' % (old[:70], n, path))
        text = text.replace(old, new)
    _save(path, text, crlf)

def patch_soft(path, edits):
    """Cosmetic texts: replaces every occurrence, never fails."""
    text, crlf = _load(path)
    for old, new in edits:
        text = text.replace(old, new)
    _save(path, text, crlf)

def copy_headers(kodir, proj, names):
    for n in names:
        shutil.copy(os.path.join(kodir, n), os.path.join(proj, n))

which, proj, kodir = sys.argv[1], sys.argv[2], sys.argv[3]
if os.path.isfile(kodir):                 # old call style: path of KoLocalize.h
    kodir = os.path.dirname(kodir)
P = lambda f: os.path.join(proj, f)

if which == 'battery':
    copy_headers(kodir, proj, ['KoLocalize.h'])
    patch(P('Battery.cpp'), [
        ('#include "pch.h"\n', '#include "pch.h"\n#include "KoLocalize.h"\n'),
        ('        return g_data.StringRes(IDS_PLUGIN_NAME).GetString();\n', '        return KoUi() ? L"배터리 플러그인" : g_data.StringRes(IDS_PLUGIN_NAME).GetString();\n'),
        ('        return g_data.StringRes(IDS_PLUGIN_DESCRIPTION).GetString();\n', '        return KoUi() ? L"TrafficMonitor용 배터리 플러그인입니다." : g_data.StringRes(IDS_PLUGIN_DESCRIPTION).GetString();\n'),
    ])
    patch(P('OptionsDlg.cpp'), [
        ('#include "OptionsDlg.h"\n', '#include "OptionsDlg.h"\n#include "KoLocalize.h"\n'),
        ('    return TRUE;  // return TRUE unless you set the focus to a control\n', '    KoLocalizeDialog(GetSafeHwnd());      // [patch] Korean texts\n    return TRUE;  // return TRUE unless you set the focus to a control\n'),
    ])
elif which == 'ipaddress':
    copy_headers(kodir, proj, ['KoLocalize.h'])
    patch(P('OptionsDlg.cpp'), [
        ('#include "OptionsDlg.h"\n', '#include "OptionsDlg.h"\n#include "KoLocalize.h"\n'),
        ('    CDialog::OnInitDialog();\n', '    CDialog::OnInitDialog();\n    KoLocalizeDialog(GetSafeHwnd());      // [patch] Korean texts\n'),
    ])
    # the refresh that the previous binary patch added: update the connection list every 10 seconds
    patch(P('IpAddress.cpp'), [
        ('#include "pch.h"\n', '#include "pch.h"\n#include "KoLocalize.h"\n'),
        ('        return g_data.StringRes(IDS_PLUGIN_NAME).GetString();\n', '        return KoUi() ? L"IP 주소 플러그인" : g_data.StringRes(IDS_PLUGIN_NAME).GetString();\n'),
        ('        return g_data.StringRes(IDS_PLUGIN_DESCRIPTION).GetString();\n', '        return KoUi() ? L"로컬 IPv4 주소를 표시하는 플러그인입니다." : g_data.StringRes(IDS_PLUGIN_DESCRIPTION).GetString();\n'),
        ('void CIpAddress::DataRequired()\n{\n    //TODO: 在此添加获取监控数据的代码\n}\n',
         '''void CIpAddress::DataRequired()
{
    // [patch] refresh the list of network connections every 10 seconds (the first call only starts the timer)
    static ULONGLONG last_tick = 0;
    ULONGLONG now = GetTickCount64();
    if (last_tick == 0)
        last_tick = now;
    else if (now - last_tick >= 10000)
    {
        last_tick = now;
        g_data.UpdateConnections();
    }
}
'''),
    ])
elif which == 'powermon':
    copy_headers(kodir, proj, ['KoLocalize.h', 'KoPowerMon.h'])
    patch(P('OptionsDlg.cpp'), [
        ('#include "OptionsDlg.h"\n', '#include "OptionsDlg.h"\n#include "KoLocalize.h"\n'),
        ('\tCDialog::OnInitDialog();\n\n\t{\n\t\t//为Tab Control增加两个页面\n\t\tm_tab.InsertItem(0, L"单位设置");\n\t\tm_tab.InsertItem(1, L"硬件监控");\n\t\tm_tab.InsertItem(2, L"信息");\n',
         '\tCDialog::OnInitDialog();\n\tKoLocalizeDialog(GetSafeHwnd());      // [patch] Korean texts (the main window is Chinese in the original)\n\n\t{\n\t\t//为Tab Control增加两个页面\n\t\tm_tab.InsertItem(0, KoUi() ? L"단위 설정" : L"单位设置");\n\t\tm_tab.InsertItem(1, KoUi() ? L"하드웨어 모니터" : L"硬件监控");\n\t\tm_tab.InsertItem(2, KoUi() ? L"정보" : L"信息");\n'),
    ])
    for f in ('HardwareSensorPage.cpp', 'InformationPage.cpp', 'UnitStringSettingPage.cpp'):
        patch(P(f), [
            ('#include "pch.h"\n', '#include "pch.h"\n#include "KoLocalize.h"\n'),
            ('\tCTabDlg::OnInitDialog();\n', '\tCTabDlg::OnInitDialog();\n\tKoLocalizeDialog(GetSafeHwnd());      // [patch] Korean texts\n'),
        ])
    # item names, labels and messages come from the string table
    patch(P('DataManager.cpp'), [
        ('#include "DataManager.h"\n', '#include "DataManager.h"\n#include "KoPowerMon.h"\n'),
        ('\t\tm_string_table[id].LoadString(id);\n', '\t\tm_string_table[id].LoadString(id);\n\t\tKoPowerMonOverride(id, m_string_table[id]);      // [patch] Korean texts\n'),
    ])
    # texts of the hardware page (cosmetic, never fails)
    patch_soft(P('HardwareSensorPage.cpp'), [
        ('InsertItem(L"Battery")', 'InsertItem(KoUi() ? L"배터리" : L"Battery")'),
        ('L"Power Rate: "', '(KoUi() ? L"전력: " : L"Power Rate: ")'),
        ('L"Capacity: "', '(KoUi() ? L"용량: " : L"Capacity: ")'),
        ('L"Voltage: "', '(KoUi() ? L"전압: " : L"Voltage: ")'),
        ('L"AC Line Status: "', '(KoUi() ? L"전원 연결 상태: " : L"AC Line Status: ")'),
        ('L"Battery Flag: "', '(KoUi() ? L"배터리 플래그: " : L"Battery Flag: ")'),
        ('L"Battery Life Percent: "', '(KoUi() ? L"배터리 잔량(%): " : L"Battery Life Percent: ")'),
        ('L"System Status Flag: "', '(KoUi() ? L"시스템 상태 플래그: " : L"System Status Flag: ")'),
        ('L"Battery Life Time: "', '(KoUi() ? L"배터리 남은 시간: " : L"Battery Life Time: ")'),
        ('L"Battery Full Life Time: "', '(KoUi() ? L"완충 시 사용 시간: " : L"Battery Full Life Time: ")'),
    ])
    patch_soft(P('InformationPage.cpp'), [
        ('L"Debug Information"', '(KoUi() ? L"디버그 정보" : L"Debug Information")'),
    ])
elif which == 'ping':
    patch(P('PingPlugin.cpp'), [
        ('const wchar_t* CPingItem::GetItemName() const ',
         'static bool PingIsKo() { static const bool ko = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN; return ko; }\nconst wchar_t* CPingItem::GetItemName() const '),
        ('case TMI_NAME:        return L"Ping Monitor";', 'case TMI_NAME:        return PingIsKo() ? L"Ping 모니터" : L"Ping Monitor";'),
        ('case TMI_DESCRIPTION: return L"Displays real-time ping latency to a configurable host.";',
         'case TMI_DESCRIPTION: return PingIsKo() ? L"지정한 호스트까지의 Ping 지연 시간을 실시간으로 표시합니다." : L"Displays real-time ping latency to a configurable host.";'),
        ('SetWindowTextW(hDlg, L"Ping Plugin - Options");', 'SetWindowTextW(hDlg, PingIsKo() ? L"Ping 플러그인 - 설정" : L"Ping Plugin - Options");'),
        ('pushStr(L"Ping Plugin Options"); // title', 'pushStr(PingIsKo() ? L"Ping 플러그인 설정" : L"Ping Plugin Options"); // title'),
        ('7, 10, 206, 10, 1001, L"STATIC", L"Ping target host or IP:");', '7, 10, 206, 10, 1001, L"STATIC", PingIsKo() ? L"Ping 대상 호스트 또는 IP:" : L"Ping target host or IP:");'),
        ('56, 46, 50, 14, IDOK, L"BUTTON", L"OK");', '56, 46, 50, 14, IDOK, L"BUTTON", PingIsKo() ? L"확인" : L"OK");'),
        ('113, 46, 50, 14, IDCANCEL, L"BUTTON", L"Cancel");', '113, 46, 50, 14, IDCANCEL, L"BUTTON", PingIsKo() ? L"취소" : L"Cancel");'),
        ('data.prompt = L"Ping target host or IP:";', 'data.prompt = PingIsKo() ? L"Ping 대상 호스트 또는 IP:" : L"Ping target host or IP:";'),
    ])
else:
    raise SystemExit('unknown plugin ' + which)
print(which, 'patched')
