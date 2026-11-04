"""Score openbw-observer-eval logs: how much of the action was on screen, and how calm the camera was.

    python metrics.py out/*.jsonl

eff        damage on screen / the most one view could have caught (per 2 s window)
empty      share of time with no player units in view
dmg_on     damage on screen / all damage
fights     damage episodes >= 300 hp; missed = never on screen; lat_med = median game
           seconds from an episode's start until it is on screen
cuts/min   camera jumps of > 3/4 view (real time); short<2s = shots under 2 s
moving     share of time the camera pans; pan = px/s; rev/min = pan direction reversals
"""
import json, sys, math, collections

def load(path):
    global units
    hdr, cams, dmg = None, [], []
    units = []
    for line in open(path):
        o = json.loads(line)
        if "view" in o: hdr = o
        elif "t" in o: cams.append((o["t"], o["f"], o["x"], o["y"])); units.append((o.get("n", 1), o.get("na", 0)))
        else: dmg.append((o["d"], o["x"], o["y"], o["a"]))
    return hdr, cams, dmg

def score(path):
    hdr, cams, dmg = load(path)
    vw, vh = hdr["view"]
    # camera at each frame: last camera sample with f <= frame
    cam_at = {}
    for t, f, x, y in cams: cam_at[f] = (x, y, t)
    frames = sorted(cam_at)
    import bisect
    def cam(f):
        i = bisect.bisect_right(frames, f) - 1
        return cam_at[frames[max(i, 0)]]
    tot = on = 0.0
    margin = 0  # strictly within the view
    for f, x, y, a in dmg:
        cx, cy, _ = cam(f)
        tot += a
        if cx <= x < cx + vw and cy <= y < cy + vh: on += a
    # fights: cluster damage into episodes (gap > 5 s game time or > 1.5 views apart)
    eps = []
    for f, x, y, a in dmg:
        for e in eps:
            if f - e["last"] < 120 and abs(x - e["x"]) < vw * 1.5 and abs(y - e["y"]) < vh * 1.5:
                e["last"] = f; e["a"] += a; e["pts"].append((f, x, y, a)); break
        else:
            eps.append({"first": f, "last": f, "x": x, "y": y, "a": a, "pts": [(f, x, y, a)]})
    big = [e for e in eps if e["a"] >= 300]
    lat = []
    missed = 0
    for e in big:
        seen = None
        for f, x, y, a in e["pts"]:
            cx, cy, t = cam(f)
            if cx <= x < cx + vw and cy <= y < cy + vh: seen = f; break
        if seen is None: missed += 1
        else: lat.append((seen - e["first"]) * 42 / 1000)
    # camera motion (real time)
    cuts, moving, rev, pan = 0, 0, 0, 0.0
    shots, last_cut_t = [], cams[0][0]
    prev = None; prev_v = None
    for t, f, x, y in cams:
        if prev:
            dx, dy = x - prev[2], y - prev[3]
            d = math.hypot(dx, dy)
            if d > vw * 0.75:
                cuts += 1; shots.append(t - last_cut_t); last_cut_t = t
            elif d > 0:
                moving += 1; pan += d
                if prev_v and (dx * prev_v[0] + dy * prev_v[1]) < 0: rev += 1
                prev_v = (dx, dy)
        prev = (t, f, x, y)
    T = cams[-1][0]
    # oracle: per 2 s game window, the most damage one view could capture (upper bound)
    win = collections.defaultdict(list)
    for f, x, y, a in dmg: win[f // 48].append((x, y, a))
    oracle = 0.0
    for evs in win.values():
        best = 0
        for cx, cy, _ in evs:
            for ox, oy in ((-vw/2, -vh/2), (-vw*0.1, -vh/2), (-vw*0.9, -vh/2), (-vw/2, -vh*0.1), (-vw/2, -vh*0.9)):
                x0, y0 = cx + ox, cy + oy
                s = sum(a for x, y, a in evs if x0 <= x < x0 + vw and y0 <= y < y0 + vh)
                best = max(best, s)
        oracle += best
    empty = sum(1 for n, na in units if n == 0) / len(units)
    short = sum(1 for s in shots if s < 2.0)
    return dict(eff=on / oracle if oracle else 0, empty=empty, dmg_on=on / tot if tot else 0, fights=len(big), missed=missed,
                lat_med=sorted(lat)[len(lat)//2] if lat else None,
                cuts_pm=cuts / T * 60, short_shots=short, moving=moving / len(cams),
                pan_px_s=pan / T, reversals_pm=rev / T * 60, T=T)

if __name__ == "__main__":
    for p in sys.argv[1:]:
        r = score(p)
        print(f"{p.split('/')[-1]:20s} eff={r['eff']:.2f} empty={r['empty']:.2f} dmg_on={r['dmg_on']:.2f} fights={r['fights']} missed={r['missed']} lat_med={r['lat_med']} "
              f"cuts/min={r['cuts_pm']:.1f} short<2s={r['short_shots']} moving={r['moving']:.2f} pan={r['pan_px_s']:.0f}px/s rev/min={r['reversals_pm']:.1f}")

