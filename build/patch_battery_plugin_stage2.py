# Stage 2 of the Battery plugin patch (runs after patch_battery_plugin.py in the same folder):
#  - taller battery body with the bolt centered inside (new icons are copied by the workflow),
#    fill area coordinates for the new body, and "100% = yellow, no bolt" for every power state.
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

P = 'plugins_src/Plugins/Battery/BatteryItem.cpp'
patch(P, [
    ('''    if (g_data.IsAcOnline())
        hIcon = (dark_mode ? g_data.GetIcon(IDI_BATTERY_LIGHT_CHARGE) : g_data.GetIcon(IDI_BATTERY_DARK_CHARGE));
''',
     '''    // [patch] 100 %: always the yellow fill and the frame without the bolt
    const bool is_full = g_data.m_sysPowerStatus.BatteryFlag != 128 && g_data.m_sysPowerStatus.BatteryLifePercent >= 100;
    if (g_data.IsAcOnline() && !is_full)
        hIcon = (dark_mode ? g_data.GetIcon(IDI_BATTERY_LIGHT_CHARGE) : g_data.GetIcon(IDI_BATTERY_DARK_CHARGE));
'''),
    ('''        rc_indicater.Y = icon_point.y + g_data.DPIF(6);
''',
     '''        rc_indicater.Y = icon_point.y + g_data.DPIF(4.2f);      // [patch] fill area of the taller body (see make_battery_icons.py)
'''),
    ('''        rc_indicater.Height = g_data.DPIF(3.7f);
''',
     '''        rc_indicater.Height = g_data.DPIF(7.6f);
        // [patch] the clip rectangle only limits the width; the fill icon itself has the vertical shape
        Gdiplus::RectF rc_clip(rc_indicater.X, static_cast<float>(icon_point.y), rc_indicater.Width, static_cast<float>(icon_size));
'''),
    ('''        //充电状态下的电量指示使用图标
        if (g_data.IsAcOnline())
''',
     '''        //充电状态下的电量指示使用图标
        if (is_full)    // [patch] 100 %: yellow, whether plugged in or not
        {
            ::DrawIconEx(pDC->GetSafeHdc(), icon_point.x, icon_point.y, g_data.GetIcon(IDI_FILL_LOW), icon_size, icon_size, 0, NULL, DI_NORMAL);
        }
        else if (g_data.IsAcOnline())
'''),
    ('''                rgn.CreateRectRgn(static_cast<int>(rc_indicater.GetLeft()), static_cast<int>(rc_indicater.GetTop())
                    , static_cast<int>(rc_indicater.GetRight()), static_cast<int>(rc_indicater.GetBottom()));
''',
     '''                rgn.CreateRectRgn(static_cast<int>(rc_clip.GetLeft()), static_cast<int>(rc_clip.GetTop())
                    , static_cast<int>(rc_clip.GetRight() + 0.5f), static_cast<int>(rc_clip.GetBottom()));
'''),
    ('''                    g_data.m_sysPowerStatus.BatteryLifePercent < 100 ? &rc_indicater : nullptr);      // blue''',
     '''                    g_data.m_sysPowerStatus.BatteryLifePercent < 100 ? &rc_clip : nullptr);      // blue'''),
])
print('battery stage 2 patches applied')
