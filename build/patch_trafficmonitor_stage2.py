# Stage 2 of the TrafficMonitor patch (runs AFTER patch_trafficmonitor.py, in the same V1.86 source folder).
#  - window height 44 px, 1/2/3 rows (R9), column separator (R12), settings window fields (R7),
#    font size / vertical margin limit (L4), unit box width that only grows (R5)
import re

def _read(path):
    raw = open(path, 'rb').read()
    bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig')
    crlf = '\r\n' in text
    return text.replace('\r\n', '\n'), bom, crlf

def _write(path, text, bom, crlf):
    if crlf:
        text = text.replace('\n', '\r\n')
    out = text.encode('utf-8')
    if bom:
        out = b'\xef\xbb\xbf' + out
    open(path, 'wb').write(out)

def patch(path, edits):
    text, bom, crlf = _read(path)
    for old, new in edits:
        n = text.count(old)
        if n != 1:
            raise SystemExit('PATCH FAILED: %r found %d times in %s' % (old[:60], n, path))
        text = text.replace(old, new)
    _write(path, text, bom, crlf)

def replace_between(path, start, end, new):
    text, bom, crlf = _read(path)
    if text.count(start) != 1 or text.count(end) != 1:
        raise SystemExit('PATCH FAILED: markers %r (%d) / %r (%d) in %s' % (start[:50], text.count(start), end[:50], text.count(end), path))
    a = text.index(start)
    b = text.index(end)
    if b < a:
        raise SystemExit('PATCH FAILED: marker order in %s' % path)
    text = text[:a] + new + text[b:]
    _write(path, text, bom, crlf)

def patch_utf16(path, edits, regex=False):
    raw = open(path, 'rb').read()
    text = raw.decode('utf-16')
    crlf = '\r\n' in text
    text = text.replace('\r\n', '\n')
    for old, new in edits:
        if regex:
            text, n = re.subn(old, lambda m: new, text)
        else:
            n = text.count(old)
            text = text.replace(old, new)
        if n != 1:
            raise SystemExit('PATCH FAILED: %r found %d times in %s' % (old[:60], n, path))
    if crlf:
        text = text.replace('\n', '\r\n')
    open(path, 'wb').write(b'\xff\xfe' + text.encode('utf-16-le'))

TB = 'TrafficMonitor/'

# ---------------------------------------------------------------- settings data
patch(TB + 'CommonData.h', [
    ('    bool horizontal_arrange{ true };    //水平排列\n',
     '''    bool horizontal_arrange{ true };    //水平排列
    // [patch] number of rows (1 - 3); horizontal_arrange is kept in sync (rows == 1) for old config files
    int rows{ 2 };
    int RowsClamped() const { return rows < 1 ? 1 : (rows > 3 ? 3 : rows); }
    // [patch] settings that were only available in config.ini before
    std::wstring value_pad_char{ L"\\u2025" };      // pad character(s) for empty digit positions (empty = off)
    int colon_gap{ 1 };                             // gap on both sides of the colon (pixels at 96 DPI, 0 - 30)
    // [patch] column separator
    bool separator_enable{ false };
    int separator_width{ 2 };                       // pixels at 96 DPI
    COLORREF separator_color{ RGB(128, 128, 128) };
    int separator_alpha{ 60 };                      // opacity in percent (0 - 100)
'''),
])

