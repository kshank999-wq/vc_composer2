import json, sys
OUT = sys.argv[1]

# ---------------- geometry ----------------
CLEF_W = 46; BAR_W = 120; NB = 8; LANE_W = CLEF_W + BAR_W * NB  # 1006
HDR_W = 220
SP = 7; HS = 3.5
LET = 'CDEFGAB'
PLAY_X = None

def pidx(n):
    return int(n[-1]) * 7 + LET.index(n[0])

def xpos(b, s):
    return CLEF_W + b * BAR_W + 16 + s * (BAR_W - 28) / 4.0

PLAY_X = round(xpos(3, 2 + 941 / 960.0), 1)

def f(v):
    return ('%.1f' % v).rstrip('0').rstrip('.')

class Staff:
    def __init__(self, top, ref, clef):
        self.top = top; self.bot = top + 4 * SP; self.ref = ref; self.clef = clef
    def y(self, i):
        return self.bot - (i - self.ref) * HS
    @property
    def mid(self):
        return self.ref + 4

# ---------------- style helpers ----------------
SILK = "font-family:'Barlow Condensed',sans-serif;font-weight:600;font-size:11px;letter-spacing:.14em;text-transform:uppercase;color:#8E98A3"
PANEL = "background:linear-gradient(180deg,#23272c 0%,#1a1d21 100%);border:1px solid #08090a;border-radius:8px;box-shadow:inset 0 1px 0 rgba(255,255,255,.07),0 2px 10px rgba(0,0,0,.55)"
LCD = "background:#06100d;border-radius:4px;box-shadow:inset 0 2px 8px rgba(0,0,0,.95),inset 0 0 0 1px #000,0 1px 0 rgba(255,255,255,.08);color:var(--lcd);font-family:'Share Tech Mono',monospace;text-shadow:0 0 6px var(--glow)"
KEYBASE = ("appearance:none;display:inline-flex;align-items:center;justify-content:center;gap:5px;box-sizing:border-box;"
           "border-radius:5px;border:1px solid #08090a;cursor:pointer;"
           "font-family:'Barlow Condensed',sans-serif;font-weight:600;font-size:12px;letter-spacing:.06em;text-transform:uppercase;")
KEY_OFF = "background:linear-gradient(180deg,#3c4148 0%,#2a2e34 100%);box-shadow:inset 0 1px 0 rgba(255,255,255,.15),0 2px 0 #08090a,0 3px 6px rgba(0,0,0,.45);color:#D3D9E0;"
KEY_ON = "background:linear-gradient(180deg,#4a4f57 0%,#363b42 100%);box-shadow:inset 0 1px 0 rgba(255,255,255,.2),inset 0 0 0 1px var(--led),0 1px 0 #08090a,0 0 10px var(--ledglow);color:#FFFFFF;"

def led(on, color='var(--led)', glow='var(--ledglow)', size=6):
    if on:
        return ('<span aria-hidden="true" style="display:inline-block;flex:0 0 auto;width:%dpx;height:%dpx;border-radius:50%%;background:%s;box-shadow:0 0 6px %s,inset 0 0 1px rgba(255,255,255,.7)"></span>' % (size, size, color, glow))
    return ('<span aria-hidden="true" style="display:inline-block;flex:0 0 auto;width:%dpx;height:%dpx;border-radius:50%%;background:#2b2e32;box-shadow:inset 0 1px 1px rgba(0,0,0,.7)"></span>' % (size, size))

def key(label, on=False, w=None, h=24, aria=None, extra='', ledon=None, pad='0 8px', fs=None):
    st = KEYBASE + (KEY_ON if on else KEY_OFF) + 'height:%dpx;padding:%s;' % (h, pad)
    if w: st += 'width:%dpx;' % w
    if fs: st += 'font-size:%dpx;' % fs
    st += extra
    a = ' aria-label="%s"' % aria if aria else ''
    pre = ''
    if ledon is not None:
        pre = led(ledon)
    return '<button type="button"%s%s style="%s">%s%s</button>' % (a, ' aria-pressed="true"' if on else '', st, pre, label)

def screw(pos, rot=35):
    return ('<div aria-hidden="true" style="position:absolute;%s;width:9px;height:9px;border-radius:50%%;background:radial-gradient(circle at 35%% 30%%,#7a8088 0%%,#3a3e44 55%%,#202327 100%%);box-shadow:0 1px 1px rgba(0,0,0,.8);display:flex;align-items:center;justify-content:center">'
            '<div style="width:7px;height:1.5px;background:#121416;transform:rotate(%ddeg)"></div></div>' % (pos, rot))

def knob(label, value, angle, size=30, aria=None):
    return ('<div style="display:flex;flex-direction:column;align-items:center;gap:3px">'
            '<button type="button" aria-label="%s" style="appearance:none;position:relative;width:%dpx;height:%dpx;padding:0;border-radius:50%%;border:1px solid #060708;cursor:pointer;'
            'background:radial-gradient(circle at 35%% 28%%,#5a6069 0%%,#2c3036 55%%,#16181b 100%%);box-shadow:0 3px 4px rgba(0,0,0,.6),inset 0 1px 0 rgba(255,255,255,.18),0 0 0 3px #15171a,0 0 0 4px #2b2f34;transform:rotate(%ddeg)">'
            '<span aria-hidden="true" style="position:absolute;left:50%%;top:3px;width:2px;height:%dpx;margin-left:-1px;border-radius:1px;background:#F2F4F6;box-shadow:0 0 4px rgba(255,255,255,.5)"></span></button>'
            '<span style="%s;font-size:10px;letter-spacing:.1em">%s</span>'
            '<span style="%s;font-size:10px;padding:1px 4px;line-height:12px">%s</span></div>'
            % (aria or (label + ' ' + value), size, size, angle, int(size * .32), SILK, label, LCD, value))

def meter_v(level, n=12, segh=4, w=5):
    segs = []
    for i in range(n, 0, -1):
        if i <= level:
            c = '#3DDC84' if i <= n - 4 else ('#FFC24A' if i <= n - 1 else '#FF5A4E')
            segs.append('<span style="display:block;width:%dpx;height:%dpx;border-radius:1px;background:%s;box-shadow:0 0 4px %s"></span>' % (w, segh, c, c + '99'))
        else:
            segs.append('<span style="display:block;width:%dpx;height:%dpx;border-radius:1px;background:#24282d"></span>' % (w, segh))
    return ''.join(segs)

def meter_h(level, n=30):
    segs = []
    for i in range(1, n + 1):
        if i <= level:
            c = '#3DDC84' if i <= n - 8 else ('#FFC24A' if i <= n - 2 else '#FF5A4E')
            segs.append('<span style="display:block;flex:1;height:7px;border-radius:1px;background:%s;box-shadow:0 0 4px %s"></span>' % (c, c + '88'))
        else:
            segs.append('<span style="display:block;flex:1;height:7px;border-radius:1px;background:#24282d"></span>')
    return ''.join(segs)

def icon(path, size=16, sw=1.8, fill=False):
    return ('<svg aria-hidden="true" width="%d" height="%d" viewBox="0 0 24 24" fill="%s" stroke="currentColor" stroke-width="%s" stroke-linecap="round" stroke-linejoin="round">%s</svg>'
            % (size, size, 'currentColor' if fill else 'none', sw, path))

I_RTZ = '<path d="M5 5v14"/><path d="M19 5 9 12l10 7z"/>'
I_REW = '<path d="M11 6 3 12l8 6z"/><path d="M21 6l-8 6 8 6z"/>'
I_FF = '<path d="M13 6l8 6-8 6z"/><path d="M3 6l8 6-8 6z"/>'
I_STOP = '<rect x="6" y="6" width="12" height="12" rx="1"/>'
I_PLAY = '<path d="M7 5l12 7-12 7z"/>'
I_REC = '<circle cx="12" cy="12" r="6"/>'
I_LOOP = '<path d="M17 2l3 3-3 3"/><path d="M4 11V9a4 4 0 0 1 4-4h12"/><path d="M7 22l-3-3 3-3"/><path d="M20 13v2a4 4 0 0 1-4 4H4"/>'
I_SEL = '<path d="M5 3l14 8-6 2-3 6z"/>'
I_PEN = '<path d="M4 20l4-1L19 8l-3-3L5 16z"/><path d="M14 7l3 3"/>'
I_ERASE = '<path d="M4 16l9-9 6 6-7 7H8z"/><path d="M12 20h8"/>'
I_SLUR = '<path d="M3 15c4-8 14-8 18 0"/>'
I_TEXT = '<path d="M5 6h14"/><path d="M12 6v13"/>'
I_HAND = '<path d="M8 13V5a1.5 1.5 0 0 1 3 0v6"/><path d="M11 11V4a1.5 1.5 0 0 1 3 0v7"/><path d="M14 11V5.5a1.5 1.5 0 0 1 3 0V14c0 4-3 7-6 7s-5-2-6.5-5L3 12.5a1.5 1.5 0 0 1 2.5-1.5L8 13"/>'
I_EYE = '<path d="M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>'
I_MINUS = '<path d="M5 12h14"/>'
I_PLUS = '<path d="M12 5v14"/><path d="M5 12h14"/>'
I_UP = '<path d="M6 15l6-6 6 6"/>'
I_DOWN = '<path d="M6 9l6 6 6-6"/>'

