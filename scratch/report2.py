#!/usr/bin/env python3
"""report2.py OUT.html -- the redesign lane's report (HTML artifact) from the measurement files.
Reads $SP/fd-matrix4.csv (+ fd-tl-*/fd-dbg-* per cell), fd-costgate-*.txt, fd-hold4-*.txt,
fd-fire4-*.txt, fd-perf4-*.txt, fd-ctl4-*.txt and the chain log fd-red.log."""
import csv, html, os, re, statistics as st, subprocess, sys
from collections import defaultdict

SP = "/tmp/claude-1000/-home-user-Projects/ee6eb242-5302-49cf-b767-1a2d8d8f0f61/scratchpad"
WT = "/home/user/Projects/wt-flipdamp"
OUT = sys.argv[1]
COMMIT = subprocess.run(["git", "-C", WT, "rev-parse", "--short=9", "HEAD"], capture_output=True, text=True).stdout.strip()
BASE = "e902c67d5"; GUARD = "d84031d2f"

def f(x):
    try: return float(x)
    except Exception: return 0.0
def i(x):
    try: return int(float(x))
    except Exception: return 0
def load(path):
    if not os.path.exists(path): return []
    return [r for r in csv.DictReader(open(path)) if r.get("rate") not in (None, "", "MISSING")]

def timeline(arm, wl, rnd):
    """(flips before first anchor, flips after first anchor, first-anchor second) from the matrix timeline."""
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

ARMS = ["base1", "guard1", "red1", "pol0a", "pol0b"]
def summarize(rows):
    g = defaultdict(list); wls = []
    for r in rows:
        g[(r["wl"], r["arm"])].append(r)
        if r["wl"] not in wls: wls.append(r["wl"])
    out = []
    for wl in wls:
        off = [f(r["rate"]) for a in ("pol0a", "pol0b") for r in g.get((wl, a), [])]
        off_mean = st.mean(off) if off else 0
        null = (max(off) - min(off)) / off_mean * 100 if off_mean and len(off) > 1 else 0
        for arm in ARMS:
            rs = g.get((wl, arm))
            if not rs: continue
            rates = [f(r["rate"]) for r in rs]
            pre = post = 0
            for r in rs:
                a, b, _ = timeline(arm, wl, r["round"])
                if a is not None: pre += a; post += b
            decisions = sorted(set(r.get("decision", "") for r in rs if r.get("decision")))
            out.append(dict(wl=wl, arm=arm, n=len(rs), rate=st.mean(rates), lo=min(rates), hi=max(rates),
                            rel=(st.mean(rates) / off_mean - 1) * 100 if off_mean else 0, null=null,
                            comp=sum(i(r["comp"]) for r in rs), xfer=sum(i(r["xfer"]) for r in rs),
                            trig=sum(i(r["trig"]) for r in rs), hold=sum(i(r["hold"]) for r in rs),
                            rt=sum(i(r.get("rt", 0)) for r in rs), pre=pre, post=post,
                            anchor=",".join(sorted(set(r["anchor"] for r in rs))),
                            live=",".join(sorted(set(r["live"] for r in rs))),
                            p99=st.median([f(r["p99"]) for r in rs]),
                            decision=",".join(decisions), misses=sum(i(r.get("misses", 0)) for r in rs),
                            costholds=sum(i(r.get("costholds", 0)) for r in rs)))
    return out

WLNAME = {"mk": "multi-key MSET8+MGET8 1:1, p32 (the defect regime)", "sk1:1": "single-key SET:GET 1:1, p32",
          "sk9:1": "single-key SET:GET 9:1, p32", "get": "pure GET, p32 (null)"}
ARMNAME = {"base1": "BASE-auto t9final, --flip-auto 1", "guard1": f"GUARD {GUARD}, --flip-auto 1",
           "red1": f"REDESIGN {COMMIT}, --flip-auto 1",
           "pol0a": f"OFF  {COMMIT}, --flip-auto 0 (a)", "pol0b": f"OFF  {COMMIT}, --flip-auto 0 (b)"}
ARMCLASS = {"base1": "pre", "guard1": "guard", "red1": "post", "pol0a": "off", "pol0b": "off"}

def matrix_table(summ, caption):
    h = [f'<figure class="tbl"><figcaption>{caption}</figcaption><div class="scroll"><table>',
         "<thead><tr><th>regime</th><th>arm</th><th class=n>cells</th><th class=n>ops/s mean</th><th class=n>min – max</th>"
         "<th class=n>vs OFF</th><th class=n>flips</th><th class=n>after anchor</th><th class=n>clients moved</th>"
         "<th class=n>triggers</th><th class=n>holds</th><th class=n>misses</th><th class=n>anchor</th><th class=n>live at end</th><th class=n>p99 ms</th><th>last decision</th></tr></thead><tbody>"]
    last_wl = None
    for s in summ:
        n_wl = sum(1 for x in summ if x["wl"] == s["wl"])
        wl_cell = f'<td class="wl" rowspan="{n_wl}">{html.escape(WLNAME.get(s["wl"], s["wl"]))}</td>' if s["wl"] != last_wl else ""
        last_wl = s["wl"]
        rel = "" if s["arm"] in ("pol0a", "pol0b") else f'{s["rel"]:+.1f}%'
        flips_cls = ' class="n bad"' if s["comp"] else ' class="n good"'
        post_cls = ' class="n bad"' if s["post"] else ' class="n"'
        miss = s["misses"] if s["arm"] in ("red1",) else ""
        h.append(f'<tr class="{ARMCLASS[s["arm"]]}">{wl_cell}<td class="arm"><code>{html.escape(ARMNAME[s["arm"]])}</code></td>'
                 f'<td class=n>{s["n"]}</td><td class=n><b>{s["rate"]/1000:,.0f}k</b></td><td class=n>{s["lo"]/1000:,.0f}–{s["hi"]/1000:,.0f}k</td>'
                 f'<td class=n>{rel}</td><td{flips_cls}>{s["comp"]}</td><td{post_cls}>{s["post"]}</td><td class=n>{s["xfer"]}</td>'
                 f'<td class=n>{s["trig"]}</td><td class=n>{s["hold"]}</td><td class=n>{miss}</td><td class=n>{s["anchor"]}</td><td class=n>{s["live"]}</td><td class=n>{s["p99"]:.0f}</td><td class="dec">{html.escape(s["decision"])}</td></tr>')
    h.append("</tbody></table></div></figure>")
    return "\n".join(h)

