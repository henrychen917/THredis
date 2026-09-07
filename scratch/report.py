#!/usr/bin/env python3
"""report.py -- build the lane report (HTML artifact) from the measurement files.
Usage: report.py OUT.html  (reads $SP/fd-matrix2.csv, fd-matrix-mk15.csv, fd-final.log, flip probes)"""
import csv, html, os, re, statistics as st, subprocess, sys
from collections import defaultdict

SP = "/tmp/claude-1000/-home-user-Projects/ee6eb242-5302-49cf-b767-1a2d8d8f0f61/scratchpad"
WT = "/home/user/Projects/wt-flipdamp"
OUT = sys.argv[1]
COMMIT = subprocess.run(["git", "-C", WT, "rev-parse", "--short=9", "HEAD"], capture_output=True, text=True).stdout.strip()
BASE = "e902c67d5"

def f(x):
    try: return float(x)
    except Exception: return 0.0
def i(x):
    try: return int(float(x))
    except Exception: return 0

def load(path):
    if not os.path.exists(path): return []
    return [r for r in csv.DictReader(open(path)) if r.get("rate") not in (None, "", "MISSING")]

def timeline_flips(arm, wl, rnd):
    """(flips before first anchor, flips after first anchor, first-anchor second) from the ab.sh timeline."""
    p = f"{SP}/fd-tl-{arm}-{wl}-{rnd}.txt"
    if not os.path.exists(p): return (None, None, None)
    first = None; at_anchor = None; last = 0
    for line in open(p):
        fs = line.split()
        if not fs: continue
        t = int(fs[0]); d = dict(kv.split(":", 1) for kv in fs[1:] if ":" in kv and not kv.startswith("foreign"))
        fc = i(d.get("flip_completed", 0)); last = fc
        if first is None and d.get("flipctl_state") == "anchored": first, at_anchor = t, fc
    if at_anchor is None: return (last, 0, None)
    return (at_anchor, last - at_anchor, first)

def summarize(rows, arms):
    """per (wl, arm): dict of aggregates; rates relative to the controller-OFF cells of the same wl."""
    g = defaultdict(list); wls = []
    for r in rows:
        g[(r["wl"], r["arm"])].append(r)
        if r["wl"] not in wls: wls.append(r["wl"])
    out = []
    for wl in wls:
        off = [f(r["rate"]) for a in ("pol0a", "pol0b") for r in g.get((wl, a), [])]
        off_mean = st.mean(off) if off else 0
        null = (max(off) - min(off)) / off_mean * 100 if off_mean and len(off) > 1 else 0
        for arm in arms:
            rs = g.get((wl, arm))
            if not rs: continue
            rates = [f(r["rate"]) for r in rs]
            pre = post = 0; anchors = []
            for r in rs:
                a, b, t = timeline_flips(arm, wl, r["round"])
                if a is not None: pre += a; post += b
                if t is not None: anchors.append(t)
            out.append(dict(wl=wl, arm=arm, n=len(rs), rate=st.mean(rates), lo=min(rates), hi=max(rates),
                            rel=(st.mean(rates) / off_mean - 1) * 100 if off_mean else 0, null=null,
                            comp=sum(i(r["comp"]) for r in rs), xfer=sum(i(r["xfer"]) for r in rs),
                            trig=sum(i(r["trig"]) for r in rs), hold=sum(i(r["hold"]) for r in rs),
                            rt=sum(i(r.get("rt", 0)) for r in rs), pre=pre, post=post,
                            anchor=",".join(sorted(set(r["anchor"] for r in rs))),
                            live=",".join(sorted(set(r["live"] for r in rs))),
                            p99=st.median([f(r["p99"]) for r in rs]),
                            anchor_s=(st.mean(anchors) if anchors else None)))
    return out

WLNAME = {"mk": "multi-key MSET8+MGET8 1:1, p32 (the defect regime)", "sk1:1": "single-key SET:GET 1:1, p32",
          "sk9:1": "single-key SET:GET 9:1, p32", "get": "pure GET, p32 (null)"}
ARMNAME = {"base1": "PRE  base e902c67d5, --flip-auto 1", "pol1": f"POST {COMMIT}, --flip-auto 1",
           "pol0a": f"OFF  {COMMIT}, --flip-auto 0 (a)", "pol0b": f"OFF  {COMMIT}, --flip-auto 0 (b)",
           "guard1": "GUARD 66d4c13a3 (hold-only guard), --flip-auto 1"}
ARMCLASS = {"base1": "pre", "pol1": "post", "pol0a": "off", "pol0b": "off", "guard1": "guard"}

