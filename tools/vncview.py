#!/usr/bin/env python3
# @file      vncview.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# A test viewer for vncserver (devplan/vnc.md §7): RFB 3.8, None or VNC authentication (OpenSSL
# for its DES), Raw, ZRLE or Tight (JPEG with --quality) decoded into a picture (Pillow) and
# saved; optionally a line typed and a click at the screen's middle. Through a tunnel:
#
#   ssh -f -N -L 15900:localhost:5900 root@bmds0.local
#   tools/vncview.py --port 15900 --enc tight --quality 6 --updates 8 --out screen.png
import io, socket, struct, sys, time, zlib, argparse
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument('--host', default='127.0.0.1')
ap.add_argument('--port', type=int, default=5900)
ap.add_argument('--enc', choices=['raw', 'zrle', 'tight'], default='tight')
ap.add_argument('--quality', type=int, default=-1)
ap.add_argument('--out', default='vnc.png')
ap.add_argument('--password')
ap.add_argument('--type')
ap.add_argument('--updates', type=int, default=1)
ap.add_argument('--bpp16', action='store_true')
a = ap.parse_args()

s = socket.create_connection((a.host, a.port), timeout=30)
def rx(n):
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise SystemExit('closed after %d of %d bytes' % (len(b), n))
        b += c
    return b
def u8(): return rx(1)[0]
def u16(): return struct.unpack('>H', rx(2))[0]
def u32(): return struct.unpack('>I', rx(4))[0]

ver = rx(12); s.sendall(b'RFB 003.008\n')
types = rx(u8())
if 2 in types:
    s.sendall(b'\x02'); ch = rx(16)
    key = bytes(int('{:08b}'.format(c)[::-1], 2) for c in (a.password or '').encode()[:8].ljust(8, b'\0'))
    from subprocess import run
    resp = run(['openssl', 'enc', '-des-ecb', '-provider', 'legacy', '-provider', 'default', '-K', key.hex(), '-nopad'],
               input=ch, capture_output=True).stdout
    s.sendall(resp)
else:
    s.sendall(b'\x01')
res = u32()
if res != 0: raise SystemExit('auth failed: ' + rx(u32()).decode())
s.sendall(b'\x01')
W, H = u16(), u16(); pf = rx(16); name = rx(u32()).decode()
print('server', ver.strip().decode(), name, W, H)
bpp = 2 if a.bpp16 else 4
if a.bpp16:
    s.sendall(b'\x00\0\0\0' + struct.pack('>BBBBHHHBBB3x', 16, 16, 0, 1, 31, 63, 31, 11, 5, 0))
encs = {'raw': [0], 'zrle': [16], 'tight': [7]}[a.enc] + [-224, -223]
if a.quality >= 0: encs.append(-32 + a.quality)
s.sendall(struct.pack('>BxH', 2, len(encs)) + b''.join(struct.pack('>i', e) for e in encs))

