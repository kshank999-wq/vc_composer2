import sys, math, random
OUT = sys.argv[1]
import os
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gen_composer.py')).read()
src = src[:src.index('edit_window = (')]
ns = {'VIEW': 'producer', '__name__': 'gp'}
sys_argv = sys.argv
sys.argv = ['x', '/dev/null']
exec(src, ns)
sys.argv = sys_argv
g = ns  # shorthand
for k in ['CLEF_W', 'BAR_W', 'NB', 'LANE_W', 'HDR_W', 'SILK', 'PANEL', 'LCD', 'KEYBASE', 'KEY_OFF', 'KEY_ON', 'PLAY_X',
          'led', 'key', 'screw', 'knob', 'meter_v', 'meter_h', 'icon', 'f', 'grid', 'playhead', 'ruler', 'navigator', 'top_deck',
          'stage_bar', 'status', 'flute', 'violins', 'piano_rh', 'piano_lh', 'cello', 'cbass', 'djembe', 'rsm',
          'I_MINUS', 'I_PLUS', 'I_DOWN', 'I_UP', 'I_PEN', 'I_HAND', 'I_EYE']:
    globals()[k] = ns[k]

BEAT = BAR_W / 4.0
SEMI = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}

def midi(n):
    return 12 * (int(n[-1]) + 1) + SEMI[n[0]] + (1 if '#' in n else 0)

def flatten(music):
    out = []
    for b, ev in enumerate(music):
        s = 0
        for d, p in ev:
            if p != 'r':
                ps = p if isinstance(p, list) else [p]
                for q in ps:
                    out.append((b, s, d, midi(q)))
            s += d
    return out

DJ = {'B': 60, 'T': 62, 'S': 64}
def flatten_dj(music):
    out = []
    for b, ev in enumerate(music):
        s = 0
        for d, p in ev:
            out.append((b, s, d, DJ[p])); s += d
    return out

def nx(b, s):
    return CLEF_W + b * BAR_W + s * BEAT

TYPE_COL = {'INST': '#C9A2FF', 'MIDI': '#4FD1FF', 'AUDIO': '#3DDC84', 'AUX': '#F5D547'}

I_ZOOM = '<circle cx="10" cy="10" r="6"/><path d="M15 15l5 5"/>'
I_TRIM = '<path d="M9 4v16"/><path d="M15 4v16"/><path d="M5 9l-2 3 2 3"/><path d="M19 9l2 3-2 3"/>'
I_IBEAM = '<path d="M9 4h6M9 20h6M12 4v16"/>'
I_GRAB = '<path d="M5 3l14 8-6 2-3 6z"/>'
I_SCISS = '<circle cx="6" cy="7" r="3"/><circle cx="6" cy="17" r="3"/><path d="M8.5 8.5L20 18M8.5 15.5L20 6"/>'
I_SMART = '<path d="M4 3l9 5-4 1.5L7 14z"/><path d="M15 12h5M15 21h5M17.5 12v9"/>'
I_CHEV_D = '<path d="M6 9l6 6 6-6"/>'
I_CHEV_R = '<path d="M9 6l6 6-6 6"/>'
I_WAVE = '<path d="M3 12h2l2-6 3 12 3-9 2 5 2-2h4"/>'

def region(x0, x1, color, name, h, top=4, fill_op='.2', strip_text='#0E1013'):
    w = x1 - x0
    return ('<rect x="%s" y="%d" width="%s" height="%d" rx="4" fill="%s" fill-opacity="%s" stroke="%s" stroke-opacity=".7"/>'
            '<path d="M%s %dV%da4 4 0 0 1 4-4h%sa4 4 0 0 1 4 4v%dz" fill="%s"/>'
            '<text x="%s" y="%d" font-family="Barlow Condensed, sans-serif" font-weight="700" font-size="10.5" letter-spacing=".06em" fill="%s">%s</text>'
            % (f(x0), top, f(w), h - top * 2, color, fill_op, color, f(x0), top + 14, top + 4, f(w - 8), 10, color, f(x0 + 7), top + 10, strip_text, name.upper()))

def gutter(h, label):
    return ('<rect x="0" y="0" width="%d" height="%d" fill="#1a1d21"/><line x1="%d" x2="%d" y1="0" y2="%d" stroke="#08090a"/>'
            '<text x="23" y="%s" text-anchor="middle" font-family="Share Tech Mono, monospace" font-size="9" style="fill:var(--lcd)">%s</text>'
            % (CLEF_W, h, CLEF_W, CLEF_W, h, f(h / 2 + 3), label))

def lane_frame(h, inner, label):
    return ('<svg role="img" aria-label="%s" width="%d" height="%d" viewBox="0 0 %d %d" style="display:block;flex:0 0 auto;background:#14171B">%s</svg>'
            % (label, LANE_W, h, LANE_W, h, ''.join(inner)))

def midi_lane_collapsed(notes, color, clipdefs, h=72, drum=False, label='READ'):
    o = [gutter(h, label)] + grid(h)[24:] if False else [gutter(h, label)]
    o += [l for l in grid(h) if 'x1="0"' not in l]
    lo = min(n[3] for n in notes); hi = max(n[3] for n in notes)
    if hi - lo < 6:
        lo -= 3; hi += 3
    for b0, b1, name in clipdefs:
        o.append(region(CLEF_W + b0 * BAR_W + 2, CLEF_W + b1 * BAR_W - 2, color, name, h))
    top = 23; bot = h - 9
    for b, s, d, p in notes:
        y = bot - (p - lo) / float(hi - lo) * (bot - top)
        x = nx(b, s) + 1
        if drum:
            o.append('<path d="M%s %sl3.5 3.5-3.5 3.5-3.5-3.5z" fill="%s"/>' % (f(x + 3), f(y - 3.5), color))
        else:
            o.append('<rect x="%s" y="%s" width="%s" height="3.6" rx="1" fill="%s"/>' % (f(x), f(y - 1.8), f(max(3, d * BEAT - 3)), color))
    o.append(playhead(h))
    return o