def table(summ, caption, arms):
    h = [f'<figure class="tbl"><figcaption>{caption}</figcaption><div class="scroll"><table>',
         "<thead><tr><th>regime</th><th>arm</th><th class=n>cells</th><th class=n>ops/s mean</th><th class=n>min – max</th>"
         "<th class=n>vs OFF</th><th class=n>flips</th><th class=n>after anchor</th><th class=n>clients moved</th>"
         "<th class=n>triggers</th><th class=n>holds</th><th class=n>anchor</th><th class=n>live at end</th><th class=n>p99 ms</th></tr></thead><tbody>"]
    last_wl = None
    for s in summ:
        if s["arm"] not in arms: continue
        wl_cell = f'<td class="wl" rowspan="{sum(1 for x in summ if x["wl"]==s["wl"] and x["arm"] in arms)}">{html.escape(WLNAME.get(s["wl"], s["wl"]))}</td>' if s["wl"] != last_wl else ""
        last_wl = s["wl"]
        rel = "" if s["arm"] in ("pol0a", "pol0b") else f'{s["rel"]:+.1f}%'
        flips_cls = ' class="n bad"' if s["comp"] else ' class="n good"'
        post_cls = ' class="n bad"' if s["post"] else ' class="n"'
        h.append(f'<tr class="{ARMCLASS[s["arm"]]}">{wl_cell}<td class="arm"><code>{html.escape(ARMNAME[s["arm"]])}</code></td>'
                 f'<td class=n>{s["n"]}</td><td class=n><b>{s["rate"]/1000:,.0f}k</b></td><td class=n>{s["lo"]/1000:,.0f}–{s["hi"]/1000:,.0f}k</td>'
                 f'<td class=n>{rel}</td><td{flips_cls}>{s["comp"]}</td><td{post_cls}>{s["post"]}</td><td class=n>{s["xfer"]}</td>'
                 f'<td class=n>{s["trig"]}</td><td class=n>{s["hold"]}</td><td class=n>{s["anchor"]}</td><td class=n>{s["live"]}</td><td class=n>{s["p99"]:.0f}</td></tr>')
    h.append("</tbody></table></div></figure>")
    return "\n".join(h)

# ---- data --------------------------------------------------------------------------------------
# matrix3 is the matrix taken with the binary this report describes; matrix2 was taken before
# the baseline-band fix and is kept only as raw history (fd-matrix2-prev.csv).
m2 = load(f"{SP}/fd-matrix3.csv") or load(f"{SP}/fd-matrix2.csv")
summ2 = summarize(m2, ["base1", "pol1", "pol0a", "pol0b"])
mk15 = load(f"{SP}/fd-matrix-mk15.csv")
summ1 = summarize(mk15, ["base1", "guard1", "pol1", "pol0a", "pol0b"])
nulls = sorted(set((s["wl"], s["null"]) for s in summ2))

# The run log is a CHAIN of logs (final.sh aborted on the box marker, finalw.sh resumed it, ver.sh
# finished the night). Reading only the first one is why the first report showed 0 hold/battery/
# differ lines while all three had run.

# ---- tonight's verification evidence ------------------------------------------------------------
import glob as _glob

def ctl_rows():
    """tests/flipctl.py, the gate row that failed on this branch: (tag, verdict, detail)."""
    out = []
    for path in sorted(_glob.glob(f"{SP}/fd-ctl-*.txt")):
        tag = os.path.basename(path)[7:-4]
        text = open(path, errors="replace").read()
        rc = re.search(r"^RC=(\d+)$", text, re.M)
        if "ok: flipctl functional acceptance" in text:
            verdict, detail = "PASS", next((l for l in text.splitlines() if l.startswith("anchored off-rail") or "anchored off-rail" in l), "")
        else:
            verdict = "FAIL"
            detail = next((l.strip() for l in text.splitlines() if "AssertionError" in l), "")
        anchor_line = re.search(r"^anchor=(\d+):(\d+)$", text, re.M)
        out.append((tag, verdict, (rc.group(1) if rc else "?"), detail[:200], anchor_line.group(0) if anchor_line else ""))
    return out

def kvline(path):
    """One-line `k=v k=v` result file -> dict, first field is the tag."""
    if not os.path.exists(path): return None
    parts = open(path).read().split()
    if not parts: return None
    d = {"tag": parts[0]}
    for token in parts[1:]:
        if "=" in token:
            k, v = token.split("=", 1); d[k] = v
    return d

fire = [d for d in (kvline(f"{SP}/fd-fire-{t}.txt") for t in
                    ("fix-22-off", "base-31-off", "base-31-on", "fix-31-off", "fix-31-on")) if d]
perf = [d for d in (kvline(f"{SP}/fd-perf-{t}.txt") for t in
                    ("base0-1", "base0-2", "fix0-1", "fix0-2", "fix1-1", "fix1-2")) if d]

def perf_mean(prefix, key):
    vals = [float(d[key]) for d in perf if d["tag"].startswith(prefix) and key in d]
    return st.mean(vals) if vals else 0

# The run log is a CHAIN (final.sh aborted on the box marker, finalw.sh resumed it, ver.sh/ver2.sh
# finished the night) and the two generations PREFIX their lines differently -- ver.sh timestamps
# every line, so patterns anchored at "HOLD"/"DIFFER" silently kept showing the previous night's
# rows. Read tonight's logs first and fall back to the older ones only when a category is missing.
def read_logs(*names):
    return "".join(open(f"{SP}/{n}").read() for n in names if os.path.exists(f"{SP}/{n}"))