# ---------------- music ----------------
flute = [
    [(2, 'F#5'), (1, 'E5'), (1, 'D5')],
    [(1.5, 'D5'), (.5, 'C#5'), (2, 'B4')],
    [(1, 'r'), (.5, 'G4'), (.5, 'A4'), (1, 'B4'), (1, 'D5')],
    [(3, 'E5'), (1, 'r')],
    [(1, 'A5'), (.5, 'F#5'), (.5, 'G5'), (1, 'A5'), (1, 'F#5')],
    [(1.5, 'A5'), (.5, 'G5'), (1, 'F#5'), (1, 'E5')],
    [(1, 'D5'), (.5, 'E5'), (.5, 'F#5'), (2, 'G5')],
    [(4, 'E5')],
]
violins = [
    [(4, 'A4')],
    [(2, 'B4'), (2, 'F#4')],
    [(2, 'G4'), (2, 'B4')],
    [(.5, 'A4'), (.5, 'B4'), (.5, 'C#5'), (.5, 'D5'), (.5, 'E5'), (.5, 'F#5'), (.5, 'G5'), (.5, 'A5')],
    [(1, 'F#5'), (1, 'D5'), (1, 'F#5'), (1, 'A5')],
    [(2, 'C#5'), (2, 'A4')],
    [(1, 'B4'), (1, 'D5'), (2, 'G5')],
    [(2, 'E5'), (1, 'C#5'), (1, 'A4')],
]
CH = {'D': ['F#4', 'A4', 'D5'], 'Bm': ['F#4', 'B4', 'D5'], 'G': ['G4', 'B4', 'D5'], 'A': ['E4', 'A4', 'C#5'],
      'F#m': ['F#4', 'A4', 'C#5'], 'Asus': ['E4', 'A4', 'D5']}
piano_rh = [
    [(2, CH['D']), (2, CH['D'])],
    [(2, CH['Bm']), (2, CH['Bm'])],
    [(2, CH['G']), (2, CH['G'])],
    [(2, CH['A']), (2, CH['A'])],
    [(1, CH['D'])] * 4,
    [(1, CH['F#m'])] * 4,
    [(1, CH['G'])] * 4,
    [(2, CH['Asus']), (2, CH['A'])],
]
piano_lh = [
    [(1.5, 'D3'), (.5, 'A3'), (2, 'D3')],
    [(1.5, 'B2'), (.5, 'F#3'), (2, 'B2')],
    [(1.5, 'G2'), (.5, 'D3'), (2, 'G2')],
    [(1.5, 'A2'), (.5, 'E3'), (2, 'A2')],
    [(1, 'D3'), (1, 'A3'), (1, 'D3'), (1, 'A3')],
    [(1, 'F#2'), (1, 'C#3'), (1, 'F#2'), (1, 'C#3')],
    [(1, 'G2'), (1, 'D3'), (1, 'G2'), (1, 'D3')],
    [(4, 'A2')],
]
cello = [
    [(2, 'F#3'), (2, 'A3')],
    [(4, 'D3')],
    [(2, 'B2'), (2, 'D3')],
    [(2, 'C#3'), (2, 'E3')],
    [(1, 'A3'), (1, 'F#3'), (2, 'A3')],
    [(2, 'C#4'), (2, 'A3')],
    [(1, 'B3'), (1, 'G3'), (2, 'D3')],
    [(4, 'E3')],
]
cbass = [
    [(4, 'D2')], [(4, 'B2')], [(4, 'G2')], [(4, 'A2')],
    [(1, 'D2'), (1, 'r'), (1, 'D2'), (1, 'A2')],
    [(1, 'F#2'), (1, 'r'), (1, 'F#2'), (1, 'C#3')],
    [(1, 'G2'), (1, 'r'), (1, 'G2'), (1, 'D3')],
    [(2, 'A2'), (2, 'A2')],
]
# perc: B = bass (F4), T = tone (C5), S = slap (E5, x-head)
PERC = {'B': ('F4', False), 'T': ('C5', False), 'S': ('E5', True)}
verse_dj = [(1, 'B'), (.5, 'T'), (.5, 'T'), (1, 'S'), (.5, 'T'), (.5, 'T')]
chorus_dj = [(.5, 'B'), (.5, 'T'), (.5, 'S'), (.5, 'T'), (.5, 'B'), (.5, 'T'), (.5, 'S'), (.5, 'S')]
djembe = [verse_dj] * 4 + [chorus_dj] * 3 + [[(.5, 'B'), (.5, 'T'), (.5, 'S'), (.5, 'T'), (2, 'S')]]

# ---------------- svg drawing ----------------
def ledger_lines(st, i, x, cls='inks'):
    out = []
    if i <= st.ref - 2:
        k = st.ref - 2
        while k >= i:
            out.append('<line x1="%s" x2="%s" y1="%s" y2="%s" class="%s" stroke-width="1.2"/>' % (f(x - 7.5), f(x + 7.5), f(st.y(k)), f(st.y(k)), cls))
            k -= 2
    if i >= st.ref + 10:
        k = st.ref + 10
        while k <= i:
            out.append('<line x1="%s" x2="%s" y1="%s" y2="%s" class="%s" stroke-width="1.2"/>' % (f(x - 7.5), f(x + 7.5), f(st.y(k)), f(st.y(k)), cls))
            k += 2
    return out

def heads(st, x, ids, dur, sel, xhead=False):
    fc, sc = ('sel', 'sels') if sel else ('ink', 'inks')
    out = []
    for i in ids:
        out += ledger_lines(st, i, x, sc)
        y = st.y(i)
        if xhead:
            out.append('<path d="M%s %sl7 7M%s %sl-7 7" class="%s" stroke-width="1.7" stroke-linecap="round"/>' % (f(x - 3.5), f(y - 3.5), f(x + 3.5), f(y - 3.5), sc))
        elif dur >= 2:
            out.append('<ellipse cx="%s" cy="%s" rx="4.4" ry="3" transform="rotate(-20 %s %s)" fill="none" class="%s" stroke-width="1.6"/>' % (f(x), f(y), f(x), f(y), sc))
        else:
            out.append('<ellipse cx="%s" cy="%s" rx="4.4" ry="3.1" transform="rotate(-20 %s %s)" class="%s"/>' % (f(x), f(y), f(x), f(y), fc))
        if dur in (1.5, 3, .75):
            dy = y - 1.75 if (i - st.ref) % 2 == 0 else y
            out.append('<circle cx="%s" cy="%s" r="1.5" class="%s"/>' % (f(x + 8), f(dy), fc))
    return out

def stem(x, y0, y1, up, sel):
    sx = x + 4 if up else x - 4
    return sx, '<line x1="%s" x2="%s" y1="%s" y2="%s" class="%s" stroke-width="1.3"/>' % (f(sx), f(sx), f(y0), f(y1), 'sels' if sel else 'inks')

def rest(st, x, dur):
    my = st.y(st.mid)
    if dur == 1:
        return ['<text x="%s" y="%s" class="ink mus" font-size="28">\U0001D13D</text>' % (f(x - 4), f(my))]
    if dur == .5:
        return ['<text x="%s" y="%s" class="ink mus" font-size="28">\U0001D13E</text>' % (f(x - 4), f(my))]
    if dur == 2:
        return ['<rect x="%s" y="%s" width="10" height="3.5" class="ink"/>' % (f(x - 5), f(my - 3.5))]
    return ['<rect x="%s" y="%s" width="10" height="3.5" class="ink"/>' % (f(x - 5), f(st.y(st.ref + 6)))]

def draw_bar(st, b, events, sel=False, perc=False):
    out = []
    starts = []
    s = 0
    for d, p in events:
        starts.append(s); s += d
    def ids_of(p):
        if perc:
            return [pidx(PERC[p][0])]
        if isinstance(p, list):
            return [pidx(q) for q in p]
        return [pidx(p)]
    i = 0
    while i < len(events):
        d, p = events[i]
        x = xpos(b, starts[i])
        if p == 'r':
            out += rest(st, x, d); i += 1; continue
        xh = perc and PERC[p][1]
        ids = ids_of(p)
        if d == .5 and starts[i] % 1 == 0 and i + 1 < len(events) and events[i + 1][0] == .5 and events[i + 1][1] != 'r':
            # beamed pair
            d2, p2 = events[i + 1]
            x2 = xpos(b, starts[i + 1])
            ids2 = ids_of(p2)
            xh2 = perc and PERC[p2][1]
            allids = ids + ids2
            up = True if perc else (sum(allids) / float(len(allids)) < st.mid)
            out += heads(st, x, ids, d, sel, xh)
            out += heads(st, x2, ids2, d2, sel, xh2)
            ys1 = [st.y(k) for k in ids]; ys2 = [st.y(k) for k in ids2]
            if up:
                by = min(ys1 + ys2) - 24
                sx1, l1 = stem(x, max(ys1), by, True, sel)
                sx2, l2 = stem(x2, max(ys2), by, True, sel)
                out += [l1, l2, '<rect x="%s" y="%s" width="%s" height="3.6" class="%s"/>' % (f(sx1 - .6), f(by), f(sx2 - sx1 + 1.2), 'sel' if sel else 'ink')]
            else:
                by = max(ys1 + ys2) + 24
                sx1, l1 = stem(x, min(ys1), by, False, sel)
                sx2, l2 = stem(x2, min(ys2), by, False, sel)
                out += [l1, l2, '<rect x="%s" y="%s" width="%s" height="3.6" class="%s"/>' % (f(sx1 - .6), f(by - 3.6), f(sx2 - sx1 + 1.2), 'sel' if sel else 'ink')]
            i += 2
            continue
        up = True if perc else (sum(ids) / float(len(ids)) < st.mid)
        out += heads(st, x, ids, d, sel, xh)
        if d < 4:
            ys = [st.y(k) for k in ids]
            if up:
                sx, l = stem(x, max(ys), min(ys) - 24, True, sel)
                end = min(ys) - 24
            else:
                sx, l = stem(x, min(ys), max(ys) + 24, False, sel)
                end = max(ys) + 24
            out.append(l)
            if d == .5:
                if up:
                    out.append('<path d="M%s %sc0 5 8 7 7 15" fill="none" class="%s" stroke-width="1.6"/>' % (f(sx), f(end), 'sels' if sel else 'inks'))
                else:
                    out.append('<path d="M%s %sc0 -5 8 -7 7 -15" fill="none" class="%s" stroke-width="1.6"/>' % (f(sx), f(end), 'sels' if sel else 'inks'))
        i += 1
    return out