def midi_lane_expanded(notes, color, sel_bar=3, h=272):
    o = []
    lo, hi = 64, 84
    rh = 9; y0 = 24
    # row backgrounds
    for p in range(lo, hi + 1):
        y = y0 + (hi - p) * rh
        if p % 12 in (1, 3, 6, 8, 10):
            o.append('<rect x="%d" y="%d" width="%d" height="%d" fill="#000" fill-opacity=".22"/>' % (CLEF_W, y, LANE_W - CLEF_W, rh))
        if p % 12 == 0:
            o.append('<line x1="%d" x2="%d" y1="%d" y2="%d" stroke="#ffffff" stroke-opacity=".12"/>' % (CLEF_W, LANE_W, y + rh, y + rh))
    o += grid(h)
    o.append(region(CLEF_W + 2, LANE_W - 2, color, 'Violins I · Countermelody · MIDI', y0 + (hi - lo + 1) * rh + 6, top=4, fill_op='.07'))
    # vertical keyboard gutter
    o.append('<rect x="0" y="0" width="%d" height="%d" fill="#1a1d21"/>' % (CLEF_W, h))
    for p in range(lo, hi + 1):
        y = y0 + (hi - p) * rh
        if p % 12 in (1, 3, 6, 8, 10):
            o.append('<rect x="0" y="%d" width="28" height="%d" fill="#1b1b1b" stroke="#000" stroke-width=".5"/>' % (y, rh))
        else:
            o.append('<rect x="0" y="%d" width="%d" height="%d" fill="#ECEBE6" stroke="#9a9a9a" stroke-width=".5"/>' % (y, CLEF_W - 3, rh))
        if p % 12 == 0:
            o.append('<text x="%d" y="%d" text-anchor="end" font-family="Share Tech Mono, monospace" font-size="8" fill="#333">C%d</text>' % (CLEF_W - 5, y + 7.5, p // 12 - 1))
    # notes
    vels = []
    k = 0
    for b, s, d, p in notes:
        y = y0 + (hi - p) * rh
        x = nx(b, s) + 1
        sel = b == sel_bar
        if sel:
            o.append('<rect x="%s" y="%d" width="%s" height="%d" rx="2" style="fill:var(--led);stroke:#fff" stroke-opacity=".7" stroke-width=".8"/>' % (f(x), y + 1, f(d * BEAT - 3), rh - 2))
        else:
            o.append('<rect x="%s" y="%d" width="%s" height="%d" rx="2" fill="%s" stroke="#000" stroke-opacity=".4" stroke-width=".8"/>' % (f(x), y + 1, f(d * BEAT - 3), rh - 2, color))
        v = 78 + (k * 4 if sel else (k * 13) % 30)
        if sel: k += 1
        else: k += 1
        vels.append((x, min(v, 112), sel))
    # selection box
    sx = CLEF_W + sel_bar * BAR_W + 3
    o.append('<rect x="%d" y="%d" width="%d" height="%d" rx="3" style="fill:var(--led);fill-opacity:.08;stroke:var(--led);stroke-opacity:.8" stroke-dasharray="4 3"/>' % (sx, y0 - 2, BAR_W - 6, (hi - lo + 1) * rh + 4))
    # velocity lane
    vy0 = y0 + (hi - lo + 1) * rh + 14; vy1 = h - 8
    o.append('<rect x="%d" y="%d" width="%d" height="%d" fill="#101215"/>' % (CLEF_W, vy0 - 4, LANE_W - CLEF_W, vy1 - vy0 + 8))
    o.append('<line x1="%d" x2="%d" y1="%d" y2="%d" stroke="#3a4048"/>' % (CLEF_W, LANE_W, vy1, vy1))
    o.append('<text x="23" y="%d" text-anchor="middle" font-family="Share Tech Mono, monospace" font-size="9" style="fill:var(--lcd)">VEL</text>' % ((vy0 + vy1) / 2 + 3))
    for x, v, sel in vels:
        top = vy1 - v / 127.0 * (vy1 - vy0)
        c = 'style="stroke:var(--led)"' if sel else 'stroke="%s"' % color
        cf = 'style="fill:var(--led)"' if sel else 'fill="%s"' % color
        o.append('<line x1="%s" x2="%s" y1="%d" y2="%s" %s stroke-width="1.5"/><circle cx="%s" cy="%s" r="2.6" %s/>' % (f(x + 1), f(x + 1), vy1, f(top), c, f(x + 1), f(top), cf))
    o.append(playhead(h))
    return o

def waveform(x0, x1, cy, amp, env, color, seed, step=2.0):
    rnd = random.Random(seed)
    top = []; bot = []
    x = x0
    while x <= x1:
        a = env(x) * amp * (0.55 + 0.45 * rnd.random())
        top.append((x, cy - a)); bot.append((x, cy + a * (0.85 + 0.15 * rnd.random())))
        x += step
    pts = top + bot[::-1]
    d = 'M' + ' L'.join('%s %s' % (f(a), f(b)) for a, b in pts) + 'Z'
    return '<path d="%s" fill="%s" fill-opacity=".9"/><line x1="%s" x2="%s" y1="%s" y2="%s" stroke="%s" stroke-opacity=".5"/>' % (d, color, f(x0), f(x1), f(cy), f(cy), color)

def env_from_notes(notes):
    starts = sorted(set(nx(b, s) for b, s, d, p in notes))
    def e(x):
        best = 0.15
        for st in starts:
            if st <= x:
                dt = (x - st) / 40.0
                best = max(best, 0.95 * math.exp(-dt * 0.9) + 0.25)
        return min(best, 1.0)
    return e

def audio_lane(color, name, h, x0, x1, notes, seed, recording=False, label='READ'):
    o = [gutter(h, label)] + [l for l in grid(h)]
    if recording:
        o.append(region(x0, x1, '#FF5A4E', name, h, fill_op='.16'))
    else:
        o.append(region(x0, x1, color, name, h, fill_op='.14'))
    cy = 23 + (h - 30) / 2.0
    o.append(waveform(x0 + 2, x1 - 2, cy, (h - 32) / 2.0, env_from_notes(notes), '#FF8A80' if recording else color, seed))
    if recording:
        o.append('<rect x="%s" y="4" width="3" height="%d" fill="#FF5A4E"/>' % (f(x1 - 2), h - 8))
    o.append(playhead(h))
    return o

def aux_lane(h=60):
    o = [gutter(h, 'TOUCH')] + grid(h)
    pts = [(CLEF_W, 34), (nx(3, 0), 34), (nx(3, 3.5), 22), (nx(4, 0), 18), (nx(7, 0), 18), (nx(7, 3), 30), (LANE_W, 30)]
    d = 'M' + ' L'.join('%s %s' % (f(a), f(b)) for a, b in pts)
    o.append('<rect x="%d" y="14" width="%d" height="%d" fill="#F5D547" fill-opacity=".05"/>' % (CLEF_W, LANE_W - CLEF_W, h - 22))
    o.append('<path d="%s" fill="none" stroke="#F5D547" stroke-width="1.6"/>' % d)
    for a, b in pts[1:-1]:
        o.append('<circle cx="%s" cy="%s" r="3" fill="#14171B" stroke="#F5D547" stroke-width="1.5"/>' % (f(a), f(b)))
    o.append('<text x="%d" y="11" font-family="Barlow Condensed, sans-serif" font-weight="600" font-size="10" letter-spacing=".12em" fill="#C9D0D8">VOLUME AUTOMATION · −6.0 → −2.5 dB</text>' % (CLEF_W + 6))
    o.append(playhead(h))
    return o

# ---------------- producer track header ----------------
def chip(t):
    c = TYPE_COL[t]
    return ('<span style="font-family:\'Barlow Condensed\',sans-serif;font-weight:700;font-size:10px;letter-spacing:.1em;color:%s;border:1px solid %s;border-radius:3px;padding:1px 5px;line-height:13px;flex:0 0 auto">%s</span>' % (c, c, t))

def chevron(expanded, name, handler=None):
    st = KEYBASE + KEY_OFF + 'width:20px;height:20px;padding:0;flex:0 0 auto;'
    on = (' onClick="{{%s}}"' % handler) if handler else ''
    return '<button type="button" aria-expanded="%s" aria-label="%s %s"%s style="%s">%s</button>' % ('true' if expanded else 'false', 'Collapse' if expanded else 'Expand', name, on, st, icon(I_CHEV_D if expanded else I_CHEV_R, 12, 2.4))

def rsmi(name, rec=False, solo=False, mute=False, inp=False, show_i=False):
    out = rsm('R', rec, '#FF5A4E', 'Record arm ' + name) + rsm('S', solo, '#FFC24A', 'Solo ' + name) + rsm('M', mute, '#4FD1FF', 'Mute ' + name)
    if show_i:
        out += rsm('I', inp, '#3DDC84', 'Input monitor ' + name)
    return out

def pheader(num, name, ttype, color, h, vol, level, expanded=False, rec=False, solo=False, mute=False, inp=False, io=None, body='', handler=None):
    show_i = ttype in ('AUDIO', 'INST', 'MIDI')
    io_html = ''
    if io:
        io_html = ('<div style="%s;font-size:10px;padding:3px 6px;display:flex;justify-content:space-between;gap:6px;white-space:nowrap;overflow:hidden">'
                   '<span>IN %s</span><span>›</span><span>OUT %s</span></div>' % (LCD, io[0], io[1]))
    return ('<div style="flex:0 0 %dpx;width:%dpx;height:%dpx;box-sizing:border-box;display:flex;background:linear-gradient(180deg,#24282d,#1c1f23);border-right:1px solid #08090a">'
            '<div style="width:5px;background:%s;box-shadow:0 0 8px %s66"></div>'
            '<div style="flex:1;min-width:0;padding:7px 8px;display:flex;flex-direction:column;gap:6px">'
            '<div style="display:flex;gap:6px;align-items:center">%s%s'
            '<div style="%s;flex:1;min-width:0;padding:3px 7px;font-size:12.5px;letter-spacing:.05em;white-space:nowrap;overflow:hidden;text-overflow:ellipsis">%02d %s</div></div>'
            '<div style="display:flex;gap:4px;align-items:center">%s<div style="flex:1"></div><span style="%s;font-size:10.5px;padding:2px 5px">%s</span></div>'
            '%s%s</div>'
            '<div aria-hidden="true" style="width:14px;padding:7px 0;display:flex;flex-direction:column;align-items:center;justify-content:flex-end;gap:2px;background:#131518;border-left:1px solid #08090a">%s</div>'
            '</div>'
            % (HDR_W, HDR_W, h, color, color, chevron(expanded, name, handler), chip(ttype), LCD, num, name,
               rsmi(name, rec, solo, mute, inp, show_i), LCD, vol, io_html, body,
               meter_v(level, max(6, min(20, (h - 16) // 6)), 4, 6)))

def row(hd, svg):
    return '<div style="display:flex;border-top:1px solid #08090a">%s%s</div>' % (hd, svg)

rows = []
fl = flatten(flute); vn = flatten(violins); pn = flatten(piano_rh) + flatten(piano_lh); ce = flatten(cello); cb = flatten(cbass); dj = flatten_dj(djembe)

rows.append(row(pheader(1, 'FLUTE 1', 'INST', '#4FA8E8', 72, '−6.0', 6, io=None),
                lane_frame(72, midi_lane_collapsed(fl, '#4FA8E8', [(0, 4, 'Flute · Verse 2 call'), (4, 8, 'Flute · Chorus melody')]), 'Flute 1 MIDI regions')))

# violins: expanded / collapsed via state
ins_rows = ''.join('<div style="display:flex;align-items:center;gap:6px;font-size:11px">%s<span style="%s;flex:1;padding:2px 6px;font-size:10.5px;white-space:nowrap;overflow:hidden">%s</span></div>' % (led(on, '#3DDC84', '#3DDC8499'), LCD, t)
                   for on, t in [(True, 'SAMPLER · STRINGS'), (True, 'EQ-7 BAND'), (True, 'COMP · OPTO')])
vio_body = ('<div style="display:flex;flex-direction:column;gap:4px;margin-top:2px"><span style="%s;font-size:10px">Inserts</span>%s</div>'
            '<div style="display:flex;gap:5px;margin-top:4px">%s%s</div>'
            '<div style="display:flex;gap:5px">%s%s</div>'
            % (SILK, ins_rows,
               key(icon(I_WAVE, 13, 2) + 'Print to audio', False, h=24, fs=11, pad='0 7px', extra='flex:1;'),
               key('Freeze', False, h=24, fs=11, pad='0 7px'),
               key('Piano roll', True, h=22, fs=10.5, pad='0 6px', extra='flex:1;'), key('Score', False, h=22, fs=10.5, pad='0 6px', extra='flex:1;')))
vio_exp = row(pheader(2, 'VIOLINS I', 'INST', '#E0699A', 272, '−4.5', 9, expanded=True, rec=False, io=('MIDI ALL', 'STR BUS'), body=vio_body, handler='toggleVln'),
              lane_frame(272, midi_lane_expanded(vn, '#E0699A'), 'Violins I piano roll with velocity lane, bars 41 to 48'))
vio_col = row(pheader(2, 'VIOLINS I', 'INST', '#E0699A', 72, '−4.5', 9, expanded=False, handler='toggleVln'),
              lane_frame(72, midi_lane_collapsed(vn, '#E0699A', [(0, 8, 'Violins I · Countermelody')]), 'Violins I MIDI region'))
rows.append('<sc-if value="{{vlnOpen}}" hint-placeholder-val="{{ true }}">%s</sc-if><sc-if value="{{vlnClosed}}" hint-placeholder-val="{{ false }}">%s</sc-if>' % (vio_exp, vio_col))

rows.append(row(pheader(3, 'VLN I · PRINT', 'AUDIO', '#FF5A4E', 84, '−4.5', 10, rec=True, inp=True, io=('BUS VLN I', 'STR BUS')),
                lane_frame(84, audio_lane('#E0699A', '● REC · VIOLINS I_PRINT_01.wav', 84, CLEF_W + 2, PLAY_X, [n for n in vn if nx(n[0], n[1]) < PLAY_X], 7, recording=True, label='REC'), 'Violins I printed to audio, recording in progress')))
rows.append(row(pheader(4, 'PIANO', 'INST', '#A78BFA', 72, '−8.0', 6),
                lane_frame(72, midi_lane_collapsed(pn, '#A78BFA', [(0, 8, 'Piano · Pad chords')]), 'Piano MIDI region')))
rows.append(row(pheader(5, 'CELLOS', 'INST', '#2EC4B6', 72, '−5.0', 8),
                lane_frame(72, midi_lane_collapsed(ce, '#2EC4B6', [(0, 4, 'Cellos · Pedal'), (4, 8, 'Cellos · Lift')]), 'Cellos MIDI regions')))
rows.append(row(pheader(6, 'CONTRABASS', 'INST', '#8FC85A', 72, '−7.5', 7),
                lane_frame(72, midi_lane_collapsed(cb, '#8FC85A', [(0, 4, 'Contrabass · Arco'), (4, 8, 'Contrabass · Pizz')]), 'Contrabass MIDI regions')))
rows.append(row(pheader(7, 'DJEMBE', 'MIDI', '#E46F4B', 72, '−9.0', 9, solo=True),
                lane_frame(72, midi_lane_collapsed(dj, '#E46F4B', [(0, 4, 'Djembe · Verse groove'), (4, 8, 'Djembe · Chorus drive')], drum=True), 'Djembe MIDI regions to external drum module')))
udu_hits = [(b, s, .5, 60) for b in range(NB) for s in (0, 1.5, 2, 3, 3.5)]
rows.append(row(pheader(8, 'UDU · LIVE', 'AUDIO', '#D98C5F', 84, '−10.0', 8, io=('IN 3 MIC', 'PERC BUS')),
                lane_frame(84, audio_lane('#D98C5F', 'Udu live_03.wav', 84, CLEF_W + 2, LANE_W - 2, udu_hits, 21), 'Udu live audio waveform')))
rows.append(row(pheader(9, 'STRINGS BUS', 'AUX', '#F5D547', 60, '−2.5', 9),
                lane_frame(60, aux_lane(60), 'Strings bus volume automation')))

# ---------------- toolbar ----------------
def toolbar_p():
    def group(label, inner):
        return ('<div style="display:flex;flex-direction:column;gap:5px"><span style="%s;font-size:10px">%s</span><div style="display:flex;gap:5px;align-items:center">%s</div></div>' % (SILK, label, inner))
    def sep():
        return '<div aria-hidden="true" style="width:1px;align-self:stretch;background:linear-gradient(180deg,transparent,#08090a 20%,#08090a 80%,transparent);box-shadow:1px 0 0 rgba(255,255,255,.05)"></div>'
    mode = ''.join(key(t, t == 'Grid', h=28, ledon=(t == 'Grid')) for t in ['Shuffle', 'Slip', 'Spot', 'Grid'])
    tools = ''.join(key(icon(ic, 15), on, w=32, h=28, pad='0', aria=a) for ic, on, a in
                    [(I_ZOOM, False, 'Zoomer'), (I_TRIM, False, 'Trim tool'), (I_IBEAM, False, 'Selector'), (I_GRAB, False, 'Grabber'),
                     (I_SCISS, False, 'Scissors'), (I_PEN, False, 'Pencil'), (I_SMART, True, 'Smart tool')])
    gridg = ('<div style="%s;height:28px;width:54px;display:flex;align-items:center;justify-content:center;font-size:14px">1/16</div>'
             '<span style="%s;font-size:10px;margin-left:4px">Nudge</span><div style="%s;height:28px;width:54px;display:flex;align-items:center;justify-content:center;font-size:14px">1/32</div>' % (LCD, SILK, LCD))
    midi_g = key('Quantize', False, h=28) + key('Input Q', True, h=28, ledon=True) + key('Merge', False, h=28, ledon=False)
    rec = key('Loop rec', False, h=28, ledon=False) + key('Punch', True, h=28, ledon=True) + key('Pre-roll', False, h=28, ledon=False)
    add = key(icon(I_PLUS, 13, 2.4) + 'New track', True, h=28)
    zoom = key(icon(I_MINUS, 13, 2.2), False, w=28, h=28, pad='0', aria='Zoom out') + key(icon(I_PLUS, 13, 2.2), False, w=28, h=28, pad='0', aria='Zoom in')
    return ('<div style="%s;display:flex;flex-wrap:wrap;gap:14px;align-items:flex-end;padding:9px 14px 11px">%s%s%s%s%s%s%s%s%s%s%s%s%s</div>'
            % (PANEL, group('Edit mode', mode), sep(), group('Tools', tools), sep(), group('Grid', gridg), sep(), group('MIDI', midi_g), sep(),
               group('Record', rec), sep(), group('Tracks', add), sep(), group('Zoom', zoom)))

# ---------------- left rack ----------------
def left_p():
    def sec(title, body, extra=''):
        return ('<section style="%s;padding:10px 8px 10px">'
                '<div style="display:flex;justify-content:space-between;align-items:center;padding:0 6px 8px"><h2 style="margin:0;%s;font-size:11px">%s</h2>%s</div>%s</section>' % (PANEL, SILK, title, extra, body))
    def nt(label, sub, col, on=False):
        st = KEYBASE + (KEY_ON if on else KEY_OFF) + 'height:44px;padding:0 6px;flex-direction:column;gap:1px;'
        return ('<button type="button"%s style="%s"><span style="display:flex;align-items:center;gap:5px"><span aria-hidden="true" style="width:7px;height:7px;border-radius:2px;background:%s"></span>%s</span>'
                '<span style="font-family:Inter,sans-serif;font-weight:400;font-size:9.5px;letter-spacing:0;text-transform:none;color:#9AA3AD">%s</span></button>' % (' aria-pressed="true"' if on else '', st, col, label, sub))
    newt = ('<div style="display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:6px;padding:0 4px">%s%s%s%s</div>'
            '<div style="display:flex;gap:6px;padding:8px 4px 0;align-items:center"><span style="%s;font-size:10px">Count</span>'
            '<div style="%s;width:30px;height:24px;display:flex;align-items:center;justify-content:center;font-size:13px">1</div>%s%s</div>'
            % (nt('MIDI', 'Out to hardware / plug-in', TYPE_COL['MIDI']), nt('Instrument', 'Virtual instrument', TYPE_COL['INST'], True),
               nt('Audio', 'Record / import WAV', TYPE_COL['AUDIO']), nt('Aux / Bus', 'Submix, FX return', TYPE_COL['AUX']),
               SILK, LCD, key('Stereo', True, h=24, fs=11, pad='0 6px'), key('Create', False, h=24, fs=11, pad='0 6px', extra='flex:1;')))
    tr = [('01', 'Flute 1', 'INST'), ('02', 'Violins I', 'INST'), ('03', 'Vln I · Print', 'AUDIO'), ('04', 'Piano', 'INST'), ('05', 'Cellos', 'INST'),
          ('06', 'Contrabass', 'INST'), ('07', 'Djembe', 'MIDI'), ('08', 'Udu · Live', 'AUDIO'), ('09', 'Strings Bus', 'AUX')]
    items = ''.join('<li style="display:flex;align-items:center;gap:7px;padding:4px 6px;border-radius:4px;%s"><span style="font-family:\'Share Tech Mono\',monospace;font-size:10px;color:#7D8792">%s</span>'
                    '<span style="flex:1;font-size:12.5px;color:#E7EBF0">%s</span><span style="font-family:\'Barlow Condensed\',sans-serif;font-weight:700;font-size:9.5px;letter-spacing:.08em;color:%s">%s</span></li>'
                    % ('background:#2a2f35' if n == '02' else '', n, nm, TYPE_COL[t], t) for n, nm, t in tr)
    clips_ = [('Violins I_PRINT_01.wav', 'REC', '#FF5A4E'), ('Udu live_03.wav', 'WAV', '#3DDC84'), ('Udu live_02.wav', 'WAV', '#3DDC84'),
              ('Violins I · Countermelody', 'MIDI', '#4FD1FF'), ('Flute · Chorus melody', 'MIDI', '#4FD1FF'), ('Piano · Pad chords', 'MIDI', '#4FD1FF')]
    cl = ''.join('<li style="display:flex;align-items:center;gap:7px;padding:3px 6px"><span style="flex:1;min-width:0;font-size:12px;color:#D3D9E0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis">%s</span>'
                 '<span style="font-family:\'Share Tech Mono\',monospace;font-size:9.5px;color:%s">%s</span></li>' % (n, c, t) for n, t, c in clips_)
    ul = '<ul style="list-style:none;margin:0;padding:0;display:flex;flex-direction:column;gap:1px">%s</ul>'
    return ('<aside style="flex:0 0 200px;min-width:0;display:flex;flex-direction:column;gap:10px">%s%s%s</aside>'
            % (sec('New track', newt), sec('Tracks', ul % items), sec('Clips', ul % cl, key('Import', False, h=18, fs=10, pad='0 5px'))))

# ---------------- inspector ----------------
def fader(pos, h=150, aria='Fader'):
    ticks = ''
    for i, lab in enumerate(['+6', '0', '−6', '−12', '−24', '−48', '−∞']):
        y = 6 + i * (h - 12) / 6.0
        ticks += ('<span style="position:absolute;right:30px;top:%spx;font-family:\'Share Tech Mono\',monospace;font-size:8.5px;color:#7D8792;line-height:8px">%s</span>'
                  '<span style="position:absolute;left:24px;top:%spx;width:8px;height:1px;background:#4a525c"></span>' % (f(y - 4), lab, f(y)))
    return ('<div style="position:relative;width:60px;height:%dpx">%s'
            '<div style="position:absolute;left:36px;top:0;width:6px;height:%dpx;border-radius:3px;background:#060708;box-shadow:inset 0 1px 3px rgba(0,0,0,.9),0 1px 0 rgba(255,255,255,.07)"></div>'
            '<button type="button" aria-label="%s" style="appearance:none;position:absolute;left:25px;top:%dpx;width:28px;height:16px;padding:0;border-radius:3px;border:1px solid #060708;cursor:pointer;'
            'background:linear-gradient(180deg,#d9dde2 0%%,#9aa1aa 45%%,#6c737c 55%%,#b9bec5 100%%);box-shadow:0 3px 5px rgba(0,0,0,.7)">'
            '<span aria-hidden="true" style="position:absolute;left:3px;right:3px;top:7px;height:1.5px;background:#111"></span></button></div>' % (h, ticks, h, aria, pos))

def vmeter2(l1, l2, n=24):
    return ('<div aria-hidden="true" style="display:flex;gap:2px;padding:3px;background:#0b0c0e;border-radius:3px">'
            '<div style="display:flex;flex-direction:column;gap:2px">%s</div><div style="display:flex;flex-direction:column;gap:2px">%s</div></div>' % (meter_v(l1, n, 4, 5), meter_v(l2, n, 4, 5)))

def inspector_p():
    def sec(title, body, right=''):
        return ('<section style="%s;padding:10px 12px 12px;display:flex;flex-direction:column;gap:9px">'
                '<div style="display:flex;justify-content:space-between;align-items:center"><h2 style="margin:0;%s;font-size:11px">%s</h2>%s</div>%s</section>' % (PANEL, SILK, title, right, body))
    slot = lambda on, n, t: ('<button type="button" aria-label="Insert %s %s" style="appearance:none;display:flex;align-items:center;gap:7px;width:100%%;padding:0;border:0;background:none;cursor:pointer">%s'
                             '<span style="font-family:\'Share Tech Mono\',monospace;font-size:10px;color:#7D8792;width:9px">%s</span><span style="%s;flex:1;text-align:left;padding:3px 7px;font-size:11px">%s</span></button>'
                             % (n, t, led(on, '#3DDC84', '#3DDC8499'), n, LCD, t))
    inserts = ''.join(slot(on, n, t) for on, n, t in [(True, 'A', 'SAMPLER · STRINGS'), (True, 'B', 'EQ-7 BAND'), (True, 'C', 'COMP · OPTO'), (False, 'D', '—'), (False, 'E', '—')])
    sends = ('<div style="display:flex;gap:10px;justify-content:space-around">%s%s%s</div>'
             % (knob('Hall', '−12.0', -30, 26, 'Send to Hall Verb −12 dB'), knob('Plate', '−∞', -135, 26, 'Send to Plate off'), knob('Pan', 'L30', -70, 26, 'Pan left 30')))
    strip = ('<div style="display:flex;flex-direction:column;gap:5px"><span style="%s;font-size:10px">Inserts</span>%s</div>'
             '<div style="display:flex;flex-direction:column;gap:5px"><span style="%s;font-size:10px">Sends · Pan</span>%s</div>'
             '<div style="display:flex;align-items:flex-end;justify-content:center;gap:12px">%s%s'
             '<div style="display:flex;flex-direction:column;gap:6px;align-items:stretch">'
             '<div style="%s;font-size:13px;padding:3px 8px;text-align:center">−4.5 dB</div>%s%s%s'
             '<div style="%s;font-size:10px;padding:3px 6px;text-align:center">OUT STR BUS</div></div></div>'
             % (SILK, inserts, SILK, sends, fader(44, 150, 'Violins I fader −4.5 dB'), vmeter2(17, 16),
                LCD, key('Read', True, h=22, fs=11, ledon=True), key('Touch', False, h=22, fs=11, ledon=False), key('Latch', False, h=22, fs=11, ledon=False), LCD))
    cb = lambda i, n, on: ('<div style="display:flex;align-items:center;gap:8px"><input type="checkbox" id="pr%d"%s style="accent-color:var(--led);width:14px;height:14px;margin:0">'
                           '<label for="pr%d" style="font-size:12.5px;color:#D3D9E0;flex:1">%s</label></div>' % (i, ' checked' if on else '', i, n))
    srcs = ''.join(cb(i, n, on) for i, (n, on) in enumerate([('Flute 1', False), ('Violins I', True), ('Piano', False), ('Cellos', True), ('Contrabass', True), ('Djembe', False)]))
    def opt_row(label, opts, sel):
        return ('<div style="display:flex;align-items:center;gap:6px"><span style="%s;font-size:10px;width:52px">%s</span>%s</div>'
                % (SILK, label, ''.join(key(o, o == sel, h=24, fs=11, pad='0 6px', extra='flex:1;') for o in opts)))
    recbtn = ('<button type="button" style="%sheight:40px;padding:0 10px;font-size:13px;letter-spacing:.12em;background:linear-gradient(180deg,#ff6b60,#d63a30);color:#1a0806;'
              'box-shadow:inset 0 1px 0 rgba(255,255,255,.45),0 2px 0 #08090a,0 0 16px rgba(255,90,78,.55)">'
              '<span aria-hidden="true" style="width:10px;height:10px;border-radius:50%%;background:#1a0806"></span>Record score to audio</button>' % KEYBASE)
    printp = ('<div style="display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:4px 10px">%s</div>%s%s%s'
              '<div style="display:flex;align-items:center;gap:6px"><span style="%s;font-size:10px;width:52px">Format</span><div style="%s;flex:1;font-size:11.5px;padding:4px 8px">WAV · 48 kHz · 24-bit</div></div>'
              '<div style="display:flex;gap:6px">%s%s</div>%s'
              '<div style="%s;padding:6px 9px;font-size:11px;display:flex;flex-direction:column;gap:4px"><div style="display:flex;justify-content:space-between"><span>PRINTING 1 / 3 · VIOLINS I</span><span>44|3|941</span></div>'
              '<div aria-hidden="true" style="height:4px;border-radius:2px;background:#123;overflow:hidden"><div style="width:46%%;height:100%%;background:var(--lcd);box-shadow:0 0 6px var(--glow)"></div></div></div>'
              % (srcs, opt_row('Range', ['Selection', 'Song'], 'Song'), opt_row('Output', ['Per track', 'Stems', 'Mix'], 'Per track'), opt_row('Mode', ['Real-time', 'Offline'], 'Real-time'),
                 SILK, LCD, key('Include FX', True, h=24, fs=11, ledon=True, extra='flex:1;'), key('Add to session', True, h=24, fs=11, ledon=True, extra='flex:1;'), recbtn, LCD))
    master = ('<div style="display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:6px">'
              '<div style="%s;padding:5px 8px;display:flex;flex-direction:column"><span style="font-size:9.5px;opacity:.65;letter-spacing:.1em">INTEGRATED</span><span style="font-size:17px">−14.2 LUFS</span></div>'
              '<div style="%s;padding:5px 8px;display:flex;flex-direction:column"><span style="font-size:9.5px;opacity:.65;letter-spacing:.1em">TRUE PEAK</span><span style="font-size:17px">−1.0 dBTP</span></div></div>'
              '<div style="display:flex;flex-direction:column;gap:4px">%s</div>'
              '<div style="display:flex;gap:6px">%s%s</div>'
              % (LCD, LCD, ''.join(slot(True, n, t) for n, t in [('A', 'MASTER EQ'), ('B', 'GLUE COMP'), ('C', 'LIMITER · −1.0 CEIL')]),
                 key('Target −14', True, h=26, fs=11, ledon=True, extra='flex:1;'), key('Export master', False, h=26, fs=11, extra='flex:1;')))
    return ('<aside style="flex:0 0 288px;min-width:0;display:flex;flex-direction:column;gap:10px">%s%s%s</aside>'
            % (sec('Channel · Violins I', strip, chip('INST')), sec('Print score to audio', printp, '<span style="display:flex;align-items:center;gap:5px"><span style="%s;font-size:10px">Armed</span>%s</span>' % (SILK, led(True, '#FF5A4E', '#FF5A4E99'))),
               sec('Master', master)))

# ---------------- mixer deck ----------------
def mixer():
    strips = [('FLUTE 1', '#4FA8E8', 'L12', 70, '−6.0', 13, False, False, False), ('VIOLINS I', '#E0699A', 'L30', 60, '−4.5', 17, False, False, False),
              ('VLN I PRINT', '#FF5A4E', 'L30', 60, '−4.5', 18, True, False, False), ('PIANO', '#A78BFA', 'C', 76, '−8.0', 12, False, False, False),
              ('CELLOS', '#2EC4B6', 'R20', 64, '−5.0', 15, False, False, False), ('CONTRABASS', '#8FC85A', 'R10', 72, '−7.5', 13, False, False, False),
              ('DJEMBE', '#E46F4B', 'L20', 80, '−9.0', 16, False, True, False), ('UDU LIVE', '#D98C5F', 'R35', 84, '−10.0', 14, False, False, False),
              ('STR BUS', '#F5D547', 'C', 54, '−2.5', 18, False, False, False), ('HALL VERB', '#9CA3AF', 'C', 66, '−12.0', 10, False, False, True)]
    out = ''
    for name, col, pan, fpos, db, lvl, rec, solo, mute in strips:
        dots = ''.join(led(True, '#3DDC84', '#3DDC8499', 5) for _ in range(3))
        out += ('<div style="flex:1 1 84px;min-width:84px;display:flex;flex-direction:column;align-items:center;gap:6px;padding:8px 4px;border-radius:6px;background:linear-gradient(180deg,#22262b,#1a1d21);box-shadow:inset 0 0 0 1px #08090a,inset 0 1px 0 rgba(255,255,255,.05)">'
                '<div aria-label="3 inserts active" style="display:flex;gap:4px">%s</div>%s'
                '<div style="display:flex;align-items:flex-end;gap:2px">%s%s</div>'
                '<div style="%s;font-size:11px;padding:2px 6px">%s</div>'
                '<div style="display:flex;gap:3px">%s</div>'
                '<div style="width:100%%;box-sizing:border-box;padding:3px 4px;border-radius:3px;background:%s;color:#0E1013;font-family:\'Barlow Condensed\',sans-serif;font-weight:700;font-size:10.5px;letter-spacing:.06em;text-align:center;white-space:nowrap;overflow:hidden">%s</div></div>'
                % (dots, knob('', pan, {'C': 0}.get(pan, -40 if pan.startswith('L') else 40), 24, 'Pan ' + name + ' ' + pan),
                   fader(fpos, 120, name.title() + ' fader ' + db + ' dB'), vmeter2(lvl, max(0, lvl - 1), 20), LCD, db,
                   rsm('S', solo, '#FFC24A', 'Solo ' + name) + rsm('M', mute, '#4FD1FF', 'Mute ' + name), col, name))
    mst = ('<div style="flex:0 0 120px;display:flex;flex-direction:column;align-items:center;gap:6px;padding:8px 6px;border-radius:6px;background:linear-gradient(180deg,#2a2e33,#1e2125);box-shadow:inset 0 0 0 1px var(--led),0 0 12px var(--ledglow)">'
           '<div style="display:flex;gap:4px">%s</div><span style="%s;font-size:10px">Master</span>'
           '<div style="display:flex;align-items:flex-end;gap:2px">%s%s</div>'
           '<div style="%s;font-size:11px;padding:2px 6px">−1.0 dBTP</div>'
           '<div style="width:100%%;box-sizing:border-box;padding:3px 4px;border-radius:3px;background:var(--led);color:#0E1013;font-family:\'Barlow Condensed\',sans-serif;font-weight:700;font-size:10.5px;letter-spacing:.1em;text-align:center">MASTER 1–2</div></div>'
           % (''.join(led(True, '#3DDC84', '#3DDC8499', 5) for _ in range(3)), SILK, fader(52, 150, 'Master fader 0 dB'), vmeter2(21, 20, 26), LCD))
    head = ('<div style="flex:1 1 100%%;display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap">'
            '<div style="display:flex;align-items:center;gap:8px"><span style="%s">Mixer</span>%s%s</div>'
            '<div style="display:flex;align-items:center;gap:6px"><span style="%s;font-size:10px">Show</span>%s%s%s%s</div></div>'
            % (SILK, key('Mixer', True, h=24, fs=11), key('Keyboard', False, h=24, fs=11), SILK,
               key('Inserts', True, h=22, fs=10.5, ledon=True), key('Sends', True, h=22, fs=10.5, ledon=True), key('I/O', False, h=22, fs=10.5, ledon=False), key('Comments', False, h=22, fs=10.5, ledon=False)))
    return ('<div style="%s;display:flex;flex-wrap:wrap;gap:10px;padding:12px 16px;position:relative">%s%s%s'
            '<div style="flex:1 1 100%%;display:flex;gap:8px;align-items:stretch;overflow-x:auto">%s%s</div></div>'
            % (PANEL, screw('top:6px;left:6px'), screw('top:6px;right:6px', 60), head, out, mst))

edit_window = ('<main style="%s;flex:999 1 640px;min-width:0;padding:10px;display:flex;flex-direction:column">'
               '<div style="display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap;margin-bottom:8px"><div style="display:flex;align-items:center;gap:16px;flex-wrap:wrap"><h1 style="margin:0;%s;font-size:12px;color:#C9D0D8">Edit · Sunset at the Border 2026</h1>%s</div>'
               '<span style="display:flex;gap:6px">%s%s%s</span></div>'
               '%s'
               '<div style="overflow-x:auto;border:1px solid #08090a;border-radius:4px;background:#08090a">'
               '<div style="width:%dpx">%s%s</div></div></main>'
               % (PANEL, SILK, stage_bar('Mix'), key('Link sel', True, h=22, ledon=True, fs=11), key('Waveforms', True, h=22, ledon=True, fs=11), key('Automation', True, h=22, ledon=True, fs=11),
                  navigator(), HDR_W + LANE_W, ruler(), ''.join(rows)))

body = '<div style="display:flex;flex-wrap:wrap;gap:10px;align-items:flex-start">%s%s%s</div>' % (left_p(), edit_window, inspector_p())

root_style = ('--led: {{led}}; --ledglow: {{ledGlow}}; --lcd: {{lcd}}; --glow: {{glow}}; --paper: #14171B; --ink: #E7EBF0; --staff: #525B66; --grid: rgba(255,255,255,0.04); '
              'box-sizing: border-box; min-height: 1660px; padding: 14px; display: flex; flex-direction: column; gap: 10px; background: #0c0d0f; '
              'font-family: Inter, sans-serif; color: #D3D9E0')

html = '''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Composer 2 – Producer view</title>
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
''' + top_deck() + '\n' + toolbar_p() + '\n' + body + '\n' + mixer() + '\n' + status() + '''
</div>
</x-dc>
<script type="text/x-dc" data-dc-script data-props='{"led":{"editor":"color","default":"#FFB547","options":["#FFB547","#FF5A4E","#4FD1FF","#9BF07A"],"section":"Hardware"},"lcd":{"editor":"color","default":"#6EE7D2","options":["#6EE7D2","#9BF07A","#FFC266","#9CC2FF"],"section":"Hardware"},"$preview":{"width":1680,"height":1660}}'>
class Component extends DCLogic {
  renderVals() {
    const led = this.props.led ?? '#FFB547';
    const lcd = this.props.lcd ?? '#6EE7D2';
    const open = !(this.state && this.state.vlnCollapsed);
    const rgba = (h, a) => {
      const n = parseInt(String(h).replace('#', '').slice(0, 6), 16);
      return 'rgba(' + ((n >> 16) & 255) + ',' + ((n >> 8) & 255) + ',' + (n & 255) + ',' + a + ')';
    };
    return {
      led: led,
      ledGlow: rgba(led, 0.6),
      lcd: lcd,
      glow: rgba(lcd, 0.55),
      vlnOpen: open,
      vlnClosed: !open,
      toggleVln: () => this.setState({ vlnCollapsed: open })
    };
  }
}
</script>
</body>
</html>
'''
open(OUT, 'w').write(html)
print(len(html))