def simple_table(caption, headers, rows, classes=None):
    h = [f'<figure class="tbl"><figcaption>{caption}</figcaption><div class="scroll"><table><thead><tr>']
    h += [f'<th class=n>{c}</th>' if k else f'<th>{c}</th>' for k, c in enumerate(headers)]
    h.append("</tr></thead><tbody>")
    for n, row in enumerate(rows):
        cls = f' class="{classes[n]}"' if classes else ""
        h.append(f"<tr{cls}>" + "".join(f'<td class=n>{c}</td>' if k else f'<td class="arm">{c}</td>' for k, c in enumerate(row)) + "</tr>")
    h.append("</tbody></table></div></figure>")
    return "\n".join(h)

def kvline(path):
    if not os.path.exists(path): return None
    parts = open(path).read().split()
    if not parts: return None
    d = {"tag": parts[0]}
    for token in parts[1:]:
        if "=" in token:
            k, v = token.split("=", 1); d[k] = v
    return d

def read(path):
    return open(path, errors="replace").read() if os.path.exists(path) else ""

log = read(f"{SP}/fd-red2.log") + read(f"{SP}/fd-red.log")
def grab(pattern, width=220):
    return [re.sub(r"^\d\d:\d\d:\d\d ", "", m)[:width] for m in re.findall(r"^(?:\d\d:\d\d:\d\d )?" + pattern, log, re.M)]
def li(lines):
    return "".join(f"<li><code>{html.escape(l)}</code></li>" for l in lines) or "<li class=muted>(not on file)</li>"

# ---- data --------------------------------------------------------------------------------------
m4 = load(f"{SP}/fd-matrix5.csv") or load(f"{SP}/fd-matrix4.csv")
summ = summarize(m4)
def cell(wl, arm): return next((s for s in summ if s["wl"] == wl and s["arm"] == arm), None)
def kv(s, key, fmt="{}"): return fmt.format(s[key]) if s else "—"

fire_tags = ["red-22-off", "red-31-off", "red-31-on", "guard-31-on", "base-31-on",
             "red8-44-off", "red8-71-off", "red8-71-on", "guard8-71-on"]
fire = [d for d in (kvline(f"{SP}/fd-fire4-{t}.txt") for t in fire_tags) if d]
perf = [d for d in (kvline(f"{SP}/fd-r2-perf-{t}.txt") or kvline(f"{SP}/fd-perf4-{t}.txt") for t in
        ("base0-1", "base0-2", "red0-1", "red0-2", "red1-1", "red1-2", "guard1-1", "guard1-2")) if d]
def perf_mean(prefix, key):
    vals = [float(d[key]) for d in perf if d["tag"].startswith(prefix) and key in d]
    return st.mean(vals) if vals else 0

costgate = []
for tag, path in (("final binary", f"{SP}/fd-r3-costgate.txt"), ("run 1", f"{SP}/fd-r2-costgate-1.txt"), ("run 2", f"{SP}/fd-r2-costgate-2.txt")):
    t = read(path)
    if not t: continue
    lines = [l for l in t.splitlines() if re.match(r"^[1-4]\. |^ok:|AssertionError", l)]
    costgate.append((tag, lines))
holds = [(t, [l for l in (read(f"{SP}/fd-r2-hold-{t}.txt") or read(f"{SP}/fd-hold4-{t}.txt")).splitlines() if re.match(r"^ok:|^anchored|^negative|^positive|AssertionError", l)]) for t in ("red-a", "red-b")]
ctl = []
for gen, label in (("r3", "final (long-window floor)"), ("r2", "before the long-window floor")):
    for r in (1, 2):
        t = read(f"{SP}/fd-{gen}-ctl-{r}.txt")
        if not t: continue
        rc = re.search(r"^RC=(\d+)", t, re.M)
        said = re.search(r"^(ramp deferred[^\n]*|.*AssertionError[^\n]*)", t, re.M)
        ctl.append((f"{label} run {r}", "PASS" if rc and rc.group(1) == "0" else "FAIL", rc.group(1) if rc else "?", said.group(1).strip() if said else ""))
bat_lines = grab(r"(?:BAT |SPINPROBE |UNIT )[^\n]*$", 170)
mode_lines = grab(r"MODES: [^\n]*$")
differ = grab(r"DIFFER [^\n]*$")
arms_line = (grab(r"ARMS [^\n]*$", 400) or [""])[0]

# the redesign's decision trail per cell (DEBUG FLIPCTL at the end of each red1 cell)
def dbg(arm, wl, rnd):
    t = read(f"{SP}/fd-dbg-{arm}-{wl}-{rnd}.txt"); d = {}
    for line in t.splitlines():
        for tok in line.split():
            if "=" in tok:
                k, v = tok.split("=", 1); d.setdefault(k, v)
    return d
trail = []
for r in m4:
    if r["arm"] != "red1": continue
    d = dbg("red1", r["wl"], r["round"])
    trail.append((r["wl"], r["round"], d.get("model_io_frac", ""), d.get("model_headroom_io", ""), d.get("model_headroom_ex", ""),
                  d.get("model_target_io", ""), d.get("model_gain_mean", ""), d.get("model_required_gain", ""),
                  d.get("cost_pays", ""), d.get("cost_payback_s", ""), d.get("cost_horizon_s", ""), r.get("decision", ""), r["comp"]))

mk_red = cell("mk", "red1"); mk_guard = cell("mk", "guard1"); mk_base = cell("mk", "base1")
tot = lambda arm, key: sum(s[key] for s in summ if s["arm"] == arm)