def staff_lines(st, perc=False):
    out = []
    for k in range(5):
        y = st.top + k * SP
        out.append('<line x1="0" x2="%d" y1="%s" y2="%s" class="stf" stroke-width="1"/>' % (LANE_W, f(y), f(y)))
    for b in range(NB + 1):
        x = CLEF_W + b * BAR_W
        out.append('<line x1="%d" x2="%d" y1="%s" y2="%s" class="stf" stroke-width="1.2"/>' % (x, x, f(st.top), f(st.bot)))
    if perc:
        out.append('<rect x="12" y="%s" width="3.2" height="14" class="ink"/><rect x="18" y="%s" width="3.2" height="14" class="ink"/>' % (f(st.top + 7), f(st.top + 7)))
    elif st.clef == 'treble':
        out.append('<text x="5" y="%s" class="ink mus" font-size="28">\U0001D11E</text>' % f(st.y(st.ref + 2)))
        out.append('<text x="27" y="%s" class="ink mus" font-size="28">♯</text>' % f(st.y(st.ref + 8)))
        out.append('<text x="34" y="%s" class="ink mus" font-size="28">♯</text>' % f(st.y(st.ref + 5)))
    else:
        out.append('<text x="5" y="%s" class="ink mus" font-size="28">\U0001D122</text>' % f(st.y(st.ref + 6)))
        out.append('<text x="27" y="%s" class="ink mus" font-size="28">♯</text>' % f(st.y(st.ref + 6)))
        out.append('<text x="34" y="%s" class="ink mus" font-size="28">♯</text>' % f(st.y(st.ref + 3)))
    return out

def grid(h):
    out = []
    for b in range(NB):
        for q in range(1, 4):
            x = CLEF_W + b * BAR_W + q * BAR_W / 4.0
            out.append('<line x1="%s" x2="%s" y1="0" y2="%d" style="stroke:var(--grid)" stroke-width="1"/>' % (f(x), f(x), h))
    for b in range(NB + 1):
        x = CLEF_W + b * BAR_W
        out.append('<line x1="%d" x2="%d" y1="0" y2="%d" style="stroke:var(--grid)" stroke-width="2"/>' % (x, x, h))
    return out

def clips(color, items, h):
    out = []
    for b0, b1, name in items:
        x = CLEF_W + b0 * BAR_W + 2; w = (b1 - b0) * BAR_W - 4
        out.append('<rect x="%d" y="4" width="%d" height="%d" rx="5" fill="%s" fill-opacity=".09" stroke="%s" stroke-opacity=".55" stroke-width="1"/>' % (x, w, h - 8, color, color))
        out.append('<path d="M%d 18V9a5 5 0 0 1 5-5h%dA5 5 0 0 1 %d 9v9z" fill="%s"/>' % (x, w - 10, x + w, color))
        out.append('<text x="%d" y="14.5" font-family="Barlow Condensed, sans-serif" font-weight="700" font-size="10.5" letter-spacing=".06em" fill="#0E1013">%s</text>' % (x + 7, name.upper()))
    return out

def dyn(st, b, beat, txt):
    return '<text x="%s" y="%s" class="ink dyn" font-size="15">%s</text>' % (f(xpos(b, beat) - 6), f(st.bot + 24), txt)

def slur(st, b, events, s0, s1):
    starts = []; s = 0
    ys = []
    for d, p in events:
        if s0 <= s <= s1 and p != 'r':
            ys.append(st.y(pidx(p)))
        starts.append(s); s += d
    x1 = xpos(b, s0) + 2; x2 = xpos(b, s1) + 2
    y = min(ys + [st.top]) - 9
    mx = (x1 + x2) / 2
    return '<path d="M%s %sQ%s %s %s %sQ%s %s %s %sZ" class="ink"/>' % (f(x1), f(y), f(mx), f(y - 13), f(x2), f(y), f(mx), f(y - 10), f(x1), f(y))

def playhead(h):
    return '<line x1="%s" x2="%s" y1="0" y2="%d" class="ph" stroke-width="1.6"/>' % (PLAY_X, PLAY_X, h)

def lane_svg(h, inner, label):
    return ('<svg role="img" aria-label="%s" width="%d" height="%d" viewBox="0 0 %d %d" style="display:block;flex:0 0 auto;background:var(--paper)">%s</svg>'
            % (label, LANE_W, h, LANE_W, h, ''.join(inner)))

# ---------------- track headers ----------------
def header(num, name, patch, color, h, vol, pan, panang, level, rec=False, solo=False, mute=False, extra=''):
    return ('<div style="flex:0 0 %dpx;width:%dpx;height:%dpx;box-sizing:border-box;display:flex;background:linear-gradient(180deg,#24282d,#1c1f23);border-right:1px solid #08090a">'
            '<div style="width:5px;background:%s;box-shadow:0 0 8px %s66"></div>'
            '<div style="flex:1;min-width:0;padding:8px 8px 8px 9px;display:flex;flex-direction:column;gap:8px">'
            '<div style="display:flex;gap:7px;align-items:stretch">'
            '<div style="%s;font-size:10px;width:20px;display:flex;align-items:center;justify-content:center">%02d</div>'
            '<div style="%s;flex:1;min-width:0;padding:4px 7px;display:flex;flex-direction:column;gap:1px">'
            '<span style="font-size:13px;letter-spacing:.06em;white-space:nowrap;overflow:hidden;text-overflow:ellipsis">%s</span>'
            '<span style="font-size:10px;opacity:.72;white-space:nowrap;overflow:hidden;text-overflow:ellipsis">%s</span></div></div>'
            '<div style="display:flex;gap:6px;align-items:flex-start">'
            '<div style="display:flex;flex-direction:column;gap:5px">'
            '<div style="display:flex;gap:4px">%s%s%s</div>%s</div>'
            '<div style="flex:1"></div>%s%s</div></div>'
            '<div aria-hidden="true" style="width:14px;padding:8px 0;display:flex;flex-direction:column;align-items:center;justify-content:flex-end;gap:2px;background:#131518;border-left:1px solid #08090a">%s</div>'
            '</div>'
            % (HDR_W, HDR_W, h, color, color, LCD, num, LCD, name, patch,
               rsm('R', rec, '#FF5A4E', 'Record arm ' + name), rsm('S', solo, '#FFC24A', 'Solo ' + name), rsm('M', mute, '#4FD1FF', 'Mute ' + name),
               extra,
               knob('VOL', vol, 210 if vol.startswith('-') else 0, 26, 'Volume ' + vol + ' dB'),
               knob('PAN', pan, panang, 26, 'Pan ' + pan),
               meter_v(level, 12, 4, 6) if h < 150 else meter_v(level, 20, 4, 6)))

def rsm(letter, on, color, aria):
    glow = color + '99'
    st = KEYBASE + 'width:24px;height:22px;padding:0;font-size:12px;'
    if on:
        st += 'background:%s;color:#121416;box-shadow:inset 0 1px 0 rgba(255,255,255,.45),0 0 10px %s,0 1px 0 #08090a;' % (color, glow)
    else:
        st += KEY_OFF
    return '<button type="button" aria-label="%s"%s style="%s">%s</button>' % (aria, ' aria-pressed="true"' if on else '', st, letter)

def small_tag(t):
    return '<span style="%s;font-size:10px;letter-spacing:.1em;padding:2px 6px;border:1px solid #33383e;border-radius:3px;align-self:flex-start">%s</span>' % (SILK, t)

# ---------------- lanes ----------------
lanes_html = []

def single_lane(num, name, patch, color, clef, music, clipdefs, vol, pan, panang, level, sel_bar=None, dyns=(), slurs=(), hairpin=None, perc=False, rec=False, solo=False, mute=False, tag='', h=108):
    ref = 30 if (clef == 'treble' or perc) else 18
    st = Staff(40, ref, clef)
    inner = []
    inner += grid(h)
    inner += clips(color, clipdefs, h)
    if sel_bar is not None:
        x = CLEF_W + sel_bar * BAR_W + 5
        inner.append('<rect x="%d" y="22" width="%d" height="%d" rx="3" style="fill:var(--led);fill-opacity:.12;stroke:var(--led);stroke-opacity:.8" stroke-width="1" stroke-dasharray="4 3"/>' % (x, BAR_W - 10, h - 30))
    inner += staff_lines(st, perc)
    for b, ev in enumerate(music):
        inner += draw_bar(st, b, ev, sel=(b == sel_bar), perc=perc)
    for b, beat, t in dyns:
        inner.append(dyn(st, b, beat, t))
    for b, s0, s1 in slurs:
        inner.append(slur(st, b, music[b], s0, s1))
    if hairpin is not None:
        b = hairpin
        x1 = CLEF_W + b * BAR_W + 16; x2 = CLEF_W + (b + 1) * BAR_W - 12; y = st.bot + 19
        inner.append('<path d="M%s %sL%s %sM%s %sL%s %s" fill="none" class="inks" stroke-width="1.3"/>' % (f(x1), f(y), f(x2), f(y - 5), f(x1), f(y), f(x2), f(y + 5)))
    inner.append(playhead(h))
    hd = header(num, name, patch, color, h, vol, pan, panang, level, rec, solo, mute, small_tag(tag) if tag else '')
    lanes_html.append('<div style="display:flex;border-top:1px solid #08090a">%s%s</div>' % (hd, lane_svg(h, inner, name.title() + ' staff, bars 41 to 48')))