ver_log = read_logs("fd-ver.log", "fd-ver2.log")
old_log = read_logs("fd-final.log", "fd-final2.log")
TS = r"^(?:\d\d:\d\d:\d\d )?"
def grab_in(pattern, text, width=200):
    return [re.sub(r"^\d\d:\d\d:\d\d ", "", m)[:width] for m in re.findall(pattern, text, re.M)]
def grab(pattern, width=200):
    return grab_in(pattern, ver_log, width) or grab_in(pattern, old_log, width)
hold_lines = grab(TS + r"HOLD .*$")
bat_lines = [l for l in grab(TS + r"(?:BAT |SPINPROBE |UNIT )[^\n]*$", 170)
             if "unexpected argument" not in l]  # ver.sh handed spinprobe a path; ver2.sh re-ran it with the pid
mode_lines = grab(TS + r"(?:MODES: |.*--flip-auto 1 boots:)[^\n]*$")
differ = grab(TS + r"DIFFER [^\n]*$")
arms_line = (grab_in(TS + r"ARMS [^\n]*$", ver_log) or [""])[0]

# explicit-flip probe per-second trace (policy binary)
rx = re.compile(r"\[RUN #\d+ +\d+%, +(\d+) secs\].*?(\d+) \(avg: +(\d+)\) ops/sec")
def persec(path):
    if not os.path.exists(path): return {}
    d = {}
    for m in rx.finditer(open(path, errors="replace").read().replace("\r", "\n")): d[int(m.group(1))] = int(m.group(2))
    return d
probe = persec(f"{SP}/fd-flipprobe-pol-mt.txt"); probe_base = persec(f"{SP}/fd-flipprobe-base-mt.txt")

def spark(series, flips=(15, 30), w=640, h=170):
    if not series: return "<p class=muted>(flip probe trace missing)</p>"
    xs = sorted(series); lo, hi = min(xs), max(xs); ymax = max(series.values()) * 1.08
    X = lambda s: 40 + (s - lo) / max(1, hi - lo) * (w - 60); Y = lambda v: 20 + (1 - v / ymax) * (h - 50)
    pts = " ".join(f"{X(s):.1f},{Y(series[s]):.1f}" for s in xs)
    ticks = "".join(f'<line x1="{X(t)}" x2="{X(t)}" y1="{h-30}" y2="{h-26}" class="ax"/><text x="{X(t)}" y="{h-12}" class="tick">{t}s</text>' for t in range(lo - lo % 5, hi + 1, 5))
    grid = "".join(f'<line x1="40" x2="{w-20}" y1="{Y(v)}" y2="{Y(v)}" class="grid"/><text x="36" y="{Y(v)+4}" class="tick r">{v//1000}k</text>' for v in (100000, 200000, 300000, 400000, 500000) if v < ymax)
    marks = "".join(f'<line x1="{X(t)}" x2="{X(t)}" y1="20" y2="{h-30}" class="flip"/><text x="{X(t)+4}" y="32" class="lab">{lab}</text>' for t, lab in zip(flips, ("FLIP 3 1", "FLIP 2 2")))
    return (f'<svg viewBox="0 0 {w} {h}" role="img" aria-label="ops per second around two explicit flips">{grid}{marks}'
            f'<polyline points="{pts}" class="line"/>{ticks}<line x1="40" x2="{w-20}" y1="{h-30}" y2="{h-30}" class="ax"/></svg>')

def phase_rate(series, a, b):
    v = [series[s] for s in series if a <= s <= b]
    return st.mean(v) if v else 0

pre22 = phase_rate(probe, 5, 13); at31 = phase_rate(probe, 17, 29); post22 = phase_rate(probe, 32, 40)
pre22b = phase_rate(probe_base, 5, 13); at31b = phase_rate(probe_base, 17, 29)

