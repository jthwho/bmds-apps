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
ap.add_argument('--compress', type=int, default=-1, help='CompressLevel 0-9 (none: the server picks)')
ap.add_argument('--out', default='vnc.png')
ap.add_argument('--password')
ap.add_argument('--type')
ap.add_argument('--wiggle', action='store_true', help='with --seconds: a pointer move before each update')
ap.add_argument('--point', help='X,Y: move the pointer there after the first update (no click)')
ap.add_argument('--type-delay', type=float, default=0, help='seconds of updates before typing')
ap.add_argument('--updates', type=int, default=1)
ap.add_argument('--bpp16', action='store_true')
ap.add_argument('--skip', action='store_true', help='parse updates without decoding them (speed tests)')
ap.add_argument('--seconds', type=float, default=0, help='then ask for updates this long and report the rate')
ap.add_argument('--cut', help='text to copy (ClientCutText, Latin-1) after the first update')
ap.add_argument('--cursor', action='store_true',
                help='the Cursor pseudo-encoding: the viewer draws the pointer (its shapes printed)')
ap.add_argument('--ext-clip', action='store_true',
                help='the Extended Clipboard (UTF-8): --cut is announced, sent when asked for')
ap.add_argument('--watch-cut', type=float, default=0, help='then print the server\'s cut texts this long')
a = ap.parse_args()

s = socket.create_connection((a.host, a.port), timeout=30)
received = [0]
def rx(n):
    b = bytearray()
    while len(b) < n:
        c = s.recv(min(n - len(b), 1 << 20))
        if not c: raise SystemExit('closed after %d of %d bytes' % (len(b), n))
        b += c
    received[0] += n
    return bytes(b)
def u8(): return rx(1)[0]
def u16(): return struct.unpack('>H', rx(2))[0]
def u32(): return struct.unpack('>I', rx(4))[0]

def vnc_auth():
    ch = rx(16)
    key = bytes(int('{:08b}'.format(c)[::-1], 2) for c in (a.password or '').encode()[:8].ljust(8, b'\0'))
    from subprocess import run
    resp = run(['openssl', 'enc', '-des-ecb', '-provider', 'legacy', '-provider', 'default', '-K', key.hex(), '-nopad'],
               input=ch, capture_output=True).stdout
    s.sendall(resp)

ver = rx(12); s.sendall(b'RFB 003.008\n')
types = rx(u8())
if 19 in types:
    # VeNCrypt 0.2, X509Vnc (261): TLS (the server's certificate shown, not checked), then VNC's.
    import ssl, hashlib
    s.sendall(b'\x13'); vv = rx(2)
    if vv != b'\x00\x02': raise SystemExit('VeNCrypt %r?' % vv)
    s.sendall(b'\x00\x02')
    if u8() != 0: raise SystemExit('VeNCrypt version refused')
    subtypes = [u32() for _ in range(u8())]
    if 261 not in subtypes: raise SystemExit('VeNCrypt subtypes %r' % subtypes)
    s.sendall(struct.pack('>I', 261))
    if u8() != 1: raise SystemExit('X509Vnc refused')
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
    t0 = time.time(); s = ctx.wrap_socket(s); dt = time.time() - t0
    der = s.getpeercert(binary_form=True)
    print('tls: %s %s in %.2f s, the server\'s certificate sha256 %s' % (s.version(), s.cipher()[0], dt,
          ':'.join('%02X' % b for b in hashlib.sha256(der).digest())), flush=True)
    vnc_auth()
elif 2 in types:
    s.sendall(b'\x02'); vnc_auth()
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
if a.compress >= 0: encs.append(-256 + a.compress)
if a.ext_clip: encs.append(-1063131698)  # 0xC0A1E5CE
if a.cursor: encs.append(-239)
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
def skip_rect(x, y, w, h, e):
    tpx = 3 if bpp == 4 else 2
    if e == 0: rx(w * h * bpp)
    elif e == 16: rx(u32())
    elif e == 7:
        c = u8() & 0xf0
        if c == 0x80: rx(tpx)
        elif c == 0x90: rx(clen())
        else:
            pal = 0
            if c & 0x40:
                rx(1); pal = u8() + 1; rx(pal * tpx)
            rowb = (w + 7) // 8 if pal == 2 else w if pal else w * tpx
            want = rowb * h
            rx(want) if want < 12 else rx(clen())
    else: raise SystemExit('encoding %d?' % e)