single_lane(1, 'FLUTE 1', 'SOLO FLUTE · LEGATO', '#4FA8E8', 'treble', flute,
            [(0, 4, 'Flute · Verse 2 call'), (4, 8, 'Flute · Chorus melody')], '-6.0', 'L12', -40, 7,
            dyns=[(0, 0, 'mp'), (4, 0, 'mf')], slurs=[(0, 0, 3), (4, 0, 3)], tag='Treble')
single_lane(2, 'VIOLINS I', 'STRINGS · SUSTAIN', '#E0699A', 'treble', violins,
            [(0, 8, 'Violins I · Countermelody')], '-4.5', 'L30', -70, 9, sel_bar=3,
            dyns=[(0, 0, 'p'), (4, 0, 'f')], hairpin=3, rec=True, tag='Treble · 8 sel')

# piano grand staff
def piano_lane():
    h = 184
    tr = Staff(40, 30, 'treble'); bs = Staff(112, 18, 'bass')
    color = '#A78BFA'
    inner = grid(h) + clips(color, [(0, 8, 'Piano · Pad chords')], h)
    inner += staff_lines(tr) + staff_lines(bs)
    inner.append('<rect x="1.5" y="40" width="3" height="100" rx="1" class="ink"/>')
    for b in range(NB):
        x = CLEF_W + b * BAR_W
        inner.append('<line x1="%d" x2="%d" y1="68" y2="112" class="stf" stroke-width="1.2"/>' % (x, x))
    x = CLEF_W + NB * BAR_W
    inner.append('<line x1="%d" x2="%d" y1="68" y2="112" class="stf" stroke-width="1.2"/>' % (x, x))
    for b in range(NB):
        inner += draw_bar(tr, b, piano_rh[b])
        inner += draw_bar(bs, b, piano_lh[b])
    inner.append('<text x="%s" y="94" class="ink dyn" font-size="15">mp</text>' % f(xpos(0, 0) - 6))
    inner.append('<text x="%s" y="94" class="ink dyn" font-size="15">mf</text>' % f(xpos(4, 0) - 6))
    # pedal marks
    for b in range(NB):
        inner.append('<text x="%s" y="172" class="ink dyn" font-size="12">Ped.</text>' % f(xpos(b, 0) - 6))
        inner.append('<line x1="%s" x2="%s" y1="168" y2="168" class="inks" stroke-width="1"/>' % (f(xpos(b, 0) + 22), f(xpos(b, 3.6))))
    inner.append(playhead(h))
    extra = ('<div style="display:flex;flex-direction:column;gap:5px;margin-top:4px">%s%s'
             '<div style="display:flex;gap:4px">%s%s</div></div>'
             % (small_tag('Grand staff'), small_tag('Split C4'), key('Ped', True, h=22, pad='0 7px', fs=11, ledon=True), key('8va', False, h=22, pad='0 7px', fs=11)))
    hd = header(3, 'PIANO', 'CONCERT GRAND · SOFT', color, h, '-8.0', 'C', 0, 6, extra=extra)
    lanes_html.append('<div style="display:flex;border-top:1px solid #08090a">%s%s</div>' % (hd, lane_svg(h, inner, 'Piano grand staff, bars 41 to 48')))
piano_lane()

single_lane(4, 'CELLOS', 'STRINGS · SUSTAIN', '#2EC4B6', 'bass', cello,
            [(0, 4, 'Cellos · Pedal'), (4, 8, 'Cellos · Lift')], '-5.0', 'R20', 50, 8,
            dyns=[(0, 0, 'p'), (4, 0, 'mf')], slurs=[(0, 0, 2), (4, 0, 2)], tag='Bass')
single_lane(5, 'CONTRABASS', 'STRINGS · PIZZ / ARCO', '#8FC85A', 'bass', cbass,
            [(0, 4, 'Contrabass · Arco roots'), (4, 8, 'Contrabass · Pizz')], '-7.5', 'R10', 25, 7,
            dyns=[(0, 0, 'pp'), (4, 0, 'mf')], tag='Bass · 8vb', mute=False)
single_lane(6, 'DJEMBE', 'WORLD PERC · DJEMBE', '#E46F4B', 'treble', djembe,
            [(0, 4, 'Djembe · Verse groove'), (4, 8, 'Djembe · Chorus drive')], '-9.0', 'L20', -50, 10,
            dyns=[(0, 0, 'mp'), (4, 0, 'f')], perc=True, solo=True, tag='Perc · B T S')

# ---------------- ruler ----------------
def ruler():
    h = 88
    o = []
    rows = [(0, 22), (22, 44), (44, 66), (66, 88)]
    for k, (a, b) in enumerate(rows):
        o.append('<rect x="0" y="%d" width="%d" height="%d" fill="%s"/>' % (a, LANE_W, b - a, '#181b1f' if k % 2 == 0 else '#1d2025'))
    bars = list(range(41, 49))
    for b in range(NB):
        x = CLEF_W + b * BAR_W
        o.append('<line x1="%d" x2="%d" y1="0" y2="22" stroke="#6B747E" stroke-width="1"/>' % (x, x))
        o.append('<text x="%d" y="14" font-family="Share Tech Mono, monospace" font-size="12" fill="#C9D0D8">%d</text>' % (x + 5, bars[b]))
        for q in range(1, 4):
            xx = x + q * BAR_W / 4.0
            o.append('<line x1="%s" x2="%s" y1="15" y2="22" stroke="#4E565F" stroke-width="1"/>' % (f(xx), f(xx)))
            o.append('<text x="%s" y="13" font-family="Share Tech Mono, monospace" font-size="9" fill="#7D8792">%d</text>' % (f(xx + 3), q + 1))
    # markers
    def marker(b, txt, c):
        x = CLEF_W + b * BAR_W
        w = len(txt) * 6.4 + 16
        return ('<path d="M%d 25h%sl6 8-6 8H%dz" fill="%s"/><text x="%d" y="37" font-family="Barlow Condensed, sans-serif" font-weight="700" font-size="11" letter-spacing=".1em" fill="#121416">%s</text>'
                % (x, f(w), x, c, x + 6, txt))
    o.append(marker(0, 'VERSE 2', '#F5D547'))
    o.append(marker(4, 'CHORUS 2', '#F5D547'))
    o.append(marker(7, 'RIT.', '#9CA3AF'))
    # chords
    chords = ['D', 'Bm', 'G', 'A', 'D', 'F#m', 'G', 'Asus4  A']
    for b, c in enumerate(chords):
        x = CLEF_W + b * BAR_W + 4
        o.append('<rect x="%d" y="47" width="%d" height="16" rx="3" fill="#2a2f35" stroke="#3a4048"/>' % (x, len(c) * 7 + 14))
        o.append('<text x="%d" y="59" font-family="Inter, sans-serif" font-weight="600" font-size="11.5" fill="#E7EBF0">%s</text>' % (x + 7, c))
    # tempo
    xr = xpos(7, 0)
    o.append('<path d="M%d 82H%sL%d 86" fill="none" stroke="var(--lcd)" stroke-width="1.4"/>' % (CLEF_W, f(xr), LANE_W))
    o.append('<text x="%d" y="78" font-size="13" fill="#E7EBF0" font-family="Noto Music">♩</text>' % (CLEF_W + 5))
    o.append('<text x="%d" y="78" font-family="Share Tech Mono, monospace" font-size="11" fill="#C9D0D8">= 96</text>' % (CLEF_W + 15))
    o.append('<text x="%s" y="78" font-family="Share Tech Mono, monospace" font-size="11" fill="#C9D0D8">96 → 88</text>' % f(xr + 4))
    # playhead
    o.append('<line x1="%s" x2="%s" y1="0" y2="%d" class="ph" stroke-width="1.6"/>' % (PLAY_X, PLAY_X, h))
    o.append('<path d="M%s 0h12l-6 8z" style="fill:var(--lcd)"/>' % f(PLAY_X - 6))
    # loop range bar 45-48 (on bars row)
    o.append('<rect x="%d" y="19" width="%d" height="3" style="fill:var(--led)"/>' % (CLEF_W + 4 * BAR_W, 4 * BAR_W))
    svg = ('<svg role="img" aria-label="Timeline ruler: bars 41 to 48, markers, chords and tempo" width="%d" height="%d" viewBox="0 0 %d %d" style="display:block;flex:0 0 auto">%s</svg>'
           % (LANE_W, h, LANE_W, h, ''.join(o)))
    labels = ''
    for t, extra in [('Bars | Beats', ''), ('Markers', key(icon(I_PLUS, 11, 2.2), False, h=16, pad='0 3px', aria='Add marker')), ('Chords', key(icon(I_PLUS, 11, 2.2), False, h=16, pad='0 3px', aria='Add chord symbol')), ('Tempo', '')]:
        labels += ('<div style="height:22px;display:flex;align-items:center;justify-content:space-between;padding:0 8px 0 12px;box-sizing:border-box;border-bottom:1px solid #121417"><span style="%s;font-size:10.5px">%s</span>%s</div>' % (SILK, t, extra))
    return ('<div style="display:flex"><div style="flex:0 0 %dpx;width:%dpx;background:#1a1d21;border-right:1px solid #08090a">%s</div>%s</div>' % (HDR_W, HDR_W, labels, svg))