css = """
:root{--bg:#F4F6F8;--paper:#FFFFFF;--ink:#171C24;--muted:#5C6673;--rule:#D6DCE3;--code:#EAEEF3;
--post:#2F4C9E;--post-soft:#E1E7F7;--pre:#9A4E22;--pre-soft:#F5E4D8;--guard:#7A6A2E;--good:#2F6F4F;--bad:#A8401F;--off:#4B5563}
@media (prefers-color-scheme: dark){:root:not([data-theme="light"]){--bg:#0F141B;--paper:#161C25;--ink:#E5EAF0;--muted:#98A3B1;--rule:#2A3441;--code:#1C2431;
--post:#8FA6EA;--post-soft:#1E2A4A;--pre:#E39A6C;--pre-soft:#3B2618;--guard:#C9B36A;--good:#6FC29A;--bad:#EF9270;--off:#AAB4C0}}
:root[data-theme="dark"]{--bg:#0F141B;--paper:#161C25;--ink:#E5EAF0;--muted:#98A3B1;--rule:#2A3441;--code:#1C2431;
--post:#8FA6EA;--post-soft:#1E2A4A;--pre:#E39A6C;--pre-soft:#3B2618;--guard:#C9B36A;--good:#6FC29A;--bad:#EF9270;--off:#AAB4C0}
body{background:var(--bg);color:var(--ink);font-family:"IBM Plex Sans",system-ui,sans-serif;font-size:16px;line-height:1.5;margin:0}
main{max-width:1160px;margin:0 auto;padding:32px 24px 64px}
h1,h2,h3{font-family:"IBM Plex Serif",Georgia,serif;text-wrap:balance;line-height:1.15;margin:0}
h1{font-size:2.1rem;font-weight:500}h2{font-size:1.45rem;font-weight:500;margin-top:44px}h3{font-size:1.1rem;font-weight:600;margin-top:22px}
p,li{max-width:74ch}p{margin:10px 0}
.eyebrow{font-size:.78rem;letter-spacing:.08em;text-transform:uppercase;color:var(--muted);margin-bottom:8px}
.verdict{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:14px;margin:22px 0 6px}
.verdict div{background:var(--paper);border:1px solid var(--rule);padding:14px 16px}
.verdict .k{font-size:.78rem;letter-spacing:.06em;text-transform:uppercase;color:var(--muted)}
.verdict .v{font-family:"IBM Plex Mono",ui-monospace,monospace;font-size:1.35rem;margin-top:4px;font-variant-numeric:tabular-nums}
.verdict .s{color:var(--muted);font-size:.9rem;margin-top:2px}
code,pre,td.n,th.n,.mono,td.dec{font-family:"IBM Plex Mono",ui-monospace,SFMono-Regular,monospace;font-variant-numeric:tabular-nums}
code{font-size:.85em;background:var(--code);padding:1px 5px;border-radius:2px}
pre{background:var(--code);padding:14px 16px;overflow-x:auto;font-size:.82rem;line-height:1.5;border:1px solid var(--rule)}
figure.tbl{margin:18px 0}figcaption{font-size:.9rem;color:var(--muted);margin-bottom:8px}
.scroll{overflow-x:auto}table{border-collapse:collapse;width:100%;font-size:.88rem;background:var(--paper)}
th,td{padding:7px 10px;border-bottom:1px solid var(--rule);text-align:left;vertical-align:top;white-space:nowrap}
th{font-size:.75rem;letter-spacing:.05em;text-transform:uppercase;color:var(--muted);font-weight:600}
td.n,th.n{text-align:right}td.wl{white-space:normal;min-width:160px;color:var(--muted);font-size:.85rem}td.dec{font-size:.8rem;color:var(--muted)}
tr.pre td.arm code{background:var(--pre-soft);color:var(--pre)}tr.post td.arm code{background:var(--post-soft);color:var(--post)}tr.guard td.arm code{color:var(--guard)}
td.good{color:var(--good);font-weight:600}td.bad{color:var(--bad);font-weight:600}
.muted{color:var(--muted)}
.status{background:var(--pre-soft);border:1px solid var(--rule);border-left:3px solid var(--pre);padding:12px 16px;margin:16px 0;font-size:.92rem}
.status b{color:var(--pre)}
.formula{background:var(--paper);border:1px solid var(--rule);border-left:3px solid var(--post);padding:14px 18px;margin:14px 0;font-family:"IBM Plex Mono",ui-monospace,monospace;font-size:.86rem;line-height:1.6;overflow-x:auto}
.formula b{color:var(--post);font-weight:600}
dl.units{display:grid;grid-template-columns:max-content 1fr;gap:4px 18px;margin:10px 0;font-size:.9rem}dl.units dt{font-family:"IBM Plex Mono",monospace;color:var(--post)}dl.units dd{margin:0;color:var(--muted)}
.two{display:grid;grid-template-columns:1fr 1fr;gap:28px;align-items:start}@media(max-width:820px){.two{grid-template-columns:1fr}}
ul{padding-left:20px}li{margin:4px 0}
.foot{margin-top:48px;border-top:1px solid var(--rule);padding-top:12px;color:var(--muted);font-size:.85rem}
"""

fire_table = simple_table(
    "non-vacuity — the same multi-key load, 60 s, server booted at the split named in the row (4 threads on 52,53,180,181; the 8-thread rows on 52-55 + siblings)",
    ["arm", "booted", "flip-auto", "ops/s", "p99 ms", "flips", "clients moved", "triggers", "holds", "anchor", "live at end", "decision", "kappa", "client cost"],
    [(f"<code>{html.escape(d['tag'])}</code>", d.get("boot", ""), d.get("fa", ""), f"<b>{f(d.get('rate', 0))/1000:,.0f}k</b>", f"{f(d.get('p99', 0)):.0f}",
      d.get("flips", ""), d.get("xfer", ""), d.get("trig", ""), d.get("holds", ""), d.get("anchor", ""), d.get("live", ""),
      html.escape(d.get("decision", "")), d.get("kappa", ""), d.get("clientcost", "")) for d in fire],
    ["post" if d.get("fa") == "1" and d["tag"].startswith("red") else "pre" if d.get("fa") == "1" else "off" for d in fire]) if fire else "<p class=muted>(probe rows not on file)</p>"