patch(TB + 'TrafficMonitor.cpp', [
    ('    m_taskbar_data.horizontal_arrange = ini.GetBool(_T("task_bar"), _T("horizontal_arrange"), false);\n',
     '''    m_taskbar_data.horizontal_arrange = ini.GetBool(_T("task_bar"), _T("horizontal_arrange"), false);
    // [patch] rows (old config files only have horizontal_arrange: true = 1 row, false = 2 rows)
    m_taskbar_data.rows = ini.GetInt(_T("task_bar"), _T("task_bar_rows"), m_taskbar_data.horizontal_arrange ? 1 : 2);
    m_taskbar_data.rows = m_taskbar_data.RowsClamped();
    m_taskbar_data.horizontal_arrange = (m_taskbar_data.rows == 1);
    m_taskbar_data.value_pad_char = ini.GetString(_T("task_bar"), _T("value_pad_char"), L"\\u2025");
    m_taskbar_data.colon_gap = ini.GetInt(_T("task_bar"), _T("colon_gap"), 1);
    if (m_taskbar_data.colon_gap < 0) m_taskbar_data.colon_gap = 0;
    if (m_taskbar_data.colon_gap > 30) m_taskbar_data.colon_gap = 30;
    m_taskbar_data.separator_enable = ini.GetBool(_T("task_bar"), _T("separator_enable"), false);
    m_taskbar_data.separator_width = ini.GetInt(_T("task_bar"), _T("separator_width"), 2);
    if (m_taskbar_data.separator_width < 1) m_taskbar_data.separator_width = 1;
    if (m_taskbar_data.separator_width > 10) m_taskbar_data.separator_width = 10;
    m_taskbar_data.separator_color = static_cast<COLORREF>(ini.GetInt(_T("task_bar"), _T("separator_color"), static_cast<int>(RGB(128, 128, 128))));
    m_taskbar_data.separator_alpha = ini.GetInt(_T("task_bar"), _T("separator_alpha"), 60);
    if (m_taskbar_data.separator_alpha < 0) m_taskbar_data.separator_alpha = 0;
    if (m_taskbar_data.separator_alpha > 100) m_taskbar_data.separator_alpha = 100;
'''),
    ('    ini.WriteBool(L"task_bar", L"horizontal_arrange", m_taskbar_data.horizontal_arrange);\n',
     '''    ini.WriteBool(L"task_bar", L"horizontal_arrange", m_taskbar_data.rows == 1);
    ini.WriteInt(L"task_bar", L"task_bar_rows", m_taskbar_data.RowsClamped());
    ini.WriteString(L"task_bar", L"value_pad_char", m_taskbar_data.value_pad_char);
    ini.WriteInt(L"task_bar", L"colon_gap", m_taskbar_data.colon_gap);
    ini.WriteBool(L"task_bar", L"separator_enable", m_taskbar_data.separator_enable);
    ini.WriteInt(L"task_bar", L"separator_width", m_taskbar_data.separator_width);
    ini.WriteInt(L"task_bar", L"separator_color", static_cast<int>(m_taskbar_data.separator_color));
    ini.WriteInt(L"task_bar", L"separator_alpha", m_taskbar_data.separator_alpha);
'''),
])

# ---------------------------------------------------------------- resource ids
patch(TB + 'resource.h', [
    ('#define IDC_MBD_TEMP_TIP_EDIT           1219\n',
     '''#define IDC_MBD_TEMP_TIP_EDIT           1219
#define IDC_TASKBAR_ROWS_STATIC         1220
#define IDC_TASKBAR_ROWS_COMBO          1221
#define IDC_VALUE_PAD_CHAR_STATIC       1222
#define IDC_VALUE_PAD_CHAR_EDIT         1223
#define IDC_COLON_GAP_STATIC            1224
#define IDC_COLON_GAP_EDIT              1225
#define IDC_SEPARATOR_ENABLE_CHECK      1226
#define IDC_SEPARATOR_WIDTH_STATIC      1227
#define IDC_SEPARATOR_WIDTH_EDIT        1228
#define IDC_SEPARATOR_COLOR_STATIC      1229
#define IDC_SEPARATOR_COLOR_EDIT        1230
#define IDC_SEPARATOR_ALPHA_STATIC      1231
#define IDC_SEPARATOR_ALPHA_EDIT        1232
'''),
    ('#define _APS_NEXT_CONTROL_VALUE         1220', '#define _APS_NEXT_CONTROL_VALUE         1233'),
])