# ---------------- navigator ----------------
def navigator():
    rows = []
    cols = ['#4FA8E8', '#E0699A', '#A78BFA', '#2EC4B6', '#8FC85A', '#E46F4B']
    segs = [
        [(8, 24), (32, 48), (56, 72), (80, 92)],
        [(0, 30), (40, 70), (76, 96)],
        [(0, 96)],
        [(4, 40), (40, 88)],
        [(8, 92)],
        [(16, 30), (34, 48), (52, 92)],
    ]
    for r, (c, ss) in enumerate(zip(cols, segs)):
        for a, b in ss:
            rows.append('<rect x="%d" y="%d" width="%d" height="3" rx="1" fill="%s" fill-opacity=".8"/>' % (a * 10, 4 + r * 4, (b - a) * 10 - 2, c))
    rows.append('<rect x="400" y="1.5" width="80" height="26" rx="2" fill="none" style="stroke:var(--lcd)" stroke-width="1.5"/>')
    rows.append('<line x1="438" x2="438" y1="0" y2="30" class="ph" stroke-width="1.2"/>')
    return ('<div style="display:flex;align-items:center;gap:10px;margin-bottom:8px">'
            '<span style="%s;flex:0 0 auto">Overview</span>'
            '<div style="%s;flex:1;min-width:0;height:30px;padding:0 4px">'
            '<svg role="img" aria-label="Song overview, bars 1 to 96, view at bars 41 to 48" width="100%%" height="30" viewBox="0 0 960 30" preserveAspectRatio="none" style="display:block">%s</svg></div>'
            '<span style="%s;flex:0 0 auto;font-size:11px;padding:4px 8px">BARS 41–48 / 96</span></div>'
            % (SILK, LCD, ''.join(rows), LCD))

# ---------------- top deck ----------------
VIEW = globals().get('VIEW', 'composer')

def view_switch(view):
    def opt(label, href, on):
        st = KEYBASE + (KEY_ON if on else KEY_OFF) + 'flex:1;height:30px;padding:0 8px;text-decoration:none;font-size:12.5px;letter-spacing:.14em;'
        return '<a href="%s"%s style="%s">%s%s</a>' % (href, ' aria-current="page"' if on else '', st, led(on), label)
    return ('<nav aria-label="Workspace view" style="display:flex;gap:6px;padding:3px;border-radius:7px;background:#0b0c0e;box-shadow:inset 0 2px 5px rgba(0,0,0,.9),0 1px 0 rgba(255,255,255,.07)">%s%s</nav>'
            % (opt('Composer', 'Main.dc.html', view == 'composer'), opt('Producer', 'Producer.dc.html', view == 'producer')))

def stage_bar(active):
    stages = ['Compose', 'Score', 'Mix', 'Master']
    ai = stages.index(active)
    parts = []
    for i, s_ in enumerate(stages):
        if i == ai:
            lab = led(True) + s_
        elif i < ai:
            lab = led(True, '#3DDC84', '#3DDC8499') + s_
        else:
            lab = led(False) + s_
        parts.append(key(lab, i == ai, h=22, fs=11, pad='0 8px'))
    sep = '<span aria-hidden="true" style="color:#5B636D;font-size:13px">›</span>'
    return ('<div role="group" aria-label="Workflow stage" style="display:flex;align-items:center;gap:6px"><span style="%s;font-size:10px;margin-right:2px">Stage</span>%s</div>' % (SILK, sep.join(parts)))

def top_deck():
    brand = ('<div style="%s;position:relative;flex:0 0 268px;height:96px;box-sizing:border-box;padding:11px 16px 12px;display:flex;flex-direction:column;justify-content:space-between;'
             'background:repeating-linear-gradient(90deg,rgba(255,255,255,.025) 0 1px,transparent 1px 3px),linear-gradient(180deg,#30353b,#1f2327)">'
             '%s%s%s%s'
             '<div style="display:flex;align-items:baseline;gap:8px;padding-left:4px">'
             '<span style="font-family:\'Barlow Condensed\',sans-serif;font-weight:500;font-size:12px;letter-spacing:.3em;color:#9AA3AD">VC</span>'
             '<span style="font-family:\'Barlow Condensed\',sans-serif;font-weight:700;font-size:24px;letter-spacing:.22em;color:#EEF1F4">COMPOSER</span>'
             '<span style="font-family:\'Barlow Condensed\',sans-serif;font-weight:700;font-size:28px;color:var(--led);text-shadow:0 0 12px var(--ledglow)">2</span></div>'
             '%s</div>'
             % (PANEL, screw('top:6px;left:6px'), screw('top:6px;right:6px', -20), screw('bottom:6px;left:6px', 70), screw('bottom:6px;right:6px', 10), view_switch(VIEW)))

    def tkey(ic, aria, on=False, color=None, fill=True):
        st = KEYBASE + 'width:46px;height:40px;padding:0;flex-direction:column;gap:3px;'
        if on:
            st += 'background:linear-gradient(180deg,#4a4f57,#363b42);box-shadow:inset 0 1px 0 rgba(255,255,255,.2),inset 0 0 0 1px %s,0 1px 0 #08090a,0 0 12px %s;color:%s;' % (color, color + '88', color)
        else:
            st += KEY_OFF
        return '<button type="button" aria-label="%s"%s style="%s">%s</button>' % (aria, ' aria-pressed="true"' if on else '', st, icon(ic, 18, 1.8, fill))
    transport = ('<div style="%s;flex:0 0 auto;height:96px;box-sizing:border-box;padding:10px 14px;display:flex;flex-direction:column;gap:8px">'
                 '<div style="display:flex;justify-content:space-between"><span style="%s">Transport</span><span style="display:flex;gap:6px;align-items:center"><span style="%s;font-size:10px">Pre-roll</span>%s<span style="%s;font-size:10px">Post</span>%s</span></div>'
                 '<div style="display:flex;gap:7px">%s%s%s%s%s%s%s</div></div>'
                 % (PANEL, SILK, SILK, led(True), SILK, led(False),
                    tkey(I_RTZ, 'Return to zero'), tkey(I_REW, 'Rewind'), tkey(I_FF, 'Fast forward'), tkey(I_STOP, 'Stop'),
                    tkey(I_PLAY, 'Play', True, '#3DDC84'), tkey(I_REC, 'Record', False, None), tkey(I_LOOP, 'Loop playback', True, '#FFB547', False)))

    counter = ('<div style="%s;flex:0 0 auto;height:96px;box-sizing:border-box;padding:10px 12px;display:flex;gap:10px">'
               '<div style="%s;width:250px;padding:6px 12px;display:flex;flex-direction:column;justify-content:center">'
               '<div style="display:flex;justify-content:space-between;font-size:10px;opacity:.7;letter-spacing:.12em"><span>MAIN · BARS|BEATS</span><span>SUB · MIN:SEC</span></div>'
               '<div style="display:flex;align-items:baseline;justify-content:space-between"><span style="font-size:36px;line-height:42px;letter-spacing:.02em">44|3|941</span>'
               '<span style="font-size:15px">01:42.929</span></div></div>'
               '<div style="%s;width:150px;padding:6px 10px;display:grid;grid-template-columns:auto 1fr;column-gap:10px;row-gap:3px;font-size:12px;align-content:center">'
               '<span style="opacity:.65;font-size:10px;letter-spacing:.1em">START</span><span style="text-align:right">44|3|941</span>'
               '<span style="opacity:.65;font-size:10px;letter-spacing:.1em">END</span><span style="text-align:right">44|3|941</span>'
               '<span style="opacity:.65;font-size:10px;letter-spacing:.1em">LENGTH</span><span style="text-align:right">0|0|000</span></div></div>'
               % (PANEL, LCD, LCD))

    def mini(label, val, w):
        return ('<div style="display:flex;flex-direction:column;gap:3px"><span style="%s;font-size:10px">%s</span>'
                '<div style="%s;width:%dpx;height:26px;display:flex;align-items:center;justify-content:center;font-size:15px">%s</div></div>' % (SILK, label, LCD, w, val))
    session = ('<div style="%s;flex:0 0 auto;height:96px;box-sizing:border-box;padding:10px 12px;display:flex;flex-direction:column;gap:8px">'
               '<div style="display:flex;gap:8px">%s%s%s</div>'
               '<div style="display:flex;gap:6px">%s%s%s</div></div>'
               % (PANEL, mini('Tempo', '96.00', 74), mini('Meter', '4/4', 50), mini('Key', 'D MAJ', 70),
                  key('Click', True, h=22, ledon=True, fs=11), key('Count-in', False, h=22, ledon=False, fs=11), key('Wait note', False, h=22, ledon=False, fs=11)))

    master = ('<div style="%s;flex:1 1 220px;min-width:200px;height:96px;box-sizing:border-box;padding:10px 14px;display:flex;flex-direction:column;gap:7px">'
              '<div style="display:flex;justify-content:space-between;align-items:center"><span style="%s">Master</span>'
              '<span style="display:flex;gap:6px;align-items:center"><span style="%s;font-size:10px">MIDI in</span>%s<span style="%s;font-size:10px;margin-left:6px">Sync int</span>%s</span></div>'
              '<div style="display:flex;align-items:center;gap:8px"><span style="%s;font-size:10px;width:10px">L</span><div aria-hidden="true" style="flex:1;display:flex;gap:2px">%s</div></div>'
              '<div style="display:flex;align-items:center;gap:8px"><span style="%s;font-size:10px;width:10px">R</span><div aria-hidden="true" style="flex:1;display:flex;gap:2px">%s</div></div>'
              '<div style="display:flex;justify-content:space-between;align-items:center"><span style="%s;font-size:10px">−60 · −18 · −6 · 0 dBFS</span>'
              '<span style="%s;font-size:11px;padding:2px 6px">PEAK −3.2</span></div></div>'
              % (PANEL, SILK, SILK, led(True, '#3DDC84', '#3DDC8499'), SILK, led(True), SILK, meter_h(23), SILK, meter_h(21), SILK, LCD))

    return '<div style="display:flex;flex-wrap:wrap;gap:10px">%s%s%s%s%s</div>' % (brand, transport, counter, session, master)