def rect():
    global W, H, fb
    x, y, w, h, e = struct.unpack('>HHHHi', rx(12))
    stats[e] = stats.get(e, 0) + 1
    if e == -239:
        d = rx(w * h * bpp); m = rx((w + 7) // 8 * h)
        shown = sum(bin(b).count('1') for b in m)
        print('cursor %dx%d hot %d,%d: %d pixels shown' % (w, h, x, y, shown), flush=True)
        return True
    if a.skip and e not in (-224, -223):
        skip_rect(x, y, w, h, e); return True
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

XTEXT, XCAPS, XREQUEST, XPEEK, XNOTIFY, XPROVIDE = 1, 1 << 24, 1 << 25, 1 << 26, 1 << 27, 1 << 28
pending_cut = [None]  # --ext-clip: the text to provide when asked

def ext_send(flags, data=b''):
    s.sendall(struct.pack('>Bxxxi', 6, -(4 + len(data))) + struct.pack('>I', flags) + data)

def ext_provide(text):
    body = text.replace('\r\n', '\n').replace('\n', '\r\n').encode() + b'\0'
    z = zlib.compressobj()  # flushed, not ended: as TigerVNC does
    ext_send(XPROVIDE | XTEXT, z.compress(struct.pack('>I', len(body)) + body) + z.flush(zlib.Z_SYNC_FLUSH))

def ext_message(payload):
    flags = struct.unpack('>I', payload[:4])[0]; rest = payload[4:]
    if flags & XCAPS:
        print('extended clipboard caps from the server: %08x, sizes %s' %
              (flags, [struct.unpack('>I', rest[i:i+4])[0] for i in range(0, len(rest), 4)]), flush=True)
        ext_send(XCAPS | XREQUEST | XNOTIFY | XPROVIDE | XTEXT, struct.pack('>I', 10 << 20))
    elif flags & XNOTIFY:
        print('extended clipboard notify from the server: %08x' % flags, flush=True)
        if flags & XTEXT: ext_send(XREQUEST | XTEXT)
    elif flags & XREQUEST:
        print('extended clipboard request from the server: %08x' % flags, flush=True)
        if pending_cut[0] is not None: ext_provide(pending_cut[0])
    elif flags & XPROVIDE:
        data = zlib.decompressobj().decompress(rest)
        n = struct.unpack('>I', data[:4])[0]
        print('extended cut text from the server: %r' % data[4:4+n].decode(errors='replace'), flush=True)
    else:
        print('extended clipboard message %08x?' % flags, flush=True)

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
        elif t == 3:
            rx(3); n = struct.unpack('>i', rx(4))[0]
            if n < 0: ext_message(rx(-n)); continue
            text = rx(n).decode('latin-1')
            print('cut text from the server: %r' % text, flush=True)
        else: raise SystemExit('message %d?' % t)

dt = update(0)
print('full update in %.2f s, rects by encoding %s' % (dt, stats))
if a.cut is not None and a.ext_clip:
    pending_cut[0] = a.cut; ext_send(XNOTIFY | XTEXT)  # the server asks for it (use --watch-cut)
elif a.cut is not None:
    data = a.cut.encode('latin-1')
    s.sendall(struct.pack('>BxxxI', 6, len(data)) + data)
if a.point:
    px, py = (int(v) for v in a.point.split(','))
    s.sendall(struct.pack('>BBHH', 5, 0, px, py)); time.sleep(0.5)
if a.type:
    end = time.time() + a.type_delay
    while time.time() < end: update(1)
    keys = [ord(ch) for ch in a.type] + [0xff0d]
    for k in keys: s.sendall(struct.pack('>BBxxI', 4, 1, k) + struct.pack('>BBxxI', 4, 0, k))
    s.sendall(struct.pack('>BBHH', 5, 0, W // 2, H // 2) + struct.pack('>BBHH', 5, 1, W//2, H//2) + struct.pack('>BBHH', 5, 0, W//2, H//2))
    time.sleep(1.0)
for i in range(a.updates - 1):
    stats.clear(); dt = update(1); print('incremental update in %.2f s, %s' % (dt, stats))
if a.seconds > 0:
    received[0] = 0; n = 0; t0 = time.time()
    while time.time() - t0 < a.seconds:
        if a.wiggle: s.sendall(struct.pack('>BBHH', 5, 0, 200 + n % 800, 200 + (n * 7) % 400))
        update(1); n += 1
    dt = time.time() - t0
    print('%d updates in %.1f s: %.1f a second, %.2f MB/s' % (n, dt, n / dt, received[0] / dt / 1e6))
if a.watch_cut > 0:
    end = time.time() + a.watch_cut
    s.settimeout(0.5)
    while time.time() < end:
        try: update(1)
        except socket.timeout: pass
    s.settimeout(None)
if not a.skip: fb.save(a.out)
s.close()
