"""Score openbw-observer-eval logs: how much of the action was on screen, and how calm the camera was.

    python metrics.py out/*.jsonl

lag        per fight (>= 600 hp), game seconds from its first damage until it is framed in the view
           (clipped at -3: camera already there; unframed within 10 s counts as 10); on<=1s = share with lag <= 1 s
still      share of time the camera moves under 8 px/s; maxstill = longest such stretch (real s);
           rms_acc = RMS acceleration (px/s^2) of non-cut steps (lower = smoother)
eff        damage on screen / the most one view could have caught (per 2 s window)
cerr       damage-weighted distance of damage from the view centre, in view sizes
           (damage within 1.5 views of the camera): how well the action is centred
empty      share of time with no player units in view
dmg_on     damage on screen / all damage
fights     damage episodes >= 300 hp; missed = never on screen; lat_med = median game
           seconds from an episode's start until it is on screen
cuts/min   camera jumps of > 3/4 view (real time); short<2s = shots under 2 s
moving     share of time the camera pans; pan = px/s; rev/min = pan direction reversals
tech       important tech starts (logged by the eval) on screen within 24 game s of starting
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
        elif "d" in o: dmg.append((o["d"], o["x"], o["y"], o["a"]))
    return hdr, cams, dmg

def tech_framed(path, window=571):
    """(framed, total): tech starts whose site is in the view within `window` frames."""
    hdr, cams, tech = None, [], []
    for line in open(path):
        o = json.loads(line)
        if "view" in o: hdr = o
        elif "t" in o: cams.append((o["f"], o["x"], o["y"]))
        elif "tech" in o: tech.append((o["tech"], o["x"], o["y"]))
    vw, vh = hdr["view"]
    framed = sum(1 for f, x, y in tech if any(f <= cf <= f + window and cx <= x < cx + vw and cy <= y < cy + vh for cf, cx, cy in cams))
    return framed, len(tech)

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
    cerr = cw = 0.0  # damage-weighted distance from view centre, for damage near the view
    margin = 0  # strictly within the view
    for f, x, y, a in dmg:
        cx, cy, _ = cam(f)
        tot += a
        if cx <= x < cx + vw and cy <= y < cy + vh: on += a
        ex, ey = (x - cx - vw / 2) / vw, (y - cy - vh / 2) / vh
        if abs(ex) < 1.5 and abs(ey) < 1.5:
            cerr += a * math.hypot(ex, ey); cw += a
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
    # lag: per fight, the reference point is the damage centroid of its first 3 game seconds;
    # lag = game seconds from the fight's first damage until that point is inside the view
    # (shrunk to 80%) and stays there for 1.5 s. Negative = the camera was there before the
    # fight; a fight not framed within 10 s counts as 10 (censored).
    CAP = 10.0  # also the search window either side of the first damage
    lags = []
    for e in [e for e in eps if e["a"] >= 600]:
        f0 = e["first"]
        near = [(x, y, a) for f, x, y, a in e["pts"] if f - f0 <= 72]
        w = sum(a for _, _, a in near)
        rx, ry = sum(x * a for x, _, a in near) / w, sum(y * a for _, y, a in near) / w
        def inview(f):
            cx, cy, _ = cam(f)
            return abs(rx - cx - vw / 2) < vw * 0.4 and abs(ry - cy - vh / 2) < vh * 0.4
        lag = CAP
        f = f0 - int(CAP * 1000 / 42)
        end = f0 + int(CAP * 1000 / 42)
        while f <= end:
            if inview(f) and all(inview(g) for g in range(f, f + 36, 6)):
                lag = max((f - f0) * 42 / 1000, -3.0); break
            f += 3
        lags.append(lag)
    lags.sort()
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
    # smoothness: speed and acceleration of non-cut steps; "still" = share of time the camera
    # moves slower than 8 px/s; stalls = longest real-time stretch it is still.
    sp, acc_sq, n_acc, still, longest, run = [], 0.0, 0, 0, 0.0, 0.0
    # acceleration from 0.2 s velocity estimates (per-sample differences are dominated by the
    # 1 px position quantisation), skipping windows that hold a cut
    pw = None
    for i in range(0, len(cams) - 12, 12):
        seg = cams[i:i + 13]
        if any(math.hypot(seg[j+1][2]-seg[j][2], seg[j+1][3]-seg[j][3]) > vw * 0.75 for j in range(12)): pw = None; continue
        dtw = seg[-1][0] - seg[0][0]
        vv = ((seg[-1][2]-seg[0][2]) / dtw, (seg[-1][3]-seg[0][3]) / dtw)
        if pw: acc_sq += ((vv[0]-pw[0])**2 + (vv[1]-pw[1])**2) / dtw**2; n_acc += 1
        pw = vv
    pv = None
    for i in range(1, len(cams)):
        dt = cams[i][0] - cams[i-1][0]
        dx, dy = cams[i][2] - cams[i-1][2], cams[i][3] - cams[i-1][3]
        if dt <= 0 or math.hypot(dx, dy) > vw * 0.75: pv = None; continue
        v = (dx / dt, dy / dt)
        pv = v
        if math.hypot(*v) < 8: still += 1; run += dt; longest = max(longest, run)
        else: run = 0.0
    return dict(lag_med=lags[len(lags)//2] if lags else None, lag_p90=lags[int(len(lags)*0.9)] if lags else None, lag_mean=sum(lags)/len(lags) if lags else None, early=sum(1 for l in lags if l <= 1.0)/len(lags) if lags else 0,
                still=still / len(cams), longest_still=longest, jerk=math.sqrt(acc_sq / n_acc) if n_acc else 0, cerr=cerr / cw if cw else 0, eff=on / oracle if oracle else 0, empty=empty, dmg_on=on / tot if tot else 0, fights=len(big), missed=missed,
                lat_med=sorted(lat)[len(lat)//2] if lat else None,
                cuts_pm=cuts / T * 60, short_shots=short, moving=moving / len(cams),
                pan_px_s=pan / T, reversals_pm=rev / T * 60, T=T)

if __name__ == "__main__":
    for p in sys.argv[1:]:
        r = score(p)
        tf, tn = tech_framed(p)
        print(f"{p.split('/')[-1]:20s} eff={r['eff']:.2f} cerr={r['cerr']:.2f} empty={r['empty']:.2f} dmg_on={r['dmg_on']:.2f} fights={r['fights']} lag med/mean/p90={r['lag_med']:.1f}/{r['lag_mean']:.1f}/{r['lag_p90']:.1f} on<=1s={r['early']:.2f} "
              f"cuts/min={r['cuts_pm']:.1f} short<2s={r['short_shots']} still={r['still']:.2f} maxstill={r['longest_still']:.1f}s rms_acc={r['jerk']:.0f} pan={r['pan_px_s']:.0f}px/s rev/min={r['reversals_pm']:.1f} tech={tf}/{tn}")