def toolbar():
    def group(label, inner):
        return ('<div style="display:flex;flex-direction:column;gap:5px"><span style="%s;font-size:10px">%s</span><div style="display:flex;gap:5px;align-items:center">%s</div></div>' % (SILK, label, inner))
    def sep():
        return '<div aria-hidden="true" style="width:1px;align-self:stretch;background:linear-gradient(180deg,transparent,#08090a 20%,#08090a 80%,transparent);box-shadow:1px 0 0 rgba(255,255,255,.05)"></div>'
    mode = ''.join(key(t, t == 'Step', h=28, ledon=(t == 'Step')) for t in ['Insert', 'Overwrite', 'Step', 'Real-time'])
    view = ''.join(key(t, t == 'Track', h=28) for t in ['Track', 'Page', 'Mixer'])
    tools = ''.join(key(icon(ic, 15), on, w=32, h=28, pad='0', aria=a) for ic, on, a in
                    [(I_SEL, False, 'Select tool'), (I_PEN, True, 'Note entry pencil'), (I_ERASE, False, 'Eraser'), (I_SLUR, False, 'Slur and tie tool'), (I_TEXT, False, 'Lyrics and text'), (I_HAND, False, 'Scroll hand')])
    def nv(glyph, num, on=False, aria=''):
        st = KEYBASE + (KEY_ON if on else KEY_OFF) + 'width:34px;height:28px;padding:0;position:relative;'
        return ('<button type="button" aria-label="%s"%s style="%s"><span aria-hidden="true" style="position:absolute;top:1px;left:3px;font-family:\'Share Tech Mono\',monospace;font-size:8px;color:#8E98A3">%s</span>'
                '<span style="font-family:\'Noto Music\';font-size:19px;line-height:1;text-transform:none;margin-top:2px">%s</span></button>' % (aria, ' aria-pressed="true"' if on else '', st, num, glyph))
    notes = ''.join([nv('\U0001D15D', '7', aria='Whole note'), nv('\U0001D15E', '6', aria='Half note'), nv('\U0001D15F', '5', aria='Quarter note'),
                     nv('\U0001D160', '4', True, 'Eighth note'), nv('\U0001D161', '3', aria='Sixteenth note'), nv('\U0001D162', '2', aria='Thirty-second note'),
                     nv('\U0001D16D', '.', aria='Dotted'), nv('3', '/', aria='Triplet'), nv('\U0001D13D', '0', aria='Rest')])
    acc = ''.join(key('<span style="font-family:\'Noto Music\';font-size:17px;text-transform:none">%s</span>' % g, False, w=30, h=28, pad='0', aria=a) for g, a in [('♭', 'Flat'), ('♮', 'Natural'), ('♯', 'Sharp')])
    gridg = ('<div style="%s;height:28px;width:58px;display:flex;align-items:center;justify-content:center;font-size:15px">1/16</div>%s%s'
             % (LCD, key(icon(I_DOWN, 13, 2.2), False, w=24, h=28, pad='0', aria='Grid finer'), key(icon(I_UP, 13, 2.2), False, w=24, h=28, pad='0', aria='Grid coarser')))
    zoom = key(icon(I_MINUS, 13, 2.2), False, w=28, h=28, pad='0', aria='Zoom out') + key(icon(I_PLUS, 13, 2.2), False, w=28, h=28, pad='0', aria='Zoom in') + key('Fit', False, h=28)
    return ('<div style="%s;display:flex;flex-wrap:wrap;gap:14px;align-items:flex-end;padding:9px 14px 11px">%s%s%s%s%s%s%s%s%s%s%s%s%s</div>'
            % (PANEL, group('Entry mode', mode), sep(), group('View', view), sep(), group('Tools', tools), sep(), group('Note value', notes), sep(), group('Accidental', acc), sep(), group('Grid', gridg), sep(), group('Zoom', zoom)))

def left_rack():
    tr = [('01', 'Flute 1', '#4FA8E8'), ('02', 'Violins I', '#E0699A'), ('03', 'Piano', '#A78BFA'), ('04', 'Cellos', '#2EC4B6'), ('05', 'Contrabass', '#8FC85A'), ('06', 'Djembe', '#E46F4B'),
          ('07', 'Violins II', '#E0699A'), ('08', 'Violas', '#E0699A'), ('09', 'Udu', '#E46F4B'), ('10', 'Chimes', '#F5D547')]
    items = ''
    for i, (n, name, c) in enumerate(tr):
        shown = i < 6
        items += ('<li style="display:flex;align-items:center;gap:8px;padding:5px 8px;border-radius:4px;%s">'
                  '<span aria-hidden="true" style="width:8px;height:8px;border-radius:2px;background:%s;opacity:%s"></span>'
                  '<span style="font-family:\'Share Tech Mono\',monospace;font-size:10px;color:#7D8792">%s</span>'
                  '<span style="flex:1;font-size:12.5px;color:%s">%s</span>'
                  '<button type="button" aria-label="%s %s" style="appearance:none;background:none;border:0;padding:2px;cursor:pointer;color:%s;display:flex">%s</button></li>'
                  % ('background:#2a2f35' if i == 1 else '', c, '1' if shown else '.45', n, '#E7EBF0' if shown else '#8E98A3', name,
                     'Hide' if shown else 'Show', name, '#C9D0D8' if shown else '#6B747E', icon(I_EYE, 14)))
    groups = ''
    for g, nm, on in [('a', 'Woodwinds', False), ('b', 'Strings', True), ('c', 'Keys', False), ('d', 'Percussion', False)]:
        groups += ('<li style="display:flex;align-items:center;gap:8px;padding:4px 8px">%s<span style="font-family:\'Share Tech Mono\',monospace;font-size:11px;color:#8E98A3">%s</span><span style="font-size:12.5px;color:#D3D9E0">%s</span></li>' % (led(on), g, nm))
    marks = ''
    for nm, bar, on in [('Intro', 1, False), ('Verse 1', 9, False), ('Chorus 1', 25, False), ('Verse 2', 41, True), ('Chorus 2', 45, False), ('Bridge', 57, False), ('Outro', 81, False)]:
        marks += ('<li><a href="#" style="display:flex;align-items:center;gap:8px;padding:4px 8px;border-radius:4px;text-decoration:none;color:%s;%s">'
                  '<span aria-hidden="true" style="width:0;height:0;border-top:5px solid transparent;border-bottom:5px solid transparent;border-left:7px solid %s"></span>'
                  '<span style="flex:1;font-size:12.5px">%s</span><span style="font-family:\'Share Tech Mono\',monospace;font-size:11px;color:#8E98A3">%d</span></a></li>'
                  % ('#FFFFFF' if on else '#D3D9E0', 'background:#2a2f35;box-shadow:inset 2px 0 0 var(--led)' if on else '', '#F5D547' if on else '#6B747E', nm, bar))
    def sec(title, body, extra=''):
        return ('<section style="%s;padding:10px 6px 8px">'
                '<div style="display:flex;justify-content:space-between;align-items:center;padding:0 8px 6px"><h2 style="margin:0;%s;font-size:11px">%s</h2>%s</div>'
                '<ul style="list-style:none;margin:0;padding:0;display:flex;flex-direction:column;gap:1px">%s</ul></section>' % (PANEL, SILK, title, extra, body))
    return ('<aside style="flex:0 0 184px;min-width:0;display:flex;flex-direction:column;gap:10px">%s%s%s</aside>'
            % (sec('Tracks', items, key(icon(I_PLUS, 11, 2.2), False, h=18, pad='0 4px', aria='New track')), sec('Groups', groups), sec('Markers', marks)))

