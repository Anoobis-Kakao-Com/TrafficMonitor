# Generates the battery icons (taller body, lightning bolt centered inside) for every DPI size.
# Geometry is defined on a 16 px grid and scaled, so every size looks the same.
import struct, io, sys, os
from PIL import Image, ImageDraw
SIZES = [16, 20, 24, 28, 32, 48, 64]
SS = 16   # supersampling
COL = {'light': (255, 255, 255), 'dark': (64, 64, 64), 'green': (128, 194, 105), 'yellow': (241, 197, 0), 'red': (206, 23, 0)}
# fill area (also used by the plugin code: X=1, Y=4.2, W=11.7*percent, H=7.6)
FILL = (1.0, 4.2, 12.7, 11.8)
BOLT = [(0.55, 0.0), (0.05, 0.56), (0.40, 0.56), (0.30, 1.0), (0.95, 0.38), (0.58, 0.38), (0.80, 0.0)]
BOLT_BOX = (4.4, 4.6, 10.0, 11.4)    # x0, y0, x1, y1 on the 16 px grid (centered in the body)

def render(kind, color, size):
    n = size * SS
    k = n / 16.0
    im = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    def rr(box, r, fill):
        d.rounded_rectangle([box[0]*k, box[1]*k, box[2]*k - 1, box[3]*k - 1], radius=r*k, fill=fill)
    c = COL[color] + (255,)
    if kind == 'frame' or kind == 'frame_bolt':
        rr((0, 3, 15, 13), 1.5, c)                 # outer body
        rr((1, 4, 14, 12), 0.6, (0, 0, 0, 0))      # hollow
        # the hollow must be transparent: punch it out
        mask = Image.new('L', (n, n), 0)
        md = ImageDraw.Draw(mask)
        md.rounded_rectangle([1*k, 4*k, 14*k - 1, 12*k - 1], radius=0.6*k, fill=255)
        im.paste((0, 0, 0, 0), (0, 0), mask)
        rr((15, 6.5, 16, 9.5), 0.4, c)             # terminal
        if kind == 'frame_bolt':
            x0, y0, x1, y1 = BOLT_BOX
            pts = [((x0 + (x1 - x0) * px) * k, (y0 + (y1 - y0) * py) * k) for px, py in BOLT]
            d = ImageDraw.Draw(im)
            d.polygon(pts, fill=c)
    else:   # fill
        rr(FILL, 0.8, c)
    return im.resize((size, size), Image.BOX)

def write_ico(path, images):
    entries = []; blobs = []
    for im in images:
        b = io.BytesIO(); im.save(b, 'PNG'); blobs.append(b.getvalue())
    off = 6 + 16 * len(images)
    out = struct.pack('<HHH', 0, 1, len(images))
    for im, blob in zip(images, blobs):
        w = im.width
        out += struct.pack('<BBBBHHII', 0 if w >= 256 else w, 0 if w >= 256 else w, 0, 0, 1, 32, len(blob), off)
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
outdir = sys.argv[1] if len(sys.argv) > 1 else '.'
for name, (kind, color) in files.items():
    write_ico(os.path.join(outdir, name), [render(kind, color, s) for s in SIZES])
print('icons written to', outdir)