# ---------------------------------------------------------------- dialog template (UTF-16)
patch_utf16(TB + 'TrafficMonitor.rc', [
    ('IDD_TASKBAR_SETTINGS_DIALOG DIALOGEX 0, 0, 270, 500', 'IDD_TASKBAR_SETTINGS_DIALOG DIALOGEX 0, 0, 270, 566'),
    ('                    "Button",BS_AUTOCHECKBOX | WS_TABSTOP,76,393,174,10\n',
     '''                    "Button",BS_AUTOCHECKBOX | WS_TABSTOP,76,393,174,10
    GROUPBOX        "TXT_PAD_SEPARATOR_SETTINGS",IDC_STATIC,7,496,256,62
    LTEXT           "TXT_VALUE_PAD_CHAR",IDC_VALUE_PAD_CHAR_STATIC,14,509,58,8
    EDITTEXT        IDC_VALUE_PAD_CHAR_EDIT,74,507,24,12,ES_AUTOHSCROLL
    LTEXT           "TXT_COLON_GAP",IDC_COLON_GAP_STATIC,118,509,60,8
    EDITTEXT        IDC_COLON_GAP_EDIT,180,507,24,12,ES_AUTOHSCROLL | ES_NUMBER
    CONTROL         "TXT_SEPARATOR_ENABLE",IDC_SEPARATOR_ENABLE_CHECK,"Button",BS_AUTOCHECKBOX | WS_TABSTOP,14,524,200,10
    LTEXT           "TXT_SEPARATOR_WIDTH",IDC_SEPARATOR_WIDTH_STATIC,14,541,44,8
    EDITTEXT        IDC_SEPARATOR_WIDTH_EDIT,60,539,22,12,ES_AUTOHSCROLL | ES_NUMBER
    LTEXT           "TXT_SEPARATOR_COLOR",IDC_SEPARATOR_COLOR_STATIC,90,541,58,8
    EDITTEXT        IDC_SEPARATOR_COLOR_EDIT,150,539,40,12,ES_AUTOHSCROLL
    LTEXT           "TXT_SEPARATOR_ALPHA",IDC_SEPARATOR_ALPHA_STATIC,198,541,40,8
    EDITTEXT        IDC_SEPARATOR_ALPHA_EDIT,240,539,20,12,ES_AUTOHSCROLL | ES_NUMBER
'''),
])
patch_utf16(TB + 'TrafficMonitor.rc', [
    (r'    CONTROL +"TXT_HORIZONTAL_ARRANGE",IDC_HORIZONTAL_ARRANGE_CHECK,\n +"Button",BS_AUTOCHECKBOX \| WS_TABSTOP,13,161,143,9\n',
     '''    LTEXT           "TXT_TASKBAR_ROWS",IDC_TASKBAR_ROWS_STATIC,13,162,40,8
    COMBOBOX        IDC_TASKBAR_ROWS_COMBO,55,160,60,40,CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP
'''),
], regex=True)

# ---------------------------------------------------------------- default language (English)
patch(TB + 'language/English.ini', [
    ('TXT_VERTICAL_MARGIN = "Vertical Margin:"\n',
     '''TXT_VERTICAL_MARGIN = "Vertical Margin:"
TXT_TASKBAR_ROWS = "Rows:"
TXT_ROWS_1 = "1 row"
TXT_ROWS_2 = "2 rows"
TXT_ROWS_3 = "3 rows"
TXT_PAD_SEPARATOR_SETTINGS = "Value padding and column separator"
TXT_VALUE_PAD_CHAR = "Pad character:"
TXT_COLON_GAP = "Colon gap (px):"
TXT_SEPARATOR_ENABLE = "Show column separator"
TXT_SEPARATOR_WIDTH = "Width (px):"
TXT_SEPARATOR_COLOR = "Color (RRGGBB):"
TXT_SEPARATOR_ALPHA = "Opacity (%):"
TXT_TASKBAR_HEIGHT_LIMIT_NOTICE = "The height limit of the taskbar window was exceeded. The maximum settings were applied: font size <%1%>, vertical margin <%2%>."
'''),
])