def inspector():
    def sec(title, body, right=''):
        return ('<section style="%s;padding:10px 12px 12px;display:flex;flex-direction:column;gap:9px">'
                '<div style="display:flex;justify-content:space-between;align-items:center"><h2 style="margin:0;%s;font-size:11px">%s</h2>%s</div>%s</section>' % (PANEL, SILK, title, right, body))
    sel = ('<div style="%s;padding:9px 11px;display:grid;grid-template-columns:auto 1fr;column-gap:12px;row-gap:4px;font-size:12.5px">'
           '<span style="opacity:.65;font-size:10px;letter-spacing:.1em;align-self:center">TRACK</span><span>VIOLINS I</span>'
           '<span style="opacity:.65;font-size:10px;letter-spacing:.1em;align-self:center">RANGE</span><span>BAR 44 · BEAT 1–4</span>'
           '<span style="opacity:.65;font-size:10px;letter-spacing:.1em;align-self:center">NOTES</span><span>8 · A4 → A5</span>'
           '<span style="opacity:.65;font-size:10px;letter-spacing:.1em;align-self:center">VALUE</span><span>1/8 · BEAMED 2+2</span>'
           '<span style="opacity:.65;font-size:10px;letter-spacing:.1em;align-self:center">VELOCITY</span><span>78 → 104</span></div>')
    def art(svgp, aria, on=False):
        return key('<svg aria-hidden="true" width="22" height="16" viewBox="0 0 22 16" fill="currentColor" stroke="currentColor" stroke-width="1.6" stroke-linecap="round">%s</svg>' % svgp, on, h=30, pad='0', aria=aria, extra='flex:1;')
    arts = ('<div style="display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:6px">%s%s%s%s%s%s</div>'
            % (art('<circle cx="11" cy="8" r="2" stroke="none"/>', 'Staccato'),
               art('<path d="M5 4l12 4-12 4" fill="none"/>', 'Accent', True),
               art('<path d="M4 8h14" fill="none" stroke-width="2.2"/>', 'Tenuto'),
               art('<path d="M5 13l6-10 6 10" fill="none" stroke-width="2"/>', 'Marcato'),
               art('<path d="M3 13a8 8 0 0 1 16 0" fill="none"/><circle cx="11" cy="11" r="1.6" stroke="none"/>', 'Fermata'),
               art('<path d="M6 4v9M3.5 6h5M11 13V7c0-1 1-2 3-2" fill="none"/>', 'Trill')))
    def dk(t, on=False, aria=None):
        return key('<span style="font-family:Georgia,serif;font-style:italic;font-weight:700;font-size:15px;text-transform:none;letter-spacing:0">%s</span>' % t, on, h=30, pad='0', aria=aria or ('Dynamic ' + t), extra='flex:1;')
    dyns = ('<div style="display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:6px">%s%s%s%s%s%s%s%s</div>'
            % (dk('pp'), dk('p'), dk('mp'), dk('mf'), dk('f'), dk('ff'),
               key('<svg aria-hidden="true" width="24" height="12" viewBox="0 0 24 12" fill="none" stroke="currentColor" stroke-width="1.6"><path d="M22 1L2 6l20 5"/></svg>', True, h=30, pad='0', aria='Crescendo hairpin', extra='flex:1;'),
               key('<svg aria-hidden="true" width="24" height="12" viewBox="0 0 24 12" fill="none" stroke="currentColor" stroke-width="1.6"><path d="M2 1l20 5-20 5"/></svg>', False, h=30, pad='0', aria='Diminuendo hairpin', extra='flex:1;')))
    def ringknob(label, val, frac, aria):
        dots = ''
        n = 15
        import math
        for i in range(n):
            a = -135 + 270 * i / (n - 1)
            r = 36
            x = 42 + r * math.sin(math.radians(a)); y = 42 - r * math.cos(math.radians(a))
            on = i / (n - 1) <= frac
            dots += '<circle cx="%s" cy="%s" r="2.2" %s/>' % (f(x), f(y), 'style="fill:var(--led);filter:drop-shadow(0 0 2px var(--led))"' if on else 'fill="#2b2e32"')
        ang = -135 + 270 * frac
        return ('<div style="display:flex;flex-direction:column;align-items:center;gap:4px;flex:1">'
                '<div style="position:relative;width:84px;height:84px">'
                '<svg aria-hidden="true" width="84" height="84" viewBox="0 0 84 84" style="position:absolute;inset:0">%s</svg>'
                '<button type="button" aria-label="%s" style="appearance:none;position:absolute;left:17px;top:17px;width:50px;height:50px;padding:0;border-radius:50%%;border:1px solid #060708;cursor:pointer;'
                'background:repeating-conic-gradient(from 0deg,rgba(255,255,255,.05) 0 4deg,rgba(0,0,0,.08) 4deg 8deg),radial-gradient(circle at 35%% 28%%,#5f656e 0%%,#2c3036 60%%,#16181b 100%%);'
                'box-shadow:0 4px 6px rgba(0,0,0,.7),inset 0 1px 0 rgba(255,255,255,.2);transform:rotate(%ddeg)">'
                '<span aria-hidden="true" style="position:absolute;left:50%%;top:5px;width:3px;height:14px;margin-left:-1.5px;border-radius:2px;background:var(--led);box-shadow:0 0 6px var(--led)"></span></button></div>'
                '<span style="%s;font-size:10px">%s</span><span style="%s;font-size:12px;padding:2px 8px">%s</span></div>' % (dots, aria, int(ang), SILK, label, LCD, val))
    knobs = '<div style="display:flex;gap:6px">%s%s</div>' % (ringknob('Velocity', '92', .72, 'Velocity 92'), ringknob('Humanize', '18%', .25, 'Humanize 18 percent'))
    voice_cols = ['#4FA8E8', '#3DDC84', '#FFB547', '#E0699A']
    voices = '<div style="display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:6px">'
    for i, c in enumerate(voice_cols):
        on = i == 0
        st = KEYBASE + 'height:28px;padding:0;' + (KEY_ON if on else KEY_OFF)
        voices += '<button type="button" aria-label="Voice %d"%s style="%s"><span aria-hidden="true" style="width:8px;height:8px;border-radius:2px;background:%s"></span>%d</button>' % (i + 1, ' aria-pressed="true"' if on else '', st, c, i + 1)
    voices += '</div>'
    transp = ('<div style="display:flex;gap:6px;align-items:center">%s<div style="%s;flex:1;height:28px;display:flex;align-items:center;justify-content:center;font-size:14px">+0 ST</div>%s%s%s</div>'
              % (key(icon(I_MINUS, 13, 2.2), False, w=28, h=28, pad='0', aria='Transpose down'), LCD, key(icon(I_PLUS, 13, 2.2), False, w=28, h=28, pad='0', aria='Transpose up'),
                 key('Oct −', False, h=28, pad='0 6px', fs=11), key('Oct +', False, h=28, pad='0 6px', fs=11)))
    return ('<aside style="flex:0 0 268px;min-width:0;display:flex;flex-direction:column;gap:10px">%s%s%s%s%s%s</aside>'
            % (sec('Selection', sel, '<span style="display:flex;align-items:center;gap:5px"><span style="%s;font-size:10px">Live</span>%s</span>' % (SILK, led(True))),
               sec('Articulation', arts), sec('Dynamics', dyns), sec('Performance', knobs), sec('Voice', voices), sec('Transpose', transp)))

def bottom_deck():
    # 88 keys: A0..C8
    order = []
    order += [('A', 0), ('B', 0)]
    for o in range(1, 8):
        for l in 'CDEFGAB':
            order.append((l, o))
    order.append(('C', 8))
    NW = len(order)  # 52
    lit = {('A', 4), ('B', 4), ('D', 5), ('E', 5), ('G', 5), ('A', 5)}
    litb = {('C', 5), ('F', 5)}  # sharps: C#5, F#5
    wk = ''
    for (l, o) in order:
        on = (l, o) in lit
        mid = (l, o) == ('C', 4)
        lab = ('%s%d' % (l, o)) if l == 'C' else ''
        dot = '<span aria-hidden="true" style="position:absolute;bottom:20px;left:50%;width:5px;height:5px;margin-left:-2.5px;border-radius:50%;background:var(--lcd);box-shadow:0 0 5px var(--glow)"></span>' if mid else ''
        wk += ('<div style="flex:1;min-width:0;position:relative;border-radius:0 0 4px 4px;border:1px solid #1a1a1a;border-top:0;%s">%s'
               '<span style="position:absolute;bottom:4px;left:0;right:0;text-align:center;font-family:\'Share Tech Mono\',monospace;font-size:9px;color:#4a4a4a">%s</span></div>'
               % ('background:linear-gradient(180deg,var(--led) 0%,var(--led) 70%,#fff3 100%);box-shadow:inset 0 -4px 0 rgba(0,0,0,.2),0 0 10px var(--ledglow)' if on else 'background:linear-gradient(180deg,#f5f4f0 0%,#e3e2dc 85%,#cfcec8 100%);box-shadow:inset 0 -4px 0 rgba(0,0,0,.12)', dot, lab))
    bk = ''
    bw = 0.6 * 100.0 / NW
    for i, (l, o) in enumerate(order[:-1]):
        if l in 'CDFGA':
            on = (l, o) in litb
            bk += ('<div style="position:absolute;top:0;left:%s%%;width:%s%%;height:62%%;border-radius:0 0 3px 3px;%s"></div>'
                   % (f((i + 1) * 100.0 / NW - bw / 2), f(bw), 'background:var(--led);box-shadow:0 0 10px var(--ledglow),inset 0 -3px 0 rgba(0,0,0,.3)' if on else 'background:linear-gradient(180deg,#2e2e2e,#0d0d0d);box-shadow:inset 0 -3px 0 rgba(255,255,255,.08),0 2px 3px rgba(0,0,0,.6)'))
    keyboard = ('<div ref="{{kbRef}}" style="flex:1 1 520px;min-width:0;overflow-x:auto;overflow-y:hidden;background:#08090a;border-radius:4px;padding:4px 0 6px;box-shadow:inset 0 2px 6px rgba(0,0,0,.9)">'
                '<div role="img" aria-label="88-key keyboard, A0 to C8, selected notes lit" style="position:relative;display:flex;width:{{kbW}}px;height:{{kbH}}px;border-top:4px solid #08090a">%s%s</div></div>' % (wk, bk))
    def wheel(label, pos, aria):
        ribs = ''.join('<span style="display:block;height:2px;background:rgba(0,0,0,.35);margin-bottom:3px"></span>' for _ in range(9))
        return ('<div style="display:flex;flex-direction:column;align-items:center;gap:5px">'
                '<div style="width:28px;height:84px;border-radius:6px;background:#0b0c0e;box-shadow:inset 0 2px 6px rgba(0,0,0,.9);padding:4px;box-sizing:border-box">'
                '<button type="button" aria-label="%s" style="appearance:none;display:block;width:20px;height:76px;border:0;border-radius:10px;padding:%dpx 0 0;cursor:pointer;background:linear-gradient(90deg,#24272b,#4a4f56 50%%,#24272b);box-sizing:border-box;overflow:hidden">%s</button></div>'
                '<span style="%s;font-size:10px">%s</span></div>' % (aria, pos, ribs, SILK, label))
    left = ('<div style="display:flex;gap:10px;align-items:flex-start;flex:0 0 auto">%s%s'
            '<div style="display:flex;flex-direction:column;gap:6px;padding-left:4px"><span style="%s;font-size:10px">Octave</span>'
            '<div style="display:flex;gap:5px">%s%s</div><div style="%s;font-size:13px;height:26px;display:flex;align-items:center;justify-content:center">±0</div></div></div>'
            % (wheel('Pitch', 18, 'Pitch bend wheel'), wheel('Mod', 4, 'Modulation wheel'), SILK,
               key(icon(I_MINUS, 13, 2.2), False, w=30, h=26, pad='0', aria='Octave down'), key(icon(I_PLUS, 13, 2.2), False, w=30, h=26, pad='0', aria='Octave up'), LCD))
    step = ('<div style="flex:0 0 220px;display:flex;flex-direction:column;gap:7px"><span style="%s;font-size:10px">Step input</span>'
            '<div style="%s;padding:8px 10px;font-size:12.5px;display:flex;flex-direction:column;gap:3px">'
            '<span>NEXT  ▸ BAR 44 · BEAT 4.5</span><span style="opacity:.75">VALUE 1/8 · VOICE 1 · VEL 92</span></div>'
            '<div style="display:flex;gap:6px">%s%s%s</div></div>'
            % (SILK, LCD, key('Chord', False, h=26, ledon=False, fs=11), key('Tie', False, h=26, fs=11), key('Enter', True, h=26, fs=11, extra='flex:1;')))
    def zbtn(ic, aria, handler):
        st = KEYBASE + KEY_OFF + 'width:26px;height:24px;padding:0;'
        return '<button type="button" aria-label="%s" onClick="{{%s}}" style="%s">%s</button>' % (aria, handler, st, icon(ic, 12, 2.2))
    fitst = KEYBASE + KEY_OFF + 'height:24px;padding:0 8px;font-size:11px;'
    selst = KEYBASE + KEY_OFF + 'height:24px;padding:0 8px;font-size:11px;'
    zoom = ('<div style="display:flex;align-items:center;gap:8px;flex-wrap:wrap">'
            '<label for="kbzoom" style="%s;font-size:10px">Key zoom</label>%s'
            '<div style="display:flex;align-items:center;height:24px;padding:0 10px;border-radius:12px;background:#0b0c0e;box-shadow:inset 0 2px 5px rgba(0,0,0,.9),0 1px 0 rgba(255,255,255,.06)">'
            '<input id="kbzoom" type="range" min="1" max="3" step="0.05" value="{{zoomVal}}" onInput="{{onZoom}}" onChange="{{onZoom}}" style="width:200px;margin:0;accent-color:var(--led);cursor:pointer"></div>'
            '%s<div style="%s;font-size:13px;height:24px;width:56px;display:flex;align-items:center;justify-content:center">{{zoomLabel}}</div>'
            '<button type="button" onClick="{{zoomFit}}" style="%s">88 · Fit</button>'
            '<button type="button" onClick="{{zoomSel}}" style="%s">Zoom to selection</button></div>'
            % (SILK, zbtn(I_MINUS, 'Zoom keyboard out', 'zoomOut'), zbtn(I_PLUS, 'Zoom keyboard in', 'zoomIn'), LCD, fitst, selst))
    head = ('<div style="flex:1 1 100%%;display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap;padding:0 12px">'
            '<div style="display:flex;align-items:center;gap:10px"><span style="%s">Keyboard</span>'
            '<span style="%s;font-size:11px;padding:3px 8px">88 KEYS · A0–C8</span>'
            '<span style="display:flex;align-items:center;gap:5px"><span style="%s;font-size:10px">Middle C</span>%s</span></div>%s</div>'
            % (SILK, LCD, SILK, led(True, 'var(--lcd)', 'var(--glow)'), zoom))
    return ('<div style="%s;display:flex;flex-wrap:wrap;gap:12px 16px;align-items:center;padding:12px 16px;position:relative">%s%s%s%s%s%s</div>'
            % (PANEL, screw('top:6px;left:6px'), screw('top:6px;right:6px', 60), head, left, keyboard, step))