# ---- time-to-first-move (120 s cells, 1 Hz trace) ----------------------------------------------
TM_TAGS = ["off-22","base-22","guard-22","red-22","off-31","base-31","guard-31","red-31",
           "off8-71","base8-71","guard8-71","red8-71"]
def tm_row(prefix, tag):
    t = read(f"{SP}/fd-{prefix}-tm-{tag}.txt")
    if not t: return None
    head, _, _ = t.partition("|")
    d = {"tag": tag, "gen": prefix}
    for tok in head.split():
        if "=" in tok:
            k, v = tok.split("=", 1); d[k] = v
    boot = d.get("boot", "0:0").split(":")
    trace = f"{SP}/fd-{prefix}-tmtrace-{tag}.txt"
    if os.path.exists(trace) and len(boot) == 2:
        out = subprocess.run(["python3", f"{WT}/scratch/ttfm.py", trace, boot[0], boot[1]],
                             capture_output=True, text=True).stdout
        for tok in out.splitlines()[0].split() if out else []:
            if "=" in tok:
                k, v = tok.split("=", 1); d[k] = v
        d["timeline"] = out.splitlines()[1].strip() if out and len(out.splitlines()) > 1 else ""
    return d
tms = [d for d in (tm_row("r2", t) for t in TM_TAGS) if d]
tms3 = [d for d in (tm_row("r3", t) for t in ("red-22", "red-31", "red8-71")) if d]
def tm_cls(tag):
    return "post" if tag.startswith("red") else "guard" if tag.startswith("guard") else "pre" if tag.startswith("base") else "off"
def tm_table_of(rows, caption):
    if not rows: return "<p class=muted>(cells not on file)</p>"
    return simple_table(caption,
        ["arm", "boot", "fa", "steady ops/s (last 30 s)", "time-to-first-move", "moves after stab.", "flips", "clients", "final split", "decision", "misses", "p99 ms", "split timeline"],
        [(f"<code>{html.escape(d['tag'])}</code>", d.get("boot",""), d.get("fa",""),
          f"<b>{f(d.get('steady',0))/1000:,.0f}k</b>",
          (d.get("ttfm","") + " s") if d.get("ttfm","NEVER") not in ("NEVER","") else ("never (held)" if d.get("fa") == "1" else "—"),
          d.get("moves_after_stab",""), d.get("flips",""), d.get("xfer",""), d.get("final",""),
          html.escape(d.get("decision","") or "—"), d.get("misses","") or "—",
          f"{f(d.get('p99',0)):.0f}", html.escape(d.get("timeline","").replace("splits: ",""))) for d in rows],
        [tm_cls(d["tag"]) for d in rows])
tm_table = tm_table_of(tms, "time-to-first-move — 120 s multi-key cells, 1 Hz INFO trace (total_commands_processed, io/ex_threads); boots 2:2 (the optimum on this rig), 3:1 and 7:1 (wrong: more executors win). Binary: the asymmetric-gate revision before the long-window fix (62d85d3d8 / a19f4fba-era build).")
tm_table3 = tm_table_of(tms3, "same three redesign cells re-run on the FINAL binary (long-window noise floor, f2c8fc9a4): the 2:2 cell must show no spurious re-maneuver, the wrong-split cells one bounded move")

perf_table = simple_table(
    "instructions and cycles per command at a matched offered rate (memtier --rate-limiting), 35 s perf window on the server's pid after 12 s of warm-up, mk at 2:2",
    ["arm", "flip-auto", "ops/s", "commands in window", "instr/op", "cycles/op", "IPC", "server cpu-s", "controller"],
    [(f"<code>{html.escape(d['tag'])}</code>", d.get("fa", ""), f"{f(d.get('rate', 0))/1000:,.0f}k", f"{i(d.get('window_cmds', 0)):,}",
      d.get("instr/op", ""), d.get("cycles/op", ""), d.get("IPC", ""), d.get("srv_cpu_s", ""),
      html.escape(" ".join(v for k, v in d.items() if k.startswith("flipctl_")))) for d in perf],
    ["post" if d["tag"].startswith("red1") else "guard" if d["tag"].startswith("guard") else "pre" if d["tag"].startswith("base") else "off" for d in perf]) if perf else "<p class=muted>(perf rows not on file)</p>"

trail_table = simple_table(
    "the redesign's decision trail per matrix cell (DEBUG FLIPCTL at the end of the cell)",
    ["cell", "io share", "headroom io", "headroom ex", "target io", "gain mean", "bar", "pays", "payback s", "horizon s", "decision", "flips"],
    [(f"<code>{wl} r{r}</code>", fr, hio, hex_, tgt, gm, bar, pays, pb, hz, html.escape(dec), fl) for (wl, r, fr, hio, hex_, tgt, gm, bar, pays, pb, hz, dec, fl) in trail]) if trail else "<p class=muted>(trail not on file)</p>"

ctl_table = simple_table(
    "tests/flipctl.py (the historical gate invocation: --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2 --lb-age-sample-rate 1024, 8 server threads on cpus 52-55 + siblings)",
    ["run", "verdict", "rc", "what the row said"],
    [(f"<code>{html.escape(t)}</code>", v, rc, html.escape(said)) for t, v, rc, said in ctl],
    ["post" if v == "PASS" else "pre" for _, v, _, _ in ctl]) if ctl else "<p class=muted>(flipctl.py rows not on file)</p>"

def pct(a, b): return (b / a - 1) * 100 if a else 0
def spread(prefix):
    vals = [float(d["instr/op"]) for d in perf if d["tag"].startswith(prefix) and "instr/op" in d]
    return (max(vals) - min(vals)) / st.mean(vals) * 100 if len(vals) > 1 else 0
