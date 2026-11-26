// BatteryShape.h - draws the battery of the Battery plugin and of the Bluetooth battery plugin (vector, any size).
// The size follows the font of the taskbar: the height of the battery body is the em height of the host font, i.e.
// exactly the height of the boxes of the keyboard indicator (so it also follows the font size, the number of rows and
// the DPI). The body is 15/8 as long as it is high (30 x 16 px at the default font), the bolt and the glyphs inside
// are scaled with it. MIT License (derived from the Battery plugin by zhongyang219), may be modified freely.
#pragma once
#include <cmath>

struct BatteryShape {
    int body_w = 30, body_h = 16;     // outer size of the body (pixels)
    int border = 1;                   // thickness of the frame
    int nub_w = 2, nub_h = 6;         // terminal
    int total_w() const { return body_w + nub_w; }
    int inner_w() const { return body_w - 2 * border; }
    int inner_h() const { return body_h - 2 * border; }
};

inline BatteryShape MakeBatteryShape(int body_h)
{
    BatteryShape s;
    if (body_h < 8) body_h = 8;
    s.body_h = body_h;
    s.body_w = (body_h * 15 + 4) / 8;                      // 16 -> 30
    s.border = (body_h * 2 + 11) / 22;                     // 1 px up to 16 px, 2 px from 17 px
    if (s.border < 1) s.border = 1;
    s.nub_w = (body_h * 14 + 50) / 100;                    // 2 px at 16
    if (s.nub_w < 2) s.nub_w = 2;
    s.nub_h = (body_h * 38 + 50) / 100;                    // 6 px at 16
    if (((body_h - s.nub_h) & 1) != 0) ++s.nub_h;          // terminal exactly centered
    return s;
}

inline Gdiplus::Color BatteryColor(COLORREF c, int alpha = 255)
{
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

inline void BatteryRoundRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r)
{
    float d = r * 2;
    if (d > w) d = w;
    if (d > h) d = h;
    if (d < 0.5f) { path.AddRectangle(Gdiplus::RectF(x, y, w, h)); return; }
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

// fill of the inside of the body, "percent" of its length (0 - 100)
inline void DrawBatteryFill(Gdiplus::Graphics& g, int x, int y, const BatteryShape& s, COLORREF color, double percent)
{
    if (percent <= 0) return;
    if (percent > 100) percent = 100;
    int fill_w = static_cast<int>(std::floor(s.inner_w() * percent / 100.0 + 0.5));
    if (fill_w < 1) fill_w = 1;
    Gdiplus::SolidBrush brush(BatteryColor(color));
    g.FillRectangle(&brush, x + s.border, y + s.border, fill_w, s.inner_h());
}

// frame (body outline + terminal), drawn after the fill
inline void DrawBatteryFrame(Gdiplus::Graphics& g, int x, int y, const BatteryShape& s, COLORREF color)
{
    Gdiplus::SolidBrush brush(BatteryColor(color));
    float ro = s.body_h * 0.12f;
    if (ro < 1.0f) ro = 1.0f;
    float ri = ro - s.border;
    if (ri < 0) ri = 0;
    Gdiplus::GraphicsPath ring;
    ring.SetFillMode(Gdiplus::FillModeAlternate);
    BatteryRoundRect(ring, (float)x, (float)y, (float)s.body_w, (float)s.body_h, ro);
    BatteryRoundRect(ring, (float)(x + s.border), (float)(y + s.border), (float)s.inner_w(), (float)s.inner_h(), ri);
    g.FillPath(&brush, &ring);
    Gdiplus::GraphicsPath nub;
    BatteryRoundRect(nub, (float)(x + s.body_w), (float)(y + (s.body_h - s.nub_h) / 2), (float)s.nub_w, (float)s.nub_h, 0.8f);
    g.FillPath(&brush, &nub);
}

// lightning bolt in the middle of the body
inline void DrawBatteryBolt(Gdiplus::Graphics& g, int x, int y, const BatteryShape& s, COLORREF color)
{
    static const float kBolt[7][2] = { { 0.55f, 0.0f }, { 0.05f, 0.56f }, { 0.40f, 0.56f }, { 0.30f, 1.0f }, { 0.95f, 0.38f }, { 0.58f, 0.38f }, { 0.80f, 0.0f } };
    const float bh = s.body_h * 0.64f;
    const float bw = bh * 0.82f;
    const float x0 = x + s.body_w / 2.0f - bw / 2;
    const float y0 = y + s.body_h / 2.0f - bh / 2;
    Gdiplus::PointF pts[7];
    for (int i = 0; i < 7; ++i) pts[i] = Gdiplus::PointF(x0 + bw * kBolt[i][0], y0 + bh * kBolt[i][1]);
    Gdiplus::SolidBrush brush(BatteryColor(color));
    g.FillPolygon(&brush, pts, 7);
}

// height of the box = em height of the font that is selected into the DC (like the boxes of the keyboard indicator)
inline int BatteryHostEmHeight(HDC dc, int dpi_min8, int dpi_max80, int dpi_default14)
{
    TEXTMETRICW tm = {};
    int h = 0;
    if (GetTextMetricsW(dc, &tm)) h = tm.tmHeight - tm.tmInternalLeading;
    if (h <= 0) h = dpi_default14;
    if (h < dpi_min8) h = dpi_min8;
    if (h > dpi_max80) h = dpi_max80;
    return h;
}