css = """
:root{--bg:#F5F7FA;--ink:#1B2330;--muted:#5B6675;--rule:#D9DEE5;--panel:#FFFFFF;--post:#0F766E;--post-soft:#D7EFEC;--pre:#9A4B1F;--pre-soft:#F6E3D8;--off:#4A5568;--good:#0F766E;--bad:#B4461D;--code:#EEF1F5}
@media (prefers-color-scheme: dark){:root:not([data-theme="light"]){--bg:#10151C;--ink:#E6EBF0;--muted:#98A3B0;--rule:#2A3441;--panel:#161D27;--post:#2DD4BF;--post-soft:#123B37;--pre:#E0875A;--pre-soft:#3D2417;--off:#AAB4C0;--good:#2DD4BF;--bad:#F0946A;--code:#1C2530}}
:root[data-theme="dark"]{--bg:#10151C;--ink:#E6EBF0;--muted:#98A3B0;--rule:#2A3441;--panel:#161D27;--post:#2DD4BF;--post-soft:#123B37;--pre:#E0875A;--pre-soft:#3D2417;--off:#AAB4C0;--good:#2DD4BF;--bad:#F0946A;--code:#1C2530}
body{background:var(--bg);color:var(--ink);font-family:"Source Sans 3",system-ui,sans-serif;font-size:16px;line-height:1.5;margin:0}
main{max-width:1120px;margin:0 auto;padding:32px 24px 64px}
h1,h2,h3{font-family:Spectral,Georgia,serif;text-wrap:balance;line-height:1.15;margin:0}
h1{font-size:2.1rem;font-weight:600}h2{font-size:1.45rem;font-weight:600;margin-top:44px}h3{font-size:1.1rem;font-weight:600;margin-top:22px}
p,li{max-width:72ch}p{margin:10px 0}
.eyebrow{font-size:.78rem;letter-spacing:.08em;text-transform:uppercase;color:var(--muted);margin-bottom:8px}
.verdict{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px;margin:22px 0 6px}
.verdict div{background:var(--panel);border:1px solid var(--rule);padding:14px 16px}
.verdict .k{font-size:.78rem;letter-spacing:.06em;text-transform:uppercase;color:var(--muted)}
.verdict .v{font-family:"JetBrains Mono",ui-monospace,monospace;font-size:1.35rem;margin-top:4px;font-variant-numeric:tabular-nums}
.verdict .s{color:var(--muted);font-size:.9rem;margin-top:2px}
code,pre,td.n,th.n,.mono{font-family:"JetBrains Mono",ui-monospace,SFMono-Regular,monospace;font-variant-numeric:tabular-nums}
code{font-size:.85em;background:var(--code);padding:1px 5px;border-radius:3px}
pre{background:var(--code);padding:14px 16px;overflow-x:auto;font-size:.82rem;line-height:1.45;border:1px solid var(--rule)}
figure.tbl{margin:18px 0}figcaption{font-size:.9rem;color:var(--muted);margin-bottom:8px}
.scroll{overflow-x:auto}table{border-collapse:collapse;width:100%;font-size:.88rem;background:var(--panel)}
th,td{padding:7px 10px;border-bottom:1px solid var(--rule);text-align:left;vertical-align:top;white-space:nowrap}
th{font-size:.75rem;letter-spacing:.05em;text-transform:uppercase;color:var(--muted);font-weight:600}
td.n,th.n{text-align:right}td.wl{white-space:normal;min-width:160px;color:var(--muted);font-size:.85rem}
tr.pre td.arm code{background:var(--pre-soft);color:var(--pre)}tr.post td.arm code{background:var(--post-soft);color:var(--post)}tr.guard td.arm code{color:var(--pre)}
td.good{color:var(--good);font-weight:600}td.bad{color:var(--bad);font-weight:600}
.muted{color:var(--muted)}
svg{width:100%;height:auto;max-width:640px;display:block}
.line{fill:none;stroke:var(--post);stroke-width:2}.grid{stroke:var(--rule);stroke-width:1}.ax{stroke:var(--muted);stroke-width:1}
.flip{stroke:var(--pre);stroke-width:1.5;stroke-dasharray:4 3}.tick{fill:var(--muted);font-size:11px;text-anchor:middle;font-family:"JetBrains Mono",monospace}.tick.r{text-anchor:end}.lab{fill:var(--pre);font-size:11px;font-family:"JetBrains Mono",monospace}
.two{display:grid;grid-template-columns:1fr 1fr;gap:28px;align-items:start}@media(max-width:820px){.two{grid-template-columns:1fr}}
ul{padding-left:20px}li{margin:4px 0}
.foot{margin-top:48px;border-top:1px solid var(--rule);padding-top:12px;color:var(--muted);font-size:.85rem}
"""

def li(lines):
    return "".join(f"<li><code>{html.escape(l)}</code></li>" for l in lines) or "<li class=muted>(not run yet)</li>"

mk_post = next((s for s in summ2 if s["wl"] == "mk" and s["arm"] == "pol1"), None)
mk_pre = next((s for s in summ2 if s["wl"] == "mk" and s["arm"] == "base1"), None)
def kv(s, key, fmt="{}"):
    return fmt.format(s[key]) if s else "—"


def simple_table(caption, headers, rows, classes=None):
    h = [f'<figure class="tbl"><figcaption>{caption}</figcaption><div class="scroll"><table><thead><tr>']
    h += [f'<th class=n>{c}</th>' if i else f'<th>{c}</th>' for i, c in enumerate(headers)]
    h.append("</tr></thead><tbody>")
    for n, row in enumerate(rows):
        cls = f' class="{classes[n]}"' if classes else ""
        h.append(f"<tr{cls}>" + "".join(f'<td class=n>{c}</td>' if i else f'<td class="arm">{c}</td>'
                                        for i, c in enumerate(row)) + "</tr>")
    h.append("</tbody></table></div></figure>")
    return "\n".join(h)