# ---------------------------------------------------------------- settings window code
patch(TB + 'TaskBarSettingsDlg.cpp', [
    ('    ((CButton*)GetDlgItem(IDC_HORIZONTAL_ARRANGE_CHECK))->SetCheck(m_data.horizontal_arrange);\n',
     '''    // [patch] rows, pad character, colon gap, column separator
    {
        CComboBox* rows_combo = (CComboBox*)GetDlgItem(IDC_TASKBAR_ROWS_COMBO);
        if (rows_combo != nullptr)
        {
            rows_combo->AddString(CCommon::LoadText(L"TXT_ROWS_1"));
            rows_combo->AddString(CCommon::LoadText(L"TXT_ROWS_2"));
            rows_combo->AddString(CCommon::LoadText(L"TXT_ROWS_3"));
            rows_combo->SetCurSel(m_data.RowsClamped() - 1);
        }
        SetDlgItemText(IDC_VALUE_PAD_CHAR_EDIT, m_data.value_pad_char.c_str());
        SetDlgItemInt(IDC_COLON_GAP_EDIT, m_data.colon_gap, FALSE);
        CheckDlgButton(IDC_SEPARATOR_ENABLE_CHECK, m_data.separator_enable);
        SetDlgItemInt(IDC_SEPARATOR_WIDTH_EDIT, m_data.separator_width, FALSE);
        CString color_str;
        color_str.Format(L"%02X%02X%02X", GetRValue(m_data.separator_color), GetGValue(m_data.separator_color), GetBValue(m_data.separator_color));
        SetDlgItemText(IDC_SEPARATOR_COLOR_EDIT, color_str);
        SetDlgItemInt(IDC_SEPARATOR_ALPHA_EDIT, m_data.separator_alpha, FALSE);
    }
'''),
    ('    m_data.horizontal_arrange = (((CButton*)GetDlgItem(IDC_HORIZONTAL_ARRANGE_CHECK))->GetCheck() != 0);\n',
     '    // [patch] the check box was replaced by the rows combo box (read in OnOK)\n'),
    ('    GetDlgItemText(IDC_FONT_NAME_EDIT1, m_data.font.name);\n',
     '''    GetDlgItemText(IDC_FONT_NAME_EDIT1, m_data.font.name);

    // [patch] read rows, pad character, colon gap and the column separator settings
    {
        CComboBox* rows_combo = (CComboBox*)GetDlgItem(IDC_TASKBAR_ROWS_COMBO);
        if (rows_combo != nullptr && rows_combo->GetCurSel() >= 0)
            m_data.rows = rows_combo->GetCurSel() + 1;
        m_data.horizontal_arrange = (m_data.rows == 1);

        CString str;
        GetDlgItemText(IDC_VALUE_PAD_CHAR_EDIT, str);
        m_data.value_pad_char = std::wstring(str.GetString());
        if (m_data.value_pad_char.size() > 2)
            m_data.value_pad_char.resize(2);

        BOOL ok = FALSE;
        int v = static_cast<int>(GetDlgItemInt(IDC_COLON_GAP_EDIT, &ok, FALSE));
        if (ok)
            m_data.colon_gap = (v < 0 ? 0 : (v > 30 ? 30 : v));

        m_data.separator_enable = (IsDlgButtonChecked(IDC_SEPARATOR_ENABLE_CHECK) != 0);
        v = static_cast<int>(GetDlgItemInt(IDC_SEPARATOR_WIDTH_EDIT, &ok, FALSE));
        if (ok)
            m_data.separator_width = (v < 1 ? 1 : (v > 10 ? 10 : v));
        GetDlgItemText(IDC_SEPARATOR_COLOR_EDIT, str);
        str.Trim();
        if (str.GetLength() > 0 && str.GetLength() <= 6)
        {
            unsigned long rgb = wcstoul(str.GetString(), nullptr, 16);
            m_data.separator_color = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        }
        v = static_cast<int>(GetDlgItemInt(IDC_SEPARATOR_ALPHA_EDIT, &ok, FALSE));
        if (ok)
            m_data.separator_alpha = (v < 0 ? 0 : (v > 100 ? 100 : v));
    }

    // [patch] The taskbar window has a fixed height (44 px, divided by the number of rows). Text that is taller than a
    // row sticks out of the window and is cut off. The font size is therefore limited to the largest size that fits
    // (the tallest ink of the line, including the pad character below the baseline), and then the vertical margin
    // to what is left. Negative margins move the text inwards and are not limited.
    {
        const int dpi = theApp.GetDpi();
        int win_h = MulDiv(44, dpi, 96);
        HWND h_tray = ::FindWindow(L"Shell_TrayWnd", nullptr);
        CRect rc_tray;
        if (h_tray != nullptr && ::GetWindowRect(h_tray, &rc_tray) && rc_tray.Height() > 0 && rc_tray.Height() < win_h)
            win_h = rc_tray.Height();
        const int row_h = win_h / m_data.RowsClamped();

        CClientDC dc(this);
        auto line_height = [&](int size) -> int
        {
            FontInfo fi = m_data.font;
            fi.size = size;
            CFont font;
            fi.Create(font, dpi);
            CFont* old_font = dc.SelectObject(&font);
            TEXTMETRIC tm{};
            dc.GetTextMetrics(&tm);
            int extra = 0;      // how far the pad character sticks out below the line
            if (!m_data.value_pad_char.empty())
            {
                GLYPHMETRICS gm{};
                MAT2 mat{ {0, 1}, {0, 0}, {0, 0}, {0, 1} };
                DWORD r = ::GetGlyphOutlineW(dc.GetSafeHdc(), m_data.value_pad_char[0], GGO_METRICS, &gm, 0, nullptr, &mat);
                if (r != GDI_ERROR)
                {
                    int below = static_cast<int>(gm.gmBlackBoxY) - gm.gmptGlyphOrigin.y;
                    if (below > tm.tmDescent)
                        extra = below - tm.tmDescent;
                }
            }
            dc.SelectObject(old_font);
            return tm.tmHeight + 2 * extra + 1;     // +1: GDI and Direct2D round the line height differently
        };

        const int requested_size = m_data.font.size;
        int size = requested_size;
        while (size > MIN_FONT_SIZE && line_height(size) > row_h)
            --size;
        int max_margin = row_h - line_height(size);
        if (max_margin < 0)
            max_margin = 0;
        max_margin = max_margin * 96 / dpi;       // the setting is in pixels at 96 DPI
        int margin = m_data.vertical_margin;
        if (margin > max_margin)
            margin = max_margin;
        if (size != requested_size || margin != m_data.vertical_margin)
        {
            m_data.font.size = size;
            m_data.vertical_margin = margin;
            m_font_size_edit.SetValue(size);
            m_vertical_margin_edit.SetValue(margin);
            MessageBox(CCommon::LoadTextFormat(L"TXT_TASKBAR_HEIGHT_LIMIT_NOTICE", { size, margin }), NULL, MB_OK | MB_ICONINFORMATION);
        }
    }
'''),
])