instr_base0 = perf_mean("base0", "instr/op"); instr_red0 = perf_mean("red0", "instr/op"); instr_red1 = perf_mean("red1", "instr/op"); instr_guard1 = perf_mean("guard1", "instr/op")
cyc_red0 = perf_mean("red0", "cycles/op"); cyc_red1 = perf_mean("red1", "cycles/op")

page = f"""<title>Flip Actuator Redesign</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Serif:wght@500;600&family=IBM+Plex+Sans:wght@400;600&family=IBM+Plex+Mono:wght@400;600&display=swap">
<style>{css}</style>
<main>
<div class="eyebrow">TomoKV · lane t-flipdamp · 2026-09-06 · commit {COMMIT} (guard {GUARD}, base {BASE})</div>
<h1>Flip actuator redesign: a move must pay for itself, in commands</h1>
<p>The owner's ask: "be innovative about flip arithmetic and signals to finally get that to being accurate and consistent." This page is the PRE/POST evidence for the redesign built on the verified guard: the demand signal measured from what the loops book faithfully, a cost gate in commands over the stationarity the workload has demonstrated, a verification window sized from the origin's own noise, and an outcome loop that learns the model's sign and magnitude. Table first; the arithmetic, the directed tests and the acceptance recipe follow.</p>

<div class="status"><b>Revision history on this page.</b> First publish (12:24): design, arithmetic, unit rows; the box was held by the owner&rsquo;s full gate. The detached chain then ran everything (12:43&ndash;14:08) on the asymmetric-gate binary. Two of its results exposed a further defect &mdash; the gate row anchored on a rail and the 120&nbsp;s matched-split cell re-maneuvered four times on a stationary load &mdash; with one root (bands learned from selected quiet pairs), fixed by the long-window noise floor (section 3, item 5) and re-verified below on the final binary.</div>

<div class="verdict">
<div><div class="k">multi-key, redesign vs controller OFF</div><div class="v">{kv(mk_red, 'rel', '{:+.1f}%')}</div><div class="s">guard {kv(mk_guard, 'rel', '{:+.1f}%')} · base {kv(mk_base, 'rel', '{:+.1f}%')} · OFF pair spread {kv(mk_red, 'null', '{:.1f}%')}</div></div>
<div><div class="k">flips after the first anchor (thrash), all regimes</div><div class="v">{tot('red1', 'post')}</div><div class="s">guard {tot('guard1', 'post')} · base {tot('base1', 'post')}</div></div>
<div><div class="k">wrong moves (misses) in 12 controller cells</div><div class="v">{tot('red1', 'misses')}</div><div class="s">guard: {tot('guard1', 'rt')} round trip(s) · base: {tot('base1', 'comp')} flips</div></div>
<div><div class="k">always-on cost at a matched rate</div><div class="v">{pct(instr_red0, instr_red1):+.2f}%</div><div class="s">instr/op, --flip-auto 1 vs 0, same binary · budget 3%</div></div>
</div>

<h2>1. PRE / POST matrix</h2>
<p>Five arms interleaved ABBA, 40 s cells, three rounds, one server boot per cell (every controller cell includes its boot maneuver). <b>OFF (a)/(b)</b> are the redesign binary with the controller off: their spread is the rig's noise floor per regime. Rates are memtier totals; flips, clients moved and decisions are the server's own counters; <i>after anchor</i> counts flips completed after the controller first reported <code>anchored</code> — the owner's definition of thrash. <i>misses</i> is the redesign's own outcome record: moves whose verification failed and were reverted.</p>
{matrix_table(summ, "4 regimes x 3 rounds x 5 arms, 40 s cells — 4 server threads on cpus 52,53,180,181 (2:2), memtier 8x32 conns p32 on 54-57,182-185, 200k keys, 32 B values, --shards 64 --atomic 1")}
<p class="muted">Noise floor (OFF pair spread): {"; ".join(f"{s['wl']} {s['null']:.1f}%" for s in summ if s['arm']=='pol0a')}. Rate differences inside the floor are not verdicts; the counters and the decisions are.</p>
{trail_table}

<h2>2. The signal defect the guard's own snapshots exposed</h2>
<p>The placement model read the roles' demand from <code>busy_ns</code>. The io loop closes its busy span <i>before</i> <code>ring_.submit_and_reap()</code>, so the <code>io_uring_enter</code> syscall — the kernel moving the bytes, which is io work — was booked as neither busy nor idle. From the guard night's <code>DEBUG LBSIGNALS</code> snapshots around a 40 s single-key cell:</p>
{simple_table("per-thread time accounting over one 40 s cell (sk1:1, 2:2 of 4), from the guard's fd-lb0/fd-lb1 snapshots", ["thread", "booked busy", "booked idle", "busy + idle", "cpu_ns (on-CPU)", "unbooked"],
  [("<code>io 0</code>", "16.36 s", "0.17 s", "16.53 s", "39.92 s", "<b>23.4 s</b>"), ("<code>io 3</code>", "22.24 s", "2.40 s", "24.64 s", "40.02 s", "<b>15.4 s</b>"),
   ("<code>ex 1</code>", "25.62 s", "2.56 s", "28.19 s", "39.99 s", "11.8 s"), ("<code>ex 2</code>", "33.25 s", "1.76 s", "35.01 s", "39.98 s", "5.0 s")])}
<p>The busy share read <code>io = 0.32</code> on a workload whose two roles were both saturated (headroom 0.5% / 3%), so the work share is ≈0.51. Work conservation then rated 1:3 above 2:2 (R(1) = min(1/0.32, 3/0.68) = 3.13 against R(2) = 2.94), the guard moved 2:2 → 1:3, measured 2.45M against 5.12M (−52%) and came back: its one round trip. Multi-key read 0.41 for a 0.477 workload — same direction, smaller, because MGET8 issues fewer syscalls per busy second.</p>
<p><b>Fix (signal):</b> work = wall − idle per thread. <code>idle_ns</code> is the one quantity both loops book faithfully (io: the blocked wait after an empty sweep; ex: empty passes and the blocked wait, since the guard's ex-loop change), and the wall is the controller's own tick clock, identical for every thread of the window — nothing new is read on the hot path. The same window now reads 0.506 and holds 2:2; multi-key reads 0.472 and holds. Headroom is idle/wall (it was idle/(busy+idle), 2.4× inflated for io thread 0).</p>

<h2>3. The arithmetic (src/core/flip_policy.h, one file, pure, unit-tested)</h2>
<div class="formula">
MOVE  iff  <b>κ g_low · R0 · (T_stat − T_black)</b>  &gt;  <b>margin · [ C_xfer (1 + P_miss) + P_miss · κ g_mean · R0 · T_black ]</b><br>
      and  κ g_low &gt; band · margin  (the noise bar: a gain the seek could not verify cannot pay for a flip)<br>
n_t   = ceil( 1 / ((κ g_mean / 4σ)² − 1/n_o) )      planned target readings; ≤ 0 ⇒ keep sampling the origin<br>
θ_k   = max( 2σ √(1/n_o + 1/k), bracket(origin), band )   accept when the target's mean clears θ_k, reject early when below −θ_k<br>
κ     = (Σ delivered + ḡ) / (Σ predicted + ḡ)   ḡ = mean predicted gain over moves; ≤ 1;  miss ⇒ margin × 2
</div>
<dl class="units">
<dt>R0</dt><dd>the origin's stabilized rate, commands/s — the mean of the Measuring readings</dd>
<dt>g_low, g_mean</dt><dd>the model's projected gain of the argmax split (R(s) = min(s/f, (N−s)/(1−f))) at the pessimistic end and the mean of the demand window; the step is the whole distance</dd>
<dt>T_stat</dt><dd>seconds the workload has held still: since the boot's first non-idle tick, or since the trigger that detected a change; a forced trigger keeps the clock</dd>
<dt>T_black</dt><dd>seconds the controller is blind after the flip: measured flip ticks + 3 settle ticks (window reset + two sub-windows: the controller's own reading mechanics) + (n_t − 1) readings</dd>
<dt>C_xfer</dt><dd>commands: client cost × predicted transfers. Client cost = Σ lost / Σ moved over every flip so far (lost = rate-before × elapsed − served, measured 8× per tick while the flip is in flight); transfers = |Δio|/max(io) share of connections × the planner's learned re-plan ratio. Zero before the first flip: that flip is the measurement</dd>
<dt>P_miss</dt><dd>(misses + 1)/(moves + 2), Laplace — one half before any move</dd>
<dt>σ, n_o</dt><dd>relative stdev and count of the origin's Measuring readings (Welford); the bracket 2(max−min)/mid guards trends</dd>
</dl>
<p><b>5. The long-window noise floor.</b> Every band the controller learned came from <i>stabilized pairs</i> — readings it selected because two adjacent sub-windows agreed — and that selection is biased quiet. Two origin readings taken inside one phase of the gate driver&rsquo;s six-second rate triangle read σ&nbsp;=&nbsp;0.05% on a load that swings ±20%, so the target&rsquo;s +24% (the swing, not the flip) cleared a 2% band and the boot maneuver anchored on a rail; a settle window that happened to be still learned an anchor band of 0.02% and then fired four maneuvers on a stationary 120&nbsp;s load, none of which could take a reading. <code>FlipEwVariance</code> is an exponentially weighted variance of the <i>second relative difference</i> of every per-tick rate sample — detrended, so a linear ramp is invisible, a level shift leaves two blips that decay, and a swing of a few ticks&rsquo; period keeps its full amplitude (the first form, deviation from a running mean, blew up on the boot ramp: σ&nbsp;16.9 and every band dead). Time constant = the boot deferral bound in ticks (no new constant); a tick at the idle floor breaks the chain, a second difference beyond 100% is a level change and resets it, and a flip the controller issued resets it. It floors the pair band, the verification band, the hypothesis&rsquo; σ and the anchor band. Accuracy from the window, and the window is the longest one the controller has.</p>
<p>What each piece replaces in <code>flipctl.cc</code>: <code>sample_role_demand</code> reads wall − idle instead of busy; <code>decide_placement</code> prices a target that clears the noise bar and keeps sampling while it does not yet pay (the horizon grows for free) — at the reading cap the hold is named <code>hold-cost</code>; the flip in flight is measured in <code>WaitingFlip</code>; the seek takes up to n_t readings and judges sequentially; <code>anchor()</code> finalizes a miss, or an <i>invalidated</i> maneuver when the origin no longer reads R0 after the return (the baseline moved; the guard doubled its bar on these too). Not a knob was added; 0/−1 semantics unchanged; no machine constants — every term is measured or derived from the tick.</p>

<h2>4. Directed tests</h2>
<h3>Cost gate and outcome loop — tests/flip_cost_gate.py</h3>
<p>Booted at 3:1 under a BITCOUNT load that saturates the one executor. (1) An absurd per-client cost is typed before the load: the model proposes its target and the gate refuses it — <code>hold-cost</code>, zero flips, split unchanged. (2) The measured cost is restored and the same workload re-opened: exactly one flip, verified against the origin's noise. (3) <code>DEBUG FLIPCTL SEEK &lt;origin&gt; FORCE</code> induces a miss: out, judged, back, margin doubled, one miss booked. (4) The same proposal judged is refused without a flip.</p>
<ul>{"".join(f"<li><b>{r}</b><ul>{li(lines)}</ul></li>" for r, lines in costgate) or "<li class=muted>(not on file)</li>"}</ul>
<h3>The defect regime held — tests/flip_multikey_hold.py</h3>
<ul>{"".join(f"<li><b>{t}</b><ul>{li(lines)}</ul></li>" for t, lines in holds)}</ul>

<h2>5. Time-to-first-move (the owner-box metric)</h2>
<p>The owner-box acceptance of the guard showed its real cost is not thrash but latency: on 64 shards 18:14 is far from optimal (more executors win), and the guard reached the better split late or, once, held on a terrible one. So the cost gate was made asymmetric (section 3): a move off a badly-underperforming split pays at about one blackout, a marginal move needs a long window. These 120 s cells report time-to-first-move, moves-after-stabilization and steady-state as three separate numbers, against BASE-auto (t9final) — the bar to beat — and the guard. On this 4-thread rig 2:2 is the optimum (loadgen-bound), so the 2:2 cells are the zero-loss control and the 3:1 / 7:1 boots are the wrong-split test the owner box runs at 28:4.</p>
{tm_table}
{tm_table3}
<p>Reading the table: BASE-auto moves first (12–14 s) but does not stay — it thrashes back to the wrong split on both 4-thread boots (6 flips, 799 clients, steady 403k at 3:1 against 522k) and lands on a poor split at 8 threads (6:2, 579k against 697k). Guard and redesign move once, 18–23 s after load, and hold; the redesign lands on the model&rsquo;s argmax (2:2, 4:4) with <code>moved-delivered</code> and zero misses. The time-to-first-move is the boot stability gate (five flat ticks after the load ramp) plus two stabilized readings plus the flip — the asymmetric gate itself paid on the first decision (<code>cost_payback_s</code> 4.7–6.0 s against a 12–23 s horizon).</p>
<h2>6. Non-vacuity: boot at the wrong split, move once, land</h2>
<p>Every zero-flip row is worthless if the controller can no longer move. Booted at 3:1 (4 threads) and 7:1 (8 threads) under the multi-key load, <code>--flip-auto 1</code> must find the split, pay for exactly one flip and stay; <code>--flip-auto 0</code> stays where it was booted.</p>
{fire_table}

<h2>7. What the machinery costs when it is not moving</h2>
<p>Rate-limited so every arm offers the same load; instructions and cycles per command are only comparable at a matched rate. <b>base fa=0 → redesign fa=0</b> is the hot path with the controller off; <b>redesign fa=0 → fa=1</b> is the always-on cost of the controller holding.</p>
{perf_table}
<p>Hot path, controller off: <b>{pct(instr_base0, instr_red0):+.2f}%</b> instr/op (base {instr_base0:.0f} → redesign {instr_red0:.0f}). Controller running and holding: <b>{pct(instr_red0, instr_red1):+.2f}%</b> instr/op, <b>{pct(cyc_red0, cyc_red1):+.2f}%</b> cycles/op against the same binary with it off. Same-arm spread this afternoon: {spread('base0'):.1f}% / {spread('red0'):.1f}% / {spread('red1'):.1f}% (other lanes were active), so both deltas are inside the rig&rsquo;s noise; last night&rsquo;s quieter measurement of the same always-on cost read +1.24%. Budget 3%.</p>

<h2>8. The gate row</h2>
{ctl_table}

<h2>9. Batteries, modes, differ</h2>
<ul>{li(bat_lines)}{li(mode_lines)}{li(differ)}</ul>

<h2>10. Acceptance on the owner's box</h2>
<pre>worktree  /home/user/Projects/wt-flipdamp   branch t-flipdamp   commit {COMMIT}   (guard {GUARD}, base {BASE})
server    ./build/tomokv --port &lt;p&gt; --save '' --ratio 18:14 --shards 64 --atomic 1 --flip-auto 1 --enable-debug-command yes

A  DEFECT CELL      MSET8+MGET8 1:1, 512 conns, p32, --ratio 18:14, ABBA x6 against --flip-auto 0
                    expect flip_completed 0 and flip_clients_transferred 0 after the boot anchor, flipctl_model_misses 0,
                    flipctl_model_last_decision hold-optimum (or moved-delivered with round_trips 0 if 18:14 is not the optimum),
                    rate inside the --flip-auto 0 pair's spread
B  BREADTH          single-key SET:GET 1:1 and 9:1, pure GET, same geometry: same counters, rate in the floor
C  NON-VACUITY      boot the SAME load at --ratio 28:4 (and 3:1 on 4 threads) with --flip-auto 1: exactly ONE flip
                    (flip_completed 1), anchor == live == the model's target, flipctl_model_last_decision moved-delivered,
                    rate above the --flip-auto 0 control at the wrong split. Zero flips here is a FAIL
D  DIRECTED         tests/flip_cost_gate.py against --ratio 3:1 --flip-auto 1 (cost gate refuses / pays / induced miss reverts and doubles the bar)
                    tests/flip_multikey_hold.py against --ratio 18:14 --flip-auto 1
E  HISTORICAL GATE ROW         --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2 --lb-age-sample-rate 1024 + tests/flipctl.py --stable-seconds 30
F  ALWAYS-ON COST   the A cell at a matched offered rate (memtier --rate-limiting) with perf stat on the server:
                    --flip-auto 1 against --flip-auto 0, same binary. Budget 3%
G  UNIT             make unit (build/flipctl-unit carries the signal, cost-gate, window and outcome rows on the measured numbers)</pre>

<h2>11. Caveats</h2>
<ul>
<li>Lane rig: 2 physical cores + SMT siblings for the server, 4 threads at 2:2; the owner's cell is 32 real cores at 18:14. Direction, counters and decisions transfer; magnitudes do not.</li>
<li>On this rig the multi-key hold is partly the saturation gate's (memtier at 27% of its cores). On the owner's saturated cell the hold must come from the model — <code>model_last_decision=hold-optimum</code> — or be a delivered move; acceptance cell A distinguishes them.</li>
<li>The transfer cost is unknown before the first flip and stays zero for it; the boot maneuver is charged through the blackout term only, and every flip after it is priced from measurement.</li>
<li>Gains too small to pay back within the reading cap (the boot deferral bound, in readings) hold as <code>hold-cost</code>; the anchored controller does not re-open a hypothesis on an unchanged workload — a move after the anchor would be thrash by definition.</li>
</ul>
<h2>12. Owner-box acceptance and the landing question (appended 2026-09-06 17:40)</h2>
<p>Coordinator&rsquo;s run of <code>dcc5a409a</code> (sha256 9b9b559a2e1a5eff) on the owner box: 64 shards, atomic 1, 512 conns, MSET8+MGET8 1:1 p32, 120 s cells, 1 Hz trace, settled = mean over t&nbsp;&ge;&nbsp;60 s.</p>
<figure class="tbl"><figcaption>owner box, both boots, four arms</figcaption><div class="scroll"><table><thead><tr><th>boot</th><th>arm</th><th class=n>first move</th><th class=n>flips / clients</th><th class=n>final</th><th class=n>settled</th><th>after 60 s</th></tr></thead><tbody>
<tr class="off"><td>18:14</td><td class="arm"><code>OFF</code></td><td class=n>—</td><td class=n>0</td><td class=n>18:14</td><td class=n>1.539M</td><td>—</td></tr>
<tr class="pre"><td></td><td class="arm"><code>BASE-auto t9final</code></td><td class=n>18 s → 31:1 (rail)</td><td class=n>3 / 973</td><td class=n>18:14</td><td class=n>1.531M</td><td>back at 24 s</td></tr>
<tr class="guard"><td></td><td class="arm"><code>GUARD d84031d2f</code></td><td class=n>16 s → 11:21</td><td class=n>1</td><td class=n>11:21</td><td class=n><b>2.106M</b></td><td>held</td></tr>
<tr class="post"><td></td><td class="arm"><code>REDESIGN dcc5a409a</code></td><td class=n>19 s → 13:19</td><td class=n>1</td><td class=n>13:19</td><td class=n>1.941M</td><td>held; 0 moves, holds 0, round_trips 0</td></tr>
<tr class="off"><td>28:4</td><td class="arm"><code>OFF</code></td><td class=n>—</td><td class=n>0</td><td class=n>28:4</td><td class=n>0.528M</td><td>—</td></tr>
<tr class="pre"><td></td><td class="arm"><code>BASE-auto t9final</code></td><td class=n>12 s → 31:1 (rail)</td><td class=n>2+</td><td class=n>28:4</td><td class=n>0.485M</td><td>back at 18 s; worse than OFF</td></tr>
<tr class="guard"><td></td><td class="arm"><code>GUARD d84031d2f</code></td><td class=n>15 s → 10:22</td><td class=n>1</td><td class=n>10:22</td><td class=n>1.973M</td><td>held</td></tr>
<tr class="post"><td></td><td class="arm"><code>REDESIGN dcc5a409a</code></td><td class=n>13 s → 12:20</td><td class=n>1</td><td class=n>12:20</td><td class=n><b>2.036M</b></td><td>held; 0 moves</td></tr>
</tbody></table></div></figure>
<p>Fixed-ratio OFF sweep (settled 20&ndash;50 s): 16:16 1.964M &middot; 14:18 2.013M &middot; 13:19 2.046M &middot; <b>11:21 2.098M</b> &middot; 10:22 1.960M &middot; 8:24 1.588M. The optimum is 11:21; the curve is within 2.5% from 13:19 down to it and falls off fast past it (10:22 &minus;6.6%, 8:24 &minus;24%).</p>
<p><b>The landing question.</b> 13:19 was the model&rsquo;s argmax at the origin&rsquo;s demand reading, not a cost-gated stop: after <code>moved-delivered</code> the maneuver ends (one jump is the whole distance) and the controller anchors; no detector fired on the stationary load (holds 0, round_trips 0), so a second step was never proposed and the gate refused nothing. The one-shot projection assumes per-command io and ex costs are the same at the target as at the origin; they are not &mdash; the <code>io_uring_enter</code> time that wall&nbsp;&minus;&nbsp;idle correctly counts amortizes over larger batches with fewer io threads &mdash; so an io-heavy origin reads the io share high (both boots ≈ 0.39 &rarr; 13 and 12). The guard&rsquo;s busy share omits exactly that part (≈ 0.33 &rarr; 11 and 10) and lands closer by cancellation, not accuracy (the same signal read 0.32 on a 0.51 single-key workload and round-tripped).</p>
<p><b>Refinement (commits <code>2a18b4a1d</code>, <code>ee40cf940</code>; verification queued behind the box).</b> After a delivered move the maneuver re-measures the landing as a fresh origin and takes one more outcome-judged step only if the local argmax differs, clears the noise bar and the cost gate (measured transfer cost), and &mdash; the rule the sweep dictates &mdash; clears <code>flip_refine_bar</code> = the model&rsquo;s error just demonstrated on this workload (predicted &minus; delivered of the step that landed). Work conservation is a V with its point at the optimum and the real curve is rounded there, so near the peak the model over-projects small steps (13:19 &rarr; 11:21: +10% by the model, +2.5% measured); a chain that respected only the noise bar would round-trip in the flat region or cross the peak. Stopping short costs 2.5%, crossing 6.6%. The local verdict is exposed as <code>flipctl_refine_decision</code> / <code>flipctl_refine_steps</code>. Expected on the owner box: both boots land as before and record <code>hold-within-error</code> at the landing with one flip; a boot whose first step delivers what it promised earns the second step.</p>
<div class="foot">Binaries: {html.escape(arms_line)}. Guard report: <a href="https://claude.ai/code/artifact/f01d7b82-5685-4f10-bf3f-e89010857b35">Flip Thrash Fix</a>. Files: scratch/NOTES.md (lane memory), scratch/red.sh, abr.sh (harness), $SP/fd-matrix4.csv, fd-tl-*.txt, fd-dbg-*.txt, fd-costgate-*.txt, fd-fire4-*.txt, fd-perf4-*.txt.</div>
</main>
"""
open(OUT, "w").write(page)
print(f"wrote {OUT} ({len(page)} bytes), matrix rows={len(m4)}, fire={len(fire)}, perf={len(perf)}")