fb = Image.new('RGB', (W, H))
def pix(b):  # a PIXEL (little-endian) to RGB
    v = int.from_bytes(b, 'little')
    if bpp == 2: return ((v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31)
    return (v >> 16 & 255, v >> 8 & 255, v & 255)
zrle = zlib.decompressobj(); tight = [zlib.decompressobj() for _ in range(4)]
def clen():
    b = u8(); n = b & 0x7f
    if b & 0x80:
        b = u8(); n |= (b & 0x7f) << 7
        if b & 0x80: n |= u8() << 14
    return n
stats = {}
def rect():
    global W, H, fb
    x, y, w, h, e = struct.unpack('>HHHHi', rx(12))
    stats[e] = stats.get(e, 0) + 1
    if e == -224: return False
    if e == -223:
        W, H = w, h; fb = Image.new('RGB', (W, H)); return True
    if e == 0:
        d = rx(w * h * bpp)
        for i in range(w * h): fb.putpixel((x + i % w, y + i // w), pix(d[i*bpp:(i+1)*bpp]))
    elif e == 16:
        d = zrle.decompress(rx(u32())); p = 0; cpx = 3 if bpp == 4 else 2
        def cp(i): return pix(d[i:i+cpx])
        for ty in range(0, h, 64):
            for tx in range(0, w, 64):
                tw, th = min(64, w - tx), min(64, h - ty); sub = d[p]; p += 1
                if sub == 1:
                    c = cp(p); p += cpx
                    for r in range(th):
                        for q in range(tw): fb.putpixel((x+tx+q, y+ty+r), c)
                elif sub == 0:
                    for r in range(th):
                        for q in range(tw): fb.putpixel((x+tx+q, y+ty+r), cp(p)); p += cpx
                else:
                    pal = [cp(p + i*cpx) for i in range(sub)]; p += sub * cpx
                    bits = 1 if sub <= 2 else 2 if sub <= 4 else 4
                    for r in range(th):
                        bit = 0
                        for q in range(tw):
                            idx = (d[p + bit // 8] >> (8 - bits - bit % 8)) & ((1 << bits) - 1)
                            fb.putpixel((x+tx+q, y+ty+r), pal[idx]); bit += bits
                        p += (bit + 7) // 8
    elif e == 7:
        c = u8()
        for i in range(4):
            if c >> i & 1: tight[i] = zlib.decompressobj()
        c &= 0xf0; tpx = 3 if bpp == 4 else 2
        def tp(b): return (b[0], b[1], b[2]) if tpx == 3 else pix(b)
        if c == 0x80:
            col = tp(rx(tpx)); fb.paste(col, (x, y, x + w, y + h))
        elif c == 0x90:
            img = Image.open(io.BytesIO(rx(clen()))).convert('RGB'); fb.paste(img, (x, y))
        else:
            st = c >> 4 & 3; pal = None
            if c & 0x40:
                assert u8() == 1; n = u8() + 1; pal = [tp(rx(tpx)) for _ in range(n)]
            rowb = (w + 7) // 8 if pal and len(pal) == 2 else w if pal else w * tpx
            want = rowb * h
            d = rx(want) if want < 12 else tight[st].decompress(rx(clen()))
            assert len(d) == want, (len(d), want)
            for r in range(h):
                for q in range(w):
                    if pal and len(pal) == 2: col = pal[d[r*rowb + q//8] >> (7 - q % 8) & 1]
                    elif pal: col = pal[d[r*rowb + q]]
                    else: col = tp(d[r*rowb + q*tpx: r*rowb + q*tpx + tpx])
                    fb.putpixel((x+q, y+r), col)
    else:
        raise SystemExit('encoding %d?' % e)
    return True

def update(incremental):
    s.sendall(struct.pack('>BBHHHH', 3, incremental, 0, 0, W, H))
    t0 = time.time(); total = 0
    while True:
        t = u8()
        if t == 0:
            rx(1); n = u16(); got = 0
            while got < n:
                if not rect(): break
                got += 1
            return time.time() - t0
        elif t == 2: pass
        elif t == 3: rx(3); rx(u32())
        else: raise SystemExit('message %d?' % t)

dt = update(0)
print('full update in %.2f s, rects by encoding %s' % (dt, stats))
if a.type:
    keys = [ord(ch) for ch in a.type] + [0xff0d]
    for k in keys: s.sendall(struct.pack('>BBxxI', 4, 1, k) + struct.pack('>BBxxI', 4, 0, k))
    s.sendall(struct.pack('>BBHH', 5, 0, W // 2, H // 2) + struct.pack('>BBHH', 5, 1, W//2, H//2) + struct.pack('>BBHH', 5, 0, W//2, H//2))
    time.sleep(1.0)
for i in range(a.updates - 1):
    stats.clear(); dt = update(1); print('incremental update in %.2f s, %s' % (dt, stats))
fb.save(a.out)
s.close()
