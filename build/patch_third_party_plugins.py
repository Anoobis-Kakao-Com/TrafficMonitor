# Korean settings windows for IpAddress, PowerMon and Ping (+ the 10 second refresh of the IpAddress plugin).
# usage:  python patch_third_party_plugins.py <ipaddress|powermon|ping> <project folder> <KoLocalize.h>
import sys, shutil, os

def patch(path, edits):
    raw = open(path, 'rb').read()
    text = raw.decode('utf-8-sig')
    crlf = '\r\n' in text
    text = text.replace('\r\n', '\n')
    for old, new in edits:
        n = text.count(old)
        if n != 1:
            raise SystemExit('PATCH FAILED: %r found %d times in %s' % (old[:70], n, path))
        text = text.replace(old, new)
    if crlf:
        text = text.replace('\n', '\r\n')
    # always UTF-8 with BOM: MSVC then reads the Korean texts correctly
    open(path, 'wb').write(b'\xef\xbb\xbf' + text.encode('utf-8'))

which, proj, header = sys.argv[1], sys.argv[2], sys.argv[3]

if which == 'ipaddress':
    shutil.copy(header, os.path.join(proj, 'KoLocalize.h'))
    patch(os.path.join(proj, 'OptionsDlg.cpp'), [
        ('#include "OptionsDlg.h"\n', '#include "OptionsDlg.h"\n#include "KoLocalize.h"\n'),
        ('    CDialog::OnInitDialog();\n',
         '    CDialog::OnInitDialog();\n    KoLocalizeDialog(GetSafeHwnd());      // [patch] Korean texts\n'),
    ])
    # the refresh that the previous binary patch added: update the connection list every 10 seconds
    patch(os.path.join(proj, 'IpAddress.cpp'), [
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
    shutil.copy(header, os.path.join(proj, 'KoLocalize.h'))
    patch(os.path.join(proj, 'OptionsDlg.cpp'), [
        ('#include "OptionsDlg.h"\n', '#include "OptionsDlg.h"\n#include "KoLocalize.h"\n'),
        ('\tCDialog::OnInitDialog();\n\n\t{\n\t\t//为Tab Control增加两个页面\n\t\tm_tab.InsertItem(0, L"单位设置");\n\t\tm_tab.InsertItem(1, L"硬件监控");\n\t\tm_tab.InsertItem(2, L"信息");\n',
         '\tCDialog::OnInitDialog();\n\tKoLocalizeDialog(GetSafeHwnd());      // [patch] Korean texts\n\n\t{\n\t\t//为Tab Control增加两个页面\n\t\tm_tab.InsertItem(0, KoUi() ? L"단위 설정" : L"单位设置");\n\t\tm_tab.InsertItem(1, KoUi() ? L"하드웨어 모니터" : L"硬件监控");\n\t\tm_tab.InsertItem(2, KoUi() ? L"정보" : L"信息");\n'),
    ])
    for f, cls in (('HardwareSensorPage.cpp', 'HardwareSensorPage'), ('InformationPage.cpp', 'InformationPage'), ('UnitStringSettingPage.cpp', 'UnitStringSettingPage')):
        p = os.path.join(proj, f)
        text = open(p, 'rb').read().decode('utf-8-sig').replace('\r\n', '\n')
        # include after the first #include "pch.h"
        patch(p, [
            ('#include "pch.h"\n', '#include "pch.h"\n#include "KoLocalize.h"\n'),
            ('\tCTabDlg::OnInitDialog();\n', '\tCTabDlg::OnInitDialog();\n\tKoLocalizeDialog(GetSafeHwnd());      // [patch] Korean texts\n'),
        ])
elif which == 'ping':
    p = os.path.join(proj, 'PingPlugin.cpp')
    patch(p, [
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
