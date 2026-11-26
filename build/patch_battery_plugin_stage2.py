# Stage 2 of the Battery plugin patch (runs after patch_battery_plugin.py in the same folder):
# the battery is no longer an icon image but drawn by code (BatteryShape.h): the height of the body is the em height of
# the host font (= the height of the boxes of the keyboard indicator), the body is 15/8 as long as it is high (30 x 16 px
# at the default font), the bolt scales with it. The fill is drawn BEFORE the frame so that the bolt stays visible.
# Colors: plugged in + charging = green, plugged in + not charging = blue, on battery = red, 100 % = yellow (no bolt).
import os, shutil

def load(path):
    raw = open(path, 'rb').read()
    text = raw.decode('utf-8-sig')
    return raw.startswith(b'\xef\xbb\xbf'), ('\r\n' in text), text.replace('\r\n', '\n')

def save(path, bom, crlf, text):
    if crlf:
        text = text.replace('\n', '\r\n')
    out = text.encode('utf-8')
    if bom:
        out = b'\xef\xbb\xbf' + out
    open(path, 'wb').write(out)

def patch(path, edits):
    bom, crlf, text = load(path)
    for old, new in edits:
        n = text.count(old)
        if n != 1:
            raise SystemExit('PATCH FAILED: %r found %d times in %s' % (old[:70], n, path))
        text = text.replace(old, new)
    save(path, bom, crlf, text)

B = 'plugins_src/Plugins/Battery/'
shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'shared', 'BatteryShape.h'), B + 'BatteryShape.h')