# ---------------------------------------------------------------- taskbar window
D = TB + 'TaskBarDlg.cpp'

# the pad glyph compensation of stage 1 did not help; the limit above replaces it
patch(D, [
    ('                int overflow = PadBottomOverflow(pDC, font, pad, rect_value, win_h);\n',
     '                int overflow = 0;     // the limit is enforced when the settings are applied (CTaskBarSettingsDlg::OnOK)\n'),
])

# colon gap and pad character now come from the settings
replace_between(D, 'static int ColonGap()\n{\n    static bool loaded = false;', '// [patch] Left bearing (a), ink width (b) and right bearing (c)',
'''static int ColonGap()
{
    int gap = theApp.m_taskbar_data.colon_gap;
    if (gap < 0)
        gap = 0;
    if (gap > 30)
        gap = 30;
    return theApp.DPI(gap);
}

''')
replace_between(D, 'static const std::wstring& ValuePadChars()\n{', '// [patch] How many pixels the ink of the pad glyph',
'''static const std::wstring& ValuePadChars()
{
    return theApp.m_taskbar_data.value_pad_char;
}

''')

# widest unit that was really shown (R5); cleared when the settings are applied
patch(D, [
    ('void CTaskBarDlg::SetTextFont()\n{\n',
     '''// [patch] The widest unit text that was really shown for every item. The unit box of an item is as wide as that
// (it only grows, so the window does not jump around), instead of the widest possible sample unit.
static std::map<std::wstring, int>& UnitHighWater()
{
    static std::map<std::wstring, int> widths;
    return widths;
}

void CTaskBarDlg::SetTextFont()
{
'''),
    ('void CTaskBarDlg::ApplySettings()\n{\n    SetTextFont();\n',
     'void CTaskBarDlg::ApplySettings()\n{\n    UnitHighWater().clear();\n    SetTextFont();\n'),
])