_ctl = ctl_rows()
ctl_table = simple_table(
    "tests/flipctl.py (the historical gate invocation: --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2 "
    "--lb-age-sample-rate 1024, 8 server threads on cpus 52-55 + siblings)",
    ["run", "verdict", "rc", "what the row said"],
    [(f"<code>{html.escape(t)}</code>", v, rc, html.escape(d or a or "")) for t, v, rc, d, a in _ctl],
    ["post" if v == "PASS" else "pre" for _, v, _, _, _ in _ctl]) if _ctl else "<p class=muted>(flipctl.py rows not on file)</p>"

fire_table = simple_table(
    "non-vacuity probe — same multi-key load, 60 s, server booted at the split named in the row",
    ["arm", "booted", "flip-auto", "ops/s", "p99 ms", "flips", "clients moved", "triggers", "holds", "anchor", "live at end"],
    [(f"<code>{html.escape(d['tag'])}</code>", d.get("boot", ""), d.get("fa", ""),
      f"<b>{f(d.get('rate', 0))/1000:,.0f}k</b>", f"{f(d.get('p99', 0)):.0f}", d.get("flips", ""),
      d.get("xfer", ""), d.get("trig", ""), d.get("holds", ""), d.get("anchor", ""), d.get("live", ""))
     for d in fire],
    ["post" if d.get("fa") == "1" else "off" for d in fire]) if fire else "<p class=muted>(non-vacuity probe not on file)</p>"

perf_table = simple_table(
    "instructions and cycles per command at a matched offered rate (memtier --rate-limiting), "
    "35 s perf window on the server's own pids after 12 s of warm-up",
    ["arm", "flip-auto", "ops/s", "commands in window", "instr/op", "cycles/op", "IPC", "server cpu-s"],
    [(f"<code>{html.escape(d['tag'])}</code>", d.get("fa", ""), f"{f(d.get('rate', 0))/1000:,.0f}k",
      f"{i(d.get('window_cmds', 0)):,}", f"{f(d.get('instr/op', 0)):.1f}", f"{f(d.get('cycles/op', 0)):.1f}",
      f"{f(d.get('IPC', 0)):.3f}", f"{f(d.get('srv_cpu_s', 0)):.1f}") for d in perf],
    ["pre" if d["tag"].startswith("base") else ("post" if d.get("fa") == "1" else "off") for d in perf]) \
    if perf else "<p class=muted>(instr/op rows not on file)</p>"
if perf:
    _b, _f0, _f1 = perf_mean("base0", "instr/op"), perf_mean("fix0", "instr/op"), perf_mean("fix1", "instr/op")
    perf_table += ("<p>Hot path, controller off: <b>%+.2f%%</b> instr/op (%.1f → %.1f). "
                   "Controller running and holding: <b>%+.2f%%</b> against the same binary with it off "
                   "(%.1f → %.1f). The 3%% always-on budget is the second number.</p>" % (
                       (_f0 / _b - 1) * 100 if _b else 0, _b, _f0,
                       (_f1 / _f0 - 1) * 100 if _f0 else 0, _f0, _f1))