# ---------------------------------------------------------------- BatteryItem.cpp: width + drawing
bom, crlf, text = load(B + 'BatteryItem.cpp')
start = text.index('int CBatteryItem::GetItemWidthEx(void * hDC) const\n{')
head = text[:start]
assert head.count('#include "GdiPlusTool.h"\n') == 1
head = head.replace('#include "GdiPlusTool.h"\n', '#include "GdiPlusTool.h"\n#include "BatteryShape.h"\n')
new_tail = r'''// [patch] Size of the battery: the height of the body is the em height of the host font (= the height of the boxes of the
// keyboard indicator), see BatteryShape.h. The width that was used last is needed by TMHostTextIconWidth().
int g_last_icon_w = 0;

static int HostEmHeight(HDC dc)
{
    return BatteryHostEmHeight(dc, g_data.DPI(8), g_data.DPI(80), g_data.DPI(14));
}

int CBatteryItem::GetItemWidthEx(void * hDC) const
{
    CDC* pDC = CDC::FromHandle((HDC)hDC);

    CString sample_str;
    if (g_data.m_setting_data.show_percent)
        sample_str = _T("100%");
    else
        sample_str = _T("100");

    BatteryShape shape = MakeBatteryShape(HostEmHeight((HDC)hDC));
    g_last_icon_w = shape.total_w();
    switch (g_data.m_setting_data.battery_type)
    {
    case BatteryType::NUMBER:
        return pDC->GetTextExtent(sample_str).cx;
    case BatteryType::ICON:
        return shape.total_w() + g_data.DPI(4);
    case BatteryType::NUMBER_BESIDE_ICON:
        return shape.total_w() + g_data.DPI(4) + pDC->GetTextExtent(sample_str).cx;
    }
    return shape.total_w() + g_data.DPI(4) + pDC->GetTextExtent(sample_str).cx;
}

void CBatteryItem::DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode)
{
    //绘图句柄
    CDC* pDC = CDC::FromHandle((HDC)hDC);
    //矩形区域
    CRect rect(CPoint(x, y), CSize(w, h));

    // [patch] as high as the letters (em height of the host font), never higher than the row, exactly centered
    int box_h = HostEmHeight(pDC->GetSafeHdc());
    if (box_h > rect.Height())
        box_h = rect.Height();
    if (((rect.Height() - box_h) & 1) != 0)
        box_h += (box_h < rect.Height() ? 1 : -1);
    BatteryShape shape = MakeBatteryShape(box_h);
    g_last_icon_w = shape.total_w();
    const int icon_w = shape.total_w();

    // the host draws the number itself when it asked for it right before this call (one-shot)
    bool host_draws_text = g_host_draws_text && g_data.m_setting_data.battery_type == BatteryType::NUMBER_BESIDE_ICON;
    g_host_draws_text = false;
    // icon + number are one block that is centered in the given width (icon only when the host draws the number)
    int content_w = icon_w;
    if (!host_draws_text && g_data.m_setting_data.battery_type == BatteryType::NUMBER_BESIDE_ICON)
    {
        std::wstring size_str{ g_data.GetBatteryString() };
        content_w += g_data.DPI(2) + pDC->GetTextExtent(size_str.c_str(), static_cast<int>(size_str.size())).cx;
    }
    int block_offset = host_draws_text ? 0 : (rect.Width() - content_w) / 2;
    if (block_offset < 0)
        block_offset = 0;
    CPoint icon_point{ rect.left + block_offset, rect.top + (rect.Height() - shape.body_h) / 2 };

    Gdiplus::Graphics graphics(pDC->GetSafeHdc());
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    const int level = g_data.m_sysPowerStatus.BatteryLifePercent;
    const bool has_battery = g_data.m_sysPowerStatus.BatteryFlag != 128;
    const bool full = has_battery && level >= 100;
    const bool ac = g_data.IsAcOnline();
    const bool charging = (g_data.m_sysPowerStatus.BatteryFlag & 8) != 0;

    //填充电量指示 (the fill first, the frame and the bolt on top of it)
    if (has_battery && level > 0 && level <= 100)
    {
        double percent = level;
        //显示充电动画
        if (g_data.m_setting_data.show_charging_animation && ac && charging && level < 100)
            percent = level + (battery_percent / 100 * (100 - level));
        COLORREF fill_color;
        if (full)
            fill_color = RGB(241, 197, 0);                          // 100 %: yellow
        else if (ac)
            fill_color = charging ? RGB(128, 194, 105)              // plugged in + charging: green
                                  : RGB(30, 144, 255);              // plugged in, not charging: blue
        else
            fill_color = BATTERY_COLOR_CRITICAL;                    // on battery: red
        DrawBatteryFill(graphics, icon_point.x, icon_point.y, shape, fill_color, percent);
    }
    const COLORREF frame_color = dark_mode ? RGB(255, 255, 255) : RGB(64, 64, 64);
    DrawBatteryFrame(graphics, icon_point.x, icon_point.y, shape, frame_color);
    if (ac && !full)
        DrawBatteryBolt(graphics, icon_point.x, icon_point.y, shape, frame_color);
    graphics.Flush(Gdiplus::FlushIntentionSync);

    //绘制电池数值
    if (g_data.m_setting_data.battery_type == BatteryType::NUMBER_BESIDE_ICON)
    {
        CRect rc_text{ rect };
        rc_text.left = icon_point.x + icon_w + g_data.DPI(2);
        std::wstring battery_str{ g_data.GetBatteryString() };
        if (!host_draws_text)
            pDC->DrawText(battery_str.c_str(), rc_text, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}
'''
save(B + 'BatteryItem.cpp', bom, crlf, head + new_tail)

# ---------------------------------------------------------------- Battery.cpp: width of the icon block for the host
patch(B + 'Battery.cpp', [
    ('#include "Battery.h"\n', '#include "Battery.h"\nextern int g_last_icon_w;          // [patch] width of the battery that was drawn last (BatteryItem.cpp)\n'),
    ('    return g_data.DPI(16) + g_data.DPI(2);\n',
     '    return (g_last_icon_w > 0 ? g_last_icon_w : g_data.DPI(32)) + g_data.DPI(2);\n'),
])
print('battery stage 2 patches applied')