# DrawInfo: rows and column separators
patch(D, [
    ('    auto last_iter = m_item_widths.begin();\n',
     '''    auto last_iter = m_item_widths.begin();
    // [patch] column separator: a vertical line in the middle of the gap between two columns. 1 row: from 20% to 80%
    // of the window height; 2 or 3 rows: from the middle of the first row to the middle of the last row.
    auto draw_separator = [&](int column_right, int rows_count)
    {
        const auto& sd = theApp.m_taskbar_data;
        if (!sd.separator_enable || sd.separator_width <= 0)
            return;
        const int w = max(1, DPI(sd.separator_width));
        const int x = column_right + DPI(sd.item_space) / 2 - w / 2;
        int y1, y2;
        if (rows_count <= 1)
        {
            y1 = m_window_height * 20 / 100;
            y2 = m_window_height * 80 / 100;
        }
        else
        {
            const int row_h = m_window_height / rows_count;
            y1 = row_h / 2;
            y2 = m_window_height - row_h / 2;
        }
        draw.FillRect(CRect(x, y1, x + w, y2), sd.separator_color, static_cast<BYTE>(sd.separator_alpha * 255 / 100));
    };
'''),
])
replace_between(D, '            if (theApp.m_taskbar_data.horizontal_arrange) //水平排列\n', '        }\n        //任务栏在桌面两侧\n',
'''            const int rows = theApp.m_taskbar_data.RowsClamped();
            if (rows <= 1) //水平排列
            {
                if (index > 0)
                    item_rect.MoveToX(item_rect.right + DPI(theApp.m_taskbar_data.item_space));
                item_rect.right = item_rect.left + iter->item_width.TotalWidth();
                item_rect.bottom = item_rect.top + m_window_height;
                if (iter->IsPlugin())
                    DrawPluginItem(draw, iter->PluginItem(), item_rect, iter->item_width.label_width);
                else
                    DrawDisplayItem(draw, iter->ItemType(), item_rect, iter->item_width.label_width);
                if (index < item_count - 1)
                    draw_separator(item_rect.right, 1);
            }
            else if (index % rows == 0)     // [patch] a column of up to "rows" items is drawn when its first item is reached
            {
                const int n = min(rows, item_count - index);
                const int left = item_rect.right + DPI(theApp.m_taskbar_data.item_space);
                if (n < 2)      // the last single item uses the whole height (name above, value below)
                {
                    item_rect.MoveToXY(left, 0);
                    item_rect.bottom = m_window_height;
                    item_rect.right = item_rect.left + iter->item_width.MaxWidth();
                    if (iter->IsPlugin())
                        DrawPluginItem(draw, iter->PluginItem(), item_rect, iter->item_width.label_width, true);
                    else
                        DrawDisplayItem(draw, iter->ItemType(), item_rect, iter->item_width.label_width, true);
                }
                else
                {
                    int width = 0;
                    for (int k = 0; k < n; ++k)
                        width = max(width, m_item_widths[index + k].item_width.TotalWidth());
                    const int row_h = m_window_height / rows;
                    const int margin = DPI(theApp.m_taskbar_data.vertical_margin);
                    for (int k = 0; k < n; ++k)
                    {
                        CRect rc;
                        rc.left = left;
                        rc.right = left + width;
                        rc.top = k * row_h;
                        rc.bottom = (k == rows - 1) ? m_window_height : (k + 1) * row_h - 1;
                        if (k == 0)
                            rc.top -= margin;
                        if (k == rows - 1)
                            rc.bottom += margin;
                        const auto& wi = m_item_widths[index + k];
                        if (wi.IsPlugin())
                            DrawPluginItem(draw, wi.PluginItem(), rc, wi.item_width.label_width);
                        else
                            DrawDisplayItem(draw, wi.ItemType(), rc, wi.item_width.label_width);
                    }
                    item_rect.left = left;
                    item_rect.right = left + width;
                    item_rect.top = 0;
                    item_rect.bottom = m_window_height;
                    if (index + n < item_count)
                        draw_separator(item_rect.right, rows);
                }
            }
''')