doc = f"""<title>Flip Thrash Fix</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Spectral:wght@500;600&family=Source+Sans+3:wght@400;600&family=JetBrains+Mono:wght@400;600&display=swap">
<style>{css}</style>
<main>
<div class="eyebrow">TomoKV · lane t-flipdamp · 2026-09-05/06 · commit {COMMIT} (PRE = {BASE})</div>
<h1>Flip thrash fix: the controller stops moving what it cannot improve</h1>
<p>Defect under test: <code>--flip-auto 1</code> in split mode lost 19.5% on multi-key with three flips, 993 connections moved and the io:ex target unchanged. Two nights of work: the placement policy that stops the thrash, and the verification that it is not a silent <code>--flip-auto 0</code>. This page is the PRE/POST evidence from the lane rig (four server threads, io:ex 2:2, the closest 4-thread analogue of 18:14), the mechanism, and the exact recipe for acceptance on the quiet box.</p>

<div class="verdict">
<div><div class="k">multi-key, POST flips / clients moved</div><div class="v">{kv(mk_post,'comp')} / {kv(mk_post,'xfer')}</div><div class="s">PRE: {kv(mk_pre,'comp')} flips / {kv(mk_pre,'xfer')} clients in {kv(mk_pre,'n')} cells</div></div>
<div><div class="k">multi-key, POST rate vs controller OFF</div><div class="v">{kv(mk_post,'rel','{:+.1f}%')}</div><div class="s">PRE: {kv(mk_pre,'rel','{:+.1f}%')}; same-binary OFF pair spread {mk_post['null'] if mk_post else 0:.1f}%</div></div>
<div><div class="k">moves after the first anchor (thrash)</div><div class="v">{kv(mk_post,'post')}</div><div class="s">POST, all regimes: {sum(s['post'] for s in summ2 if s['arm']=='pol1')} — PRE: {sum(s['post'] for s in summ2 if s['arm']=='base1')}</div></div>
<div><div class="k">cost of one out-and-back on this rig</div><div class="v">{(at31/pre22-1)*100 if pre22 else 0:+.0f}% at 3:1</div><div class="s">2:2 → 3:1 → 2:2 by explicit FLIP; transient one second each way</div></div>
</div>

<h2>1. PRE / POST matrix</h2>
<p>Four arms interleaved ABBA, 40 s cells, three rounds each, one server boot per cell (so every controller cell includes its boot maneuver). <b>OFF (a)/(b)</b> are the same binary with the same flags: their spread is the rig's noise floor for that regime. Rates are memtier totals; flips and clients moved are the server's own counters; <i>after anchor</i> counts flips completed after the controller first reported <code>anchored</code> — the owner's definition of thrash.</p>
{table(summ2, "4 regimes x 3 rounds x 4 arms, 40 s cells — 4 server threads on cpus 52,53,180,181 (2:2), memtier 8x32 conns on 54-57,182-185, 200k keys, 32 B values, --shards 64 --atomic 1", ["base1","pol1","pol0a","pol0b"])}
<p class="muted">Noise floor (OFF pair spread): {"; ".join(f"{wl} {n:.1f}%" for wl, n in nulls)}. Rate differences inside the floor are not verdicts; the counters are.</p>

<h3>The guard alone was not enough</h3>
<p>The earlier hold-only guard (commit 66d4c13a3: corrected demand share, three-sample hold, pass-depth removed from the trigger) was measured in the first run at 30 s cells. It still flipped out and back in every multi-key cell, and two of its three cells ended with connections stalled for the whole cell (memtier p99 = 30 s), something neither the base nor the policy binary reproduced in the explicit-flip probes. It is superseded, not shipped.</p>
{table(summ1, "first run — 30 s cells, same rig; guard1 = 66d4c13a3", ["base1","guard1","pol1","pol0a","pol0b"])}

<h2>2. What a move costs here</h2>
<div class="two">
<div>{spark(probe)}<p class="muted">Policy binary, controller off, multi-key load; explicit <code>FLIP 3 1</code> at 15 s and <code>FLIP 2 2</code> at 30 s. Ops/s per second from memtier.</p></div>
<div>
<p>At 2:2 the rig runs <b>{pre22/1000:,.0f}k</b> ops/s; at 3:1 it runs <b>{at31/1000:,.0f}k</b> ({(at31/pre22-1)*100 if pre22 else 0:+.0f}%), and it is back to <b>{post22/1000:,.0f}k</b> the second after the return flip. The base binary measures the same ({pre22b/1000:,.0f}k → {at31b/1000:,.0f}k). The flip itself — quiesce, transfer of ~180 connections, re-plan of client weights — costs about one second each way.</p>
<p>Tonight's non-vacuity probe measures the same thing on the binary this page describes and without any explicit FLIP: 530k at 2:2 against 236k booted at 3:1 (&minus;55%), 60 s cells. So the price of a wrong move is the <i>time spent at the wrong split</i>, not the flip mechanics. The old seek paid it three times over: jump past the model's optimum, measure at the bad split for two stabilized windows, step back, measure again, settle. The policy pays it at most once (one stabilized reading, then straight back), and on a stationary workload it does not pay it at all.</p>
</div></div>

<h2>3. Mechanism: three biases, all pointing the same way</h2>
<ul>
<li><b>Demand divided by the wrong count.</b> The placement model divided each role's busy time by its <i>own</i> op counter; the executor counts one op per shard task — 7.6 per MGET8 — so ex looked cheap and the model wanted io ≈ 82%. On 8 threads: 5:3 → 7:1 (0.29M vs 0.95M) → back. That is the 3 flips / 993 clients / unchanged target. Fixed by using busy-time <i>share</i> (the command count cancels). Single-key is exact (one command = one task), which is why the defect was 40× workload-dependent.</li>
<li><b>A trigger the actuator moves.</b> The fingerprint's parse-pass-depth family moved 0.048 on one io step while the mix families moved 4e-7: the controller re-fired on its own last move (sweep-abandon law). Removed from the trigger distance; still dumped for diagnosis.</li>
<li><b>Spin "correction" that treated an empty pass like a task batch.</b> Found today from the controller's own trail: <code>model_io_frac=0.73</code> with <code>model_headroom_ex=0.75</code> on a workload whose measured busy shares were io 0.87 / ex 0.97. The executor enters its busy span on every pass and spins up to 2048 empty passes between batches; <code>busy × (1 − spins/iterations)</code> cut the ex share to a quarter and would have projected 3:1 as +35% — the probe above measures it at −60%. The ex loop now books a pass that found nothing as idle (one local, one branch per pass; counters stay monotone), and the sampler uses raw busy/idle.</li>
<li><b>A confirmation the baseline could outrun.</b> Found tonight, by the gate's own row: the seek confirmed its move by comparing one rate window before the flip with one after, which is only meaningful if the baseline held still in between. Section 5 has the trail and the fix.</li>
</ul>

<h3>The policy (src/core/flip_policy.h, one file)</h3>
<ul>
<li><b>Window + variance.</b> One io-share draw per stabilized reading into a Welford window; every projection is evaluated across the window's ±2 SE interval. Decision is sequential: move when the target's gain at the <i>pessimistic</i> end clears the bar, hold when no split clears it even at the <i>optimistic</i> end, otherwise keep sampling — the server keeps serving at its live split, so time costs nothing but the maneuver's sampling arm. Cap: the existing 30 s boot-deferral bound, in readings.</li>
<li><b>Cost gate in throughput space.</b> R(s) ∝ min(s/f, (N−s)/(1−f)) over every legal split; argmax, never round(N·f) plus an overshoot. The bar is the controller's own learned throughput band × an outcome margin — a gain the seek could not verify cannot pay for the flip it costs.</li>
<li><b>Saturation gate.</b> The capacity model holds only while one role's busiest thread has no headroom. Both roles idling more than the band means the rate is set by something the split does not touch (a paced driver, the closed loop's wake-up latency: measured memtier at 27% of its cores with io 86% / ex 93%) — hold.</li>
<li><b>Verify-or-revert seek.</b> One model-directed flip, one stabilized reading against the origin's; better by the band → anchor, else straight back. The halving-step walk, the overshoot and <code>visited()</code> are gone.</li>
<li><b>Outcome margin.</b> A maneuver that flipped and ended where it began doubles the margin the model must clear (a biased estimator does not get less biased with more samples — the bar rises, not the window); a move that delivered halves it; never past the point where the required gain would exceed 100%.</li>
</ul>
<p>No machine constants: N is the live pool, f and its interval are measured, the band is learned, the cap reuses the boot deferral. No new knob. Both thread modes boot with <code>--flip-auto 1</code> (fused reports the controller unavailable, as before).</p>

<h2>4. Directed test, batteries, differ</h2>
<ul>{li(hold_lines)}</ul>
<p class="muted">tests/flip_multikey_hold.py: negative phase holds the mix constant while parse-pass occupancy sweeps (must not move the controller); positive phase changes the mix for real (must re-maneuver). Measured: the policy binary passes both runs with zero flips and zero transfers; the base binary fails on the first stationary batch, having already moved the split (got 1 flip, live 3:1, want 0).</p>
<ul>{li(bat_lines)}{li(mode_lines)}{li(differ)}</ul>
<p class="muted">Batteries ran on 8 server threads (cpus 52-55 + siblings) so the gate's <code>--ratio 6:2</code> flipctl row keeps its geometry. Unit test <code>build/flipctl-unit</code> carries the defect's own numbers (io = 0.82 at 5:3 → 6:2, never 7:1; io = 0.63 at 2:2 of 4 holds; a 0.20-wide window stays undecided).</p>

<h2>5. The gate row this branch broke, and the fix</h2>
<p><code>tests/gate.sh</code> runs <code>tests/flipctl.py</code> — "flip controller: ramp gate, hold, surge + mix re-maneuvers". On the first policy binary it FAILED with a <b>rail anchor</b>: the boot maneuver moved 6:2 → 1:7 and stayed. The move itself is the model doing its job (the driver is three connections of <code>BITCOUNT</code> over 4 MB bitmaps, so the busiest executor thread has zero headroom and io is 99% idle: the saturation gate opens and work conservation really does rate 1:7 at 3.5×). What was wrong is the <i>confirmation</i>. <code>DEBUG FLIPCTL</code> at the failure:</p>
<pre>origin_rate=4898.601   anchor_rate=6000.916   model_last_decision=moved-delivered
boot_rate_slope=0.022869547   boot_rate_slope_threshold=0.029664539</pre>
<p>The load was still trending — just under the controller's own deferral threshold — so the "+22.5% delivered" that confirmed the flip was the driver's ramp, not the flip's doing. Base does not rail here because it random-walks with halving steps and settles on the best of several readings; the verify-or-revert seek takes one probe and compares it with one pre-flip reading, which is only as good as the baseline's own stillness.</p>
<p><b>Fix:</b> the Measuring phase never moves the split, so the readings the model already takes while deciding <i>are</i> readings of the origin. Bracket them and floor every band of the maneuver at twice that spread — the same 2× observed-jitter convention as the signature band, the rate band and <code>band_</code>. On the gate's ramping driver the floor becomes 40%, the ramp's 22.5% confirms nothing, the seek reverts to 6:2 and anchors off-rail. On a still baseline the floor is ≈0 and nothing changes. In the measured historical build, the floor also applied under the now-removed <code>--flip-auto-band</code>: that knob says how small a gain is worth chasing, not how still the workload is holding.</p>
{ctl_table}

<h2>6. Non-vacuity: the guard must not be a silent <code>--flip-auto 0</code></h2>
<p>Every zero-flip row above is worthless if the controller can no longer move at all. This probe boots the server at the <b>wrong</b> split for the load — 3:1, which the explicit-flip trace measures at about a third of 2:2 — and runs the same multi-key workload. <code>--flip-auto 1</code> has to find the split and pay for the flip; <code>--flip-auto 0</code> must stay where it was booted.</p>
{fire_table}

<h2>7. What the machinery costs when it is not moving</h2>
<p>Rate-limited so every arm offers the same load (<code>memtier --rate-limiting</code>): instructions and cycles per command are only comparable at a matched rate, because a spinning loop inflates both. <b>base fa=0 → fix fa=0</b> is the hot-path cost of the ex-loop accounting change (one local, one branch per pass); <b>fix fa=0 → fix fa=1</b> is the always-on cost of running the controller while it holds.</p>
{perf_table}

<h2>8. Acceptance on the quiet box</h2>
<pre>worktree  /home/user/Projects/wt-flipdamp   branch t-flipdamp   commit {COMMIT}   (PRE {BASE})
server    ./build/tomokv --port &lt;p&gt; --save '' --ratio 18:14 --shards 64 --atomic 1 --flip-auto 1 --enable-debug-command yes

A  DEFECT CELL      MSET8+MGET8 1:1, 512 conns, p32, --ratio 18:14, ABBA x6 against --flip-auto 0
                    expect flip_completed 0 and flip_clients_transferred 0 after the boot anchor,
                    flipctl_model_holds &ge; 1, flipctl_round_trips 0, rate inside the --flip-auto 0 pair's spread
B  BREADTH          single-key SET:GET 1:1 and 9:1, and pure GET, same geometry: same counters, rate in the floor
C  NON-VACUITY      boot the SAME load at a deliberately wrong split (e.g. --ratio 28:4) with --flip-auto 1:
                    the controller MUST move (1 flip, anchor at its target, live == anchor) and MUST beat
                    the --flip-auto 0 control at 28:4. A zero-flip result here is a FAIL, not a pass
D  HISTORICAL GATE ROW         tests/gate.sh (or just: --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2
                    --lb-age-sample-rate 1024 + tests/flipctl.py --stable-seconds 30) -- this was the row the
                    branch broke and the baseline-band floor fixes; it must anchor OFF-RAIL
E  ALWAYS-ON COST   the A cell at a matched offered rate (memtier --rate-limiting) with perf stat on the
                    server: --flip-auto 1 against --flip-auto 0, same binary. Budget 3%</pre>
<p>If 18:14 is <i>not</i> the throughput optimum for that load, the correct outcome is one flip to the model's target that then stays (<code>model_last_decision=moved-delivered</code>, <code>round_trips=0</code>) — a move that pays for itself is the feature working, not thrash.</p>

<h2>9. Caveats and what is still open</h2>
<ul>
<li>Lane rig: 2 physical cores + SMT siblings for the server, 4 threads at 2:2; the owner's cell is 32 real cores at 18:14. Direction and counters transfer, magnitudes do not.</li>
<li>One POST cell out of twelve moved: single-key 1:1 round 3 flipped out, did not deliver, reverted, and doubled its own bar (<code>round_trips=1</code>, <code>model_margin=2</code>) — the outcome loop working, at a cost of 5.7% in that one 40 s cell. PRE did the same thing four times per cell in every cell of that regime.</li>
<li>The multi-key hold on this rig is the <i>saturation gate's</i> (memtier at 27% of its cores, io 86% / ex 93%, neither side waiting on the other's capacity), not the model's. On the owner's saturated cell the hold must come from the model — <code>model_last_decision=hold-optimum</code> — or be a delivered move. Acceptance cell A is what distinguishes them.</li>
<li>The box carried other lanes all night (load 0.2 to 64, marker held for 100 of the 230 minutes). Every cell re-checked the gate and paused rather than measuring through someone else's run; the OFF-pair spread printed under the table is the resulting noise floor per regime.</li>
<li>Not chased, by instruction: <code>expwide</code>, <code>climon2</code> and <code>evict-lfu</code> under fused+armed (another lane owns them). Not attempted, by instruction: the client-weight actuator redesign.</li>
</ul>
<div class="foot">Binaries: {html.escape(arms_line)}. Files: scratch/NOTES.md (lane memory), scratch/ab.sh, abw.sh, ver.sh (harness), $SP/fd-matrix3.csv, fd-tl-*.txt (timelines), fd-mt-*.txt (memtier per-second), fd-dbg-*.txt (DEBUG FLIPCTL per cell). Memory corrected: tomokv-flip-accuracy-design (the role-flip round trip, not the client-weight actuator).</div>
</main>
"""
open(OUT, "w").write(doc)
print("wrote", OUT, len(doc), "bytes;", len(m2), "matrix2 rows;", len(hold_lines), "hold lines;", len(bat_lines), "battery lines;", len(differ), "differ lines")
