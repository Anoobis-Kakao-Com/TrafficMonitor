def patch(path, edits):
    raw = open(path, 'rb').read()
    bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig')
    crlf = '\r\n' in text
    text = text.replace('\r\n', '\n')
    for old, new in edits:
        n = text.count(old)
        if n != 1:
            raise SystemExit('PATCH FAILED: %r found %d times in %s' % (old[:50], n, path))
        text = text.replace(old, new)
    if crlf:
        text = text.replace('\n', '\r\n')
    out = text.encode('utf-8')
    if bom:
        out = b'\xef\xbb\xbf' + out
    open(path, 'wb').write(out)

# Battery plugin (TrafficMonitorPlugins)
#  - no space between number and %
#  - cooperation with the patched TrafficMonitor: with "number beside icon" the plugin draws ONLY the icon and the
#    host draws the number exactly like every other text (same font, rendering and vertical position)
#  - without the patched host the plugin draws icon + number itself, as one block centered in the given width
patch('plugins_src/Plugins/Battery/DataManager.cpp', [
    ("""            if (m_sysPowerStatus.BatteryLifePercent < 100)
                str.push_back(L' ');
            str.push_back(L'%');""", """            str.push_back(L'%');"""),
])
patch('plugins_src/Plugins/Battery/BatteryItem.cpp', [
    ("static double battery_percent = 0.0;",
     "bool g_host_draws_text = false;      // [patch] one-shot request of the host: draw only the icon\nstatic double battery_percent = 0.0;"),
    ("rc_text.left = rect.left + icon_size + g_data.DPI(4);",
     "rc_text.left = icon_point.x + icon_size + g_data.DPI(2);"),
    ("pDC->DrawText(battery_str.c_str(), rc_text, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);",
     "if (!host_draws_text)\n            pDC->DrawText(battery_str.c_str(), rc_text, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);"),
    ("""    CPoint icon_point{ rect.TopLeft() };
    icon_point.x = rect.left + g_data.DPI(2);""", """    // [patch] the host draws the number itself when it asked for it right before this call (one-shot)
    bool host_draws_text = g_host_draws_text && g_data.m_setting_data.battery_type == BatteryType::NUMBER_BESIDE_ICON;
    g_host_draws_text = false;
    // icon + number are one block that is centered in the given width (icon only when the host draws the number)
    int content_w = icon_size;
    if (!host_draws_text && g_data.m_setting_data.battery_type == BatteryType::NUMBER_BESIDE_ICON)
    {
        std::wstring size_str{ g_data.GetBatteryString() };
        content_w += g_data.DPI(2) + pDC->GetTextExtent(size_str.c_str(), static_cast<int>(size_str.size())).cx;
    }
    int block_offset = host_draws_text ? 0 : (rect.Width() - content_w) / 2;
    if (block_offset < 0)
        block_offset = 0;
    CPoint icon_point{ rect.TopLeft() };
    icon_point.x = rect.left + block_offset;"""),
])
patch('plugins_src/Plugins/Battery/Battery.cpp', [
    ("void CBattery::OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data)",
     "extern bool g_host_draws_text;\n\nvoid CBattery::OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data)"),
    ("    case ITMPlugin::EI_CONFIG_DIR:",
     "    case static_cast<ITMPlugin::ExtendedInfoIndex>(900):     // [patch] the host will draw the number\n        g_host_draws_text = (data != nullptr && wcscmp(data, L\"1\") == 0);\n        break;\n    case ITMPlugin::EI_CONFIG_DIR:"),
    ("ITMPlugin* TMPluginGetInstance()",
     "// [patch] Cooperation with the patched TrafficMonitor: with the number beside the icon the host draws the\n// number itself. Returns the width of the icon block (icon + gap), 0 if the host has nothing to do.\nextern \"C\" __declspec(dllexport) int TMHostTextIconWidth()\n{\n    AFX_MANAGE_STATE(AfxGetStaticModuleState());\n    if (g_data.m_setting_data.battery_type != BatteryType::NUMBER_BESIDE_ICON)\n        return 0;\n    return g_data.DPI(16) + g_data.DPI(2);\n}\n\nITMPlugin* TMPluginGetInstance()"),
])
# Icon colors by power state: discharging = red, charging = green, plugged in but not charging (full or charge
# limit reached) = blue. The level bars keep their shape (the plugged-in bar has a notch for the lightning bolt).
patch('plugins_src/Plugins/Battery/BatteryItem.cpp', [
    ("void CBatteryItem::DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode)", r"""// [patch] Draws an icon with all its pixels in one color (shape and alpha of the icon are kept), optionally
// clipped to a rectangle. Used for the blue level bar (plugged in, not charging).
static void DrawTintedIcon(HDC hdc, HICON hIcon, int x, int y, int size, COLORREF color, const Gdiplus::RectF* clip)
{
    Gdiplus::Bitmap bmp(hIcon);
    if (bmp.GetLastStatus() != Gdiplus::Ok)
        return;
    const float r = GetRValue(color) / 255.0f;
    const float g = GetGValue(color) / 255.0f;
    const float b = GetBValue(color) / 255.0f;
    Gdiplus::ColorMatrix cm = {{ { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 }, { 0, 0, 0, 1, 0 }, { r, g, b, 0, 1 } }};
    Gdiplus::ImageAttributes attr;
    attr.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
    Gdiplus::Graphics graphics(hdc);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    if (clip != nullptr)
        graphics.SetClip(*clip);
    graphics.DrawImage(&bmp, Gdiplus::Rect(x, y, size, size), 0, 0, static_cast<INT>(bmp.GetWidth()), static_cast<INT>(bmp.GetHeight()), Gdiplus::UnitPixel, &attr);
}

void CBatteryItem::DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode)"""),
    ("HICON hFill;", "HICON hFill;\n            bool plugged_idle = (g_data.m_sysPowerStatus.BatteryFlag & 8) == 0;      // plugged in but not charging"),
    ("hFill = g_data.GetIcon(IDI_FILL_CRITICAL);", "hFill = g_data.GetIcon(IDI_FILL_HIGH);"),
    ("hFill = g_data.GetIcon(IDI_FILL_LOW);", "hFill = g_data.GetIcon(IDI_FILL_HIGH);"),
    ("::DrawIconEx(pDC->GetSafeHdc(), icon_point.x, icon_point.y, hFill, icon_size, icon_size, 0, NULL, DI_NORMAL);", r"""if (plugged_idle)
                DrawTintedIcon(pDC->GetSafeHdc(), hFill, icon_point.x, icon_point.y, icon_size, RGB(30, 144, 255),
                    g_data.m_sysPowerStatus.BatteryLifePercent < 100 ? &rc_indicater : nullptr);      // blue
            else
                ::DrawIconEx(pDC->GetSafeHdc(), icon_point.x, icon_point.y, hFill, icon_size, icon_size, 0, NULL, DI_NORMAL);      // green"""),
    ("CGdiPlusTool::COLORREFToGdiplusColor(g_data.GetBatteryColor())", "CGdiPlusTool::COLORREFToGdiplusColor(BATTERY_COLOR_CRITICAL)"),      # discharging: red
    # the "charging" animation only while it is really charging (not when plugged in with a full / limited battery)
    ("g_data.m_setting_data.show_charging_animation && g_data.IsAcOnline() && g_data.m_sysPowerStatus.BatteryLifePercent < 100",
     "g_data.m_setting_data.show_charging_animation && g_data.IsAcOnline() && (g_data.m_sysPowerStatus.BatteryFlag & 8) != 0 && g_data.m_sysPowerStatus.BatteryLifePercent < 100"),
])
print('battery color patches applied')
print('battery patches applied')