# CalculateWindowSize
patch(D, [
    ('    bool horizontal_arrange = theApp.m_taskbar_data.horizontal_arrange && m_taskbar_on_top_or_bottom;\n',
     '    const int tb_rows = theApp.m_taskbar_data.RowsClamped();\n    bool horizontal_arrange = (tb_rows <= 1) && m_taskbar_on_top_or_bottom;\n'),
    ('        if (theApp.m_taskbar_data.horizontal_arrange)   //水平排列时\n',
     '        if (tb_rows <= 1)   //水平排列时\n'),
    ('''        if (!horizontal_arrange)
            m_window_height = TASKBAR_WND_HEIGHT;
        else
            m_window_height = TASKBAR_WND_HEIGHT * 2 / 3;
''',
     '''        // [patch] fixed height for 1, 2 and 3 rows (44 px; never higher than the taskbar itself)
        m_window_height = DPI(44);
        if (m_rcTaskbar.Height() > 0 && m_rcTaskbar.Height() < m_window_height)
            m_window_height = m_rcTaskbar.Height();
'''),
])
replace_between(D, '    // [patch] make the upper and lower item of the same column line up:\n', '    //计算窗口总宽度\n',
'''    // [patch] unit box = the widest unit text that was really shown so far (grows only; cleared when the settings change)
    {
        auto& high_water = UnitHighWater();
        for (auto& wi : m_item_widths)
        {
            ItemWidth& iw = wi.item_width;
            if (iw.num_width <= 0 || iw.value_width <= iw.num_width)
                continue;
            std::wstring text;
            std::wstring key;
            if (wi.IsPlugin())
            {
                if (wi.PluginItem() == nullptr)
                    continue;
                const wchar_t* plugin_text = wi.PluginItem()->GetItemValueText();
                text = (plugin_text != nullptr) ? plugin_text : L"";
                key = L"p:";
                key += wi.PluginItem()->GetItemId();
            }
            else
            {
                text = wi.GetItemValueText(false).GetString();
                key = L"i:" + std::to_wstring(static_cast<int>(wi.ItemType()));
            }
            size_t n = 0;
            while (n < text.size() && IsNumberChar(text[n]))
                ++n;
            if (n == 0)
                continue;       // no number (NaN, words): keep the sample based width
            size_t m = n;
            while (m < text.size() && (text[m] == L' ' || text[m] == 0x00A0))
                ++m;
            if (m >= text.size())
                continue;       // no unit
            int unit_w = m_pDC->GetTextExtent(text.c_str() + m, static_cast<int>(text.size() - m)).cx;
            int& best = high_water[key];
            if (unit_w > best)
                best = unit_w;
            iw.value_width = iw.num_width + best;
        }
    }

    // [patch] make the items of the same column (1 - 3 rows) line up:
    //  - items with a name : label box (name + gap + colon) equally wide -> colons and values start at the same x;
    //                        number box equally wide -> units start at the same x
    //  - no name, values with a unit: same format as with names (number right, unit left)
    //  - otherwise an item without a name is centered (see DrawSplitValue)
    if (m_taskbar_on_top_or_bottom && tb_rows >= 2)
    {
        for (int g = 0; g < item_count; g += tb_rows)
        {
            const int n = min(tb_rows, item_count - g);
            if (n < 2)
                break;
            int label_max = 0, num_max = 0, labeled_num = 0;
            for (int k = 0; k < n; ++k)
            {
                ItemWidth& w = m_item_widths[g + k].item_width;
                if (w.label_width > 0)
                {
                    label_max = max(label_max, w.label_width);
                    if (w.num_width > 0)
                    {
                        num_max = max(num_max, w.num_width);
                        ++labeled_num;
                    }
                }
            }
            for (int k = 0; k < n; ++k)
            {
                ItemWidth& w = m_item_widths[g + k].item_width;
                if (w.label_width > 0)
                    w.label_width = label_max;
                if (labeled_num >= 2 && w.label_width > 0 && w.num_width > 0)
                {
                    w.value_width += num_max - w.num_width;
                    w.num_width = num_max;
                }
            }
            int plain_max = 0, plain_count = 0;
            for (int k = 0; k < n; ++k)
            {
                ItemWidth& w = m_item_widths[g + k].item_width;
                if (w.label_width == 0 && w.num_width > 0 && w.value_width > w.num_width)
                {
                    plain_max = max(plain_max, w.num_width);
                    ++plain_count;
                }
            }
            if (plain_count >= 2)
            {
                for (int k = 0; k < n; ++k)
                {
                    ItemWidth& w = m_item_widths[g + k].item_width;
                    if (w.label_width == 0 && w.num_width > 0 && w.value_width > w.num_width)
                    {
                        w.value_width += plain_max - w.num_width;
                        w.num_width = plain_max;
                        w.align_units = true;
                    }
                }
            }
        }
    }

''')
replace_between(D, '        else        //非水平排列时，每两个一组排列\n', '    }\n    else        //任务栏在桌面两侧时\n',
'''        else        //非水平排列时，每 tb_rows 个一组排列
        {
            for (int g = 0; g < item_count; g += tb_rows)
            {
                const int n = min(tb_rows, item_count - g);
                if (n < 2)      //最后一个单独的项目使用整个高度
                {
                    m_window_width += m_item_widths[g].item_width.MaxWidth();
                }
                else
                {
                    int w = 0;
                    for (int k = 0; k < n; ++k)
                        w = max(w, m_item_widths[g + k].item_width.TotalWidth());
                    m_window_width += w;
                }
            }

            m_window_width += DPI(theApp.m_taskbar_data.item_space) * ((item_count + tb_rows - 1) / tb_rows + 1);   //加上每个标签间的空隙
        }
''')
print('stage 2 patches applied')
