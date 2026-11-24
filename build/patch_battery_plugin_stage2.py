# Stage 2 of the Battery plugin patch (runs after patch_battery_plugin.py in the same folder):
#  - icon is 37 x 16 px (35 px wide body, taller than the original, bolt centered, see make_battery_icons.py)
#  - the fill covers the whole inside of the body and is drawn BEFORE the frame, so the bolt of the frame is visible
#  - 100 % = yellow, no bolt, for every power state
import sys

def patch(path, edits):
    raw = open(path, 'rb').read()
    bom = raw.startswith(b'\xef\xbb\xbf')
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
    out = text.encode('utf-8')
    if bom:
        out = b'\xef\xbb\xbf' + out
    open(path, 'wb').write(out)

B = 'plugins_src/Plugins/Battery/'

patch(B + 'BatteryItem.cpp', [
    # ---- width of the item: icon 37 px + gap
    ('''    case BatteryType::ICON:
        return g_data.DPI(20);
    case BatteryType::NUMBER_BESIDE_ICON:
        return g_data.DPI(20) + pDC->GetTextExtent(sample_str).cx;
    }
    return g_data.DPI(20) + pDC->GetTextExtent(sample_str).cx;
''',
     '''    case BatteryType::ICON:
        return g_data.DPI(41);          // [patch] icon 37 px + 4 px
    case BatteryType::NUMBER_BESIDE_ICON:
        return g_data.DPI(41) + pDC->GetTextExtent(sample_str).cx;
    }
    return g_data.DPI(41) + pDC->GetTextExtent(sample_str).cx;
'''),
    # ---- tinted icon: width and height
    ('static void DrawTintedIcon(HDC hdc, HICON hIcon, int x, int y, int size, COLORREF color, const Gdiplus::RectF* clip)',
     'static void DrawTintedIcon(HDC hdc, HICON hIcon, int x, int y, int width, int height, COLORREF color, const Gdiplus::RectF* clip)'),
    ('graphics.DrawImage(&bmp, Gdiplus::Rect(x, y, size, size), 0, 0,', 'graphics.DrawImage(&bmp, Gdiplus::Rect(x, y, width, height), 0, 0,'),
    # ---- frame selection: 100 % never shows the bolt
    ('''    if (g_data.IsAcOnline())
        hIcon = (dark_mode ? g_data.GetIcon(IDI_BATTERY_LIGHT_CHARGE) : g_data.GetIcon(IDI_BATTERY_DARK_CHARGE));
''',
     '''    // [patch] 100 %: always the yellow fill and the frame without the bolt
    const bool is_full = g_data.m_sysPowerStatus.BatteryFlag != 128 && g_data.m_sysPowerStatus.BatteryLifePercent >= 100;
    if (g_data.IsAcOnline() && !is_full)
        hIcon = (dark_mode ? g_data.GetIcon(IDI_BATTERY_LIGHT_CHARGE) : g_data.GetIcon(IDI_BATTERY_DARK_CHARGE));
'''),
    ('    const int icon_size{ g_data.DPI(16) };\n', '    const int icon_size{ g_data.DPI(16) };      // height of the icon\n    const int icon_w{ g_data.DPI(37) };         // [patch] width of the icon (35 px body + 2 px terminal)\n'),
    ('    int content_w = icon_size;\n', '    int content_w = icon_w;\n'),
    # ---- the frame is drawn after the fill
    ('''    ::DrawIconEx(pDC->GetSafeHdc(), icon_point.x, icon_point.y, hIcon, icon_size, icon_size, 0, NULL, DI_NORMAL);
    //填充电量指示
''',
     '''    //填充电量指示 ([patch] the fill is drawn first, the frame (with the bolt) on top of it)
'''),
    ('''    //绘制电池数值
''',
     '''    ::DrawIconEx(pDC->GetSafeHdc(), icon_point.x, icon_point.y, hIcon, icon_w, icon_size, 0, NULL, DI_NORMAL);
    //绘制电池数值
'''),
    # ---- fill area: the whole inside of the body (x 1..34, y 4..12 on the 37 x 16 grid)
    ('        rc_indicater.Y = icon_point.y + g_data.DPIF(6);\n', '        rc_indicater.Y = icon_point.y + g_data.DPIF(4.0f);      // [patch] fill area = inside of the body\n'),
    ('        float indicater_width = static_cast<float>(g_data.DPIF(11.7f) * percent / 100);\n', '        float indicater_width = static_cast<float>(g_data.DPIF(33.0f) * percent / 100);\n'),
    ('        rc_indicater.Height = g_data.DPIF(3.7f);\n',
     '''        rc_indicater.Height = g_data.DPIF(8.0f);
        // [patch] the clip rectangle only limits the width; the fill icon itself has the vertical shape
        Gdiplus::RectF rc_clip(rc_indicater.X, static_cast<float>(icon_point.y), rc_indicater.Width, static_cast<float>(icon_size));
'''),
    ('''        //充电状态下的电量指示使用图标
        if (g_data.IsAcOnline())
''',
     '''        //充电状态下的电量指示使用图标
        if (is_full)    // [patch] 100 %: yellow, whether plugged in or not
        {
            ::DrawIconEx(pDC->GetSafeHdc(), icon_point.x, icon_point.y, g_data.GetIcon(IDI_FILL_LOW), icon_w, icon_size, 0, NULL, DI_NORMAL);
        }
        else if (g_data.IsAcOnline())
'''),
    ('''                rgn.CreateRectRgn(static_cast<int>(rc_indicater.GetLeft()), static_cast<int>(rc_indicater.GetTop())
                    , static_cast<int>(rc_indicater.GetRight()), static_cast<int>(rc_indicater.GetBottom()));
''',
     '''                rgn.CreateRectRgn(static_cast<int>(rc_clip.GetLeft()), static_cast<int>(rc_clip.GetTop())
                    , static_cast<int>(rc_clip.GetRight() + 0.5f), static_cast<int>(rc_clip.GetBottom()));
'''),
    ('''                DrawTintedIcon(pDC->GetSafeHdc(), hFill, icon_point.x, icon_point.y, icon_size, RGB(30, 144, 255),
                    g_data.m_sysPowerStatus.BatteryLifePercent < 100 ? &rc_indicater : nullptr);      // blue
            else
                ::DrawIconEx(pDC->GetSafeHdc(), icon_point.x, icon_point.y, hFill, icon_size, icon_size, 0, NULL, DI_NORMAL);      // green
''',
     '''                DrawTintedIcon(pDC->GetSafeHdc(), hFill, icon_point.x, icon_point.y, icon_w, icon_size, RGB(30, 144, 255),
                    g_data.m_sysPowerStatus.BatteryLifePercent < 100 ? &rc_clip : nullptr);      // blue
            else
                ::DrawIconEx(pDC->GetSafeHdc(), icon_point.x, icon_point.y, hFill, icon_w, icon_size, 0, NULL, DI_NORMAL);      // green
'''),
    ('        rc_text.left = icon_point.x + icon_size + g_data.DPI(2);\n', '        rc_text.left = icon_point.x + icon_w + g_data.DPI(2);\n'),
])
patch(B + 'Battery.cpp', [
    ('    return g_data.DPI(16) + g_data.DPI(2);\n', '    return g_data.DPI(37) + g_data.DPI(2);      // [patch] icon width + gap\n'),
])
patch(B + 'DataManager.cpp', [
    ('MAKEINTRESOURCE(id), IMAGE_ICON, DPI(16), DPI(16), 0);', 'MAKEINTRESOURCE(id), IMAGE_ICON, DPI(37), DPI(16), 0);      // [patch] icons are 37 x 16'),
])
print('battery stage 2 patches applied')