def status():
    items = ['48 kHz · 24-bit', 'CPU 18%', 'DISK 4%', 'Score: D major · 4/4', 'Concert pitch', 'Autosaved 08:14']
    s = ''.join('<span>%s</span>' % t for t in items)
    return ('<div style="display:flex;flex-wrap:wrap;gap:18px;padding:2px 6px;font-family:\'Share Tech Mono\',monospace;font-size:11px;color:#8E98A3">%s</div>' % s)

edit_window = ('<main style="%s;flex:999 1 640px;min-width:0;padding:10px;display:flex;flex-direction:column">'
               '<div style="display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap;margin-bottom:8px"><div style="display:flex;align-items:center;gap:16px;flex-wrap:wrap"><h1 style="margin:0;%s;font-size:12px;color:#C9D0D8">Edit · Sunset at the Border 2026</h1>%s</div>'
               '<span style="display:flex;gap:6px">%s%s%s</span></div>'
               '%s'
               '<div style="overflow-x:auto;border:1px solid #08090a;border-radius:4px;background:#08090a">'
               '<div style="width:%dpx">%s%s</div></div></main>'
               % (PANEL, SILK, stage_bar('Compose' if VIEW == 'composer' else 'Mix'), key('Link sel', True, h=22, ledon=True, fs=11), key('Follow', True, h=22, ledon=True, fs=11), key('Show velocity', False, h=22, ledon=False, fs=11),
                  navigator(), HDR_W + LANE_W, ruler(), ''.join(lanes_html)))

body = ('<div style="display:flex;flex-wrap:wrap;gap:10px;align-items:flex-start">%s%s%s</div>' % (left_rack(), edit_window, inspector()))

root_style = ('--led: {{led}}; --ledglow: {{ledGlow}}; --lcd: {{lcd}}; --glow: {{glow}}; --paper: {{paper}}; --ink: {{ink}}; --staff: {{staff}}; --grid: {{grid}}; '
              'box-sizing: border-box; min-height: 1240px; padding: 14px; display: flex; flex-direction: column; gap: 10px; background: #0c0d0f; '
              'font-family: Inter, sans-serif; color: #D3D9E0')

html = '''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Composer 2 – Edit window</title>
<script src="./support.js"></script>
</head>
<body>
<x-dc>
<helmet>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Barlow+Condensed:wght@500;600;700&family=Inter:wght@400;500;600&family=Noto+Music&family=Share+Tech+Mono&display=swap">
<style>
body{margin:0;background:#0c0d0f}
a{color:#D3D9E0}a:hover{color:#FFFFFF}
.ink{fill:var(--ink)}.inks{stroke:var(--ink)}.stf{stroke:var(--staff)}.sel{fill:var(--led)}.sels{stroke:var(--led)}.ph{stroke:var(--lcd)}
.mus{font-family:'Noto Music',serif}.dyn{font-family:Georgia,'Times New Roman',serif;font-style:italic;font-weight:700}
</style>
</helmet>
<div style="''' + root_style + '''">
''' + top_deck() + '\n' + toolbar() + '\n' + body + '\n' + bottom_deck() + '\n' + status() + '''
</div>
</x-dc>
<script type="text/x-dc" data-dc-script data-props='{"led":{"editor":"color","default":"#FFB547","options":["#FFB547","#FF5A4E","#4FD1FF","#9BF07A"],"section":"Hardware"},"lcd":{"editor":"color","default":"#6EE7D2","options":["#6EE7D2","#9BF07A","#FFC266","#9CC2FF"],"section":"Hardware"},"staffPaper":{"editor":"enum","options":["Dark","Paper"],"default":"Dark","section":"Score"},"$preview":{"width":1680,"height":1240}}'>
class Component extends DCLogic {
  componentDidUpdate(prevProps, prevState) {
    const z = (this.state && this.state.zoom) || 1;
    const pz = (prevState && prevState.zoom) || 1;
    if (this.kb && z !== pz) {
      // keep the selected notes (around E5, white key 32 of 52) centred while zooming
      const kw = 22 * z;
      this.kb.scrollLeft = Math.max(0, 32.5 * kw - this.kb.clientWidth / 2);
    }
  }
  renderVals() {
    const led = this.props.led ?? '#FFB547';
    const lcd = this.props.lcd ?? '#6EE7D2';
    const paper = (this.props.staffPaper ?? 'Dark') === 'Paper';
    const rgba = (h, a) => {
      const n = parseInt(String(h).replace('#', '').slice(0, 6), 16);
      return 'rgba(' + ((n >> 16) & 255) + ',' + ((n >> 8) & 255) + ',' + (n & 255) + ',' + a + ')';
    };
    const zoom = (this.state && this.state.zoom) || 1;
    const setZ = (z) => this.setState({ zoom: Math.max(1, Math.min(3, Math.round(z * 100) / 100)) });
    const kw = 22 * zoom;
    return {
      zoomVal: zoom,
      zoomLabel: zoom.toFixed(2) + '\u00d7',
      kbW: Math.round(52 * kw),
      kbH: Math.round(96 + (zoom - 1) * 48),
      onZoom: (e) => setZ(parseFloat(e.target.value)),
      zoomIn: () => setZ(zoom + 0.25),
      zoomOut: () => setZ(zoom - 0.25),
      zoomFit: () => setZ(1),
      zoomSel: () => setZ(2.5),
      kbRef: (el) => { this.kb = el; },
      led: led,
      ledGlow: rgba(led, 0.6),
      lcd: lcd,
      glow: rgba(lcd, 0.55),
      paper: paper ? '#F1ECE1' : '#14171B',
      ink: paper ? '#1B1A18' : '#E7EBF0',
      staff: paper ? '#8C8579' : '#525B66',
      grid: paper ? 'rgba(0,0,0,0.07)' : 'rgba(255,255,255,0.04)'
    };
  }
}
</script>
</body>
</html>
'''
open(OUT, 'w').write(html)
print(len(html))
