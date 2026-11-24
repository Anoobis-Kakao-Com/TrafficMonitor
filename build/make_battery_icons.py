# Generates the battery icons (35 px wide body, taller than the original, lightning bolt centered inside, fill that
# covers the whole inside) for every DPI size. Geometry is defined on a 37 x 16 px grid and scaled.
# The same numbers are used by the plugin code (BatteryItem.cpp / WirelessBattery.cpp):
#   icon 37 x 16 px, body x 0..35, y 3..13, inside x 1..34, y 4..12, terminal x 35..37
import struct, io, sys, os
from PIL import Image, ImageDraw
GW, GH = 37, 16                      # grid size of the icon
SCALES = [1.0, 1.25, 1.5, 1.75, 2.0, 3.0]
SS = 12                              # supersampling
COL = {'light': (255, 255, 255), 'dark': (64, 64, 64), 'green': (128, 194, 105), 'yellow': (241, 197, 0), 'red': (206, 23, 0)}
INSIDE = (1.0, 4.0, 34.0, 12.0)      # fill area = whole inside of the body
BOLT = [(0.55, 0.0), (0.05, 0.56), (0.40, 0.56), (0.30, 1.0), (0.95, 0.38), (0.58, 0.38), (0.80, 0.0)]
BOLT_BOX = (14.7, 4.6, 20.3, 11.4)   # centered in the body (x center 17.5, y center 8)

def render(kind, color, w, h):
    kx, ky = w * SS / GW, h * SS / GH
    im = Image.new('RGBA', (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = COL[color] + (255,)
    def rr(box, r, fill, target=None):
        dd = target or d
        dd.rounded_rectangle([box[0]*kx, box[1]*ky, box[2]*kx - 1, box[3]*ky - 1], radius=r*ky, fill=fill)
    if kind in ('frame', 'frame_bolt'):
        rr((0, 3, 35, 13), 1.8, c)
        mask = Image.new('L', im.size, 0)
        md = ImageDraw.Draw(mask)
        md.rounded_rectangle([1*kx, 4*ky, 34*kx - 1, 12*ky - 1], radius=0.8*ky, fill=255)
        im.paste((0, 0, 0, 0), (0, 0), mask)
        d = ImageDraw.Draw(im)
        rr((35, 6.2, 37, 9.8), 0.5, c, d)
        if kind == 'frame_bolt':
            x0, y0, x1, y1 = BOLT_BOX
            pts = [((x0 + (x1 - x0) * px) * kx, (y0 + (y1 - y0) * py) * ky) for px, py in BOLT]
            d.polygon(pts, fill=c)
    else:
        rr(INSIDE, 0.6, c)
    return im.resize((w, h), Image.BOX)

def write_ico(path, images):
    blobs = []
    for im in images:
        b = io.BytesIO(); im.save(b, 'PNG'); blobs.append(b.getvalue())
    off = 6 + 16 * len(images)
    out = struct.pack('<HHH', 0, 1, len(images))
    for im, blob in zip(images, blobs):
        out += struct.pack('<BBBBHHII', im.width if im.width < 256 else 0, im.height if im.height < 256 else 0, 0, 0, 1, 32, len(blob), off)
        off += len(blob)
    open(path, 'wb').write(out + b''.join(blobs))

files = {
    'battery_dark.ico':         ('frame', 'dark'),
    'battery_light.ico':        ('frame', 'light'),
    'battery_dark_charge.ico':  ('frame_bolt', 'dark'),
    'battery_light_charge.ico': ('frame_bolt', 'light'),
    'fill_high.ico':            ('fill', 'green'),
    'fill_low.ico':             ('fill', 'yellow'),
    'fill_critical.ico':        ('fill', 'red'),
}
if __name__ == '__main__':
    outdir = sys.argv[1] if len(sys.argv) > 1 else '.'
    for name, (kind, color) in files.items():
        write_ico(os.path.join(outdir, name), [render(kind, color, round(GW * s), round(GH * s)) for s in SCALES])
    print('icons written to', outdir)
