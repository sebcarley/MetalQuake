#!/usr/bin/env python3
"""test/perf/perf-report.py -- render the sweep's results into the findings report.

Usage: python3 test/perf/perf-report.py [outdir]        (default test/perf/out)

Reads results.tsv (perf-sweep.sh) and, if present, audit.txt (perf-audit.sh).
Writes report.html (the human-readable deliverable) and findings.json (the
machine half, consumed by the retier step).

Statistics doctrine, straight from the tree's bench history:
  - the warmup round is DISCARDED;
  - an arm's figure is the MEDIAN of its rounds (medians survive the odd-boot
    class better than means);
  - every delta is against the SAME BED's baseline median from the SAME sweep;
  - the null arms' spread is the noise floor, printed beside every table, and
    any delta inside it is labelled NOISE rather than reported as a finding.
"""
import json, os, statistics, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "test/perf/out"
RES = os.path.join(OUT, "results.tsv")
AUD = os.path.join(OUT, "audit.txt")

rows = []
flagged = 0
with open(RES) as f:
    hdr = f.readline().rstrip("\n").split("\t")
    for line in f:
        p = line.rstrip("\n").split("\t")
        if len(p) < len(hdr):
            continue
        r = dict(zip(hdr, p))
        if r["round"] == "warmup":
            continue
        if r["arm"].startswith("!"):
            # a fullscreen run that did not land on the requested geometry
            # (perf-sweep.sh's '!' flag, the 1920x1017 class) -- dropped, and
            # counted, never silently averaged in
            flagged += 1
            continue
        try:
            r["fps"] = float(r["fps"]); r["min1s"] = float(r["min1s"])
        except ValueError:
            continue
        for k in ("trace_ms", "fog_ms", "shaft_ms"):
            try: r[k] = float(r[k])
            except (ValueError, KeyError): r[k] = None
        rows.append(r)

if flagged:
    print("perf-report: %d row(s) excluded -- flagged '!' by perf-sweep.sh (ran off the requested geometry)" % flagged)

# Group by (bed, phase): an arm's delta is only meaningful against the SAME
# phase's baseline rounds -- pooling phases lets plateau drift between sweeps
# masquerade as a dirty floor (measured 2026-08-12: demo14 A at ~89 fps and B
# at ~92 pooled to a false ±3.99% floor).
beds = sorted({(r["bed"], r["phase"]) for r in rows})

def med(vals):
    return statistics.median(vals) if vals else None

def arm_stats(bedphase, arm):
    bed, phase = bedphase
    sel = [r for r in rows if r["bed"] == bed and r["phase"] == phase and r["arm"] == arm]
    if not sel: return None
    s = {"n": len(sel),
         "fps": med([r["fps"] for r in sel]),
         "min1s": med([r["min1s"] for r in sel])}
    for k in ("trace_ms", "fog_ms", "shaft_ms"):
        v = [r[k] for r in sel if r[k] is not None]
        s[k] = med(v) if v else None
    return s

findings = {"beds": {}}
for bed in beds:
    base = arm_stats(bed, "baseline")
    if not base: continue
    arms = sorted({r["arm"] for r in rows if r["bed"] == bed[0] and r["phase"] == bed[1]} - {"baseline"})
    nulls = [a for a in arms if a.startswith("null")]
    floor = 0.0
    for a in nulls:
        s = arm_stats(bed, a)
        if s: floor = max(floor, abs(s["fps"] - base["fps"]) / base["fps"] * 100.0)
    entries = []
    for a in arms:
        s = arm_stats(bed, a)
        if not s: continue
        d = (s["fps"] - base["fps"]) / base["fps"] * 100.0
        dmin = (s["min1s"] - base["min1s"]) / base["min1s"] * 100.0 if base["min1s"] else 0.0
        entries.append({"arm": a, "fps": s["fps"], "min1s": s["min1s"],
                        "dpct": d, "dminpct": dmin, "n": s["n"],
                        "trace_ms": s["trace_ms"], "fog_ms": s["fog_ms"], "shaft_ms": s["shaft_ms"],
                        "noise": abs(d) <= max(floor, 1.0)})
    entries.sort(key=lambda e: e["dpct"], reverse=True)
    findings["beds"]["%s phase-%s" % bed] = {"baseline": base, "floor_pct": floor, "arms": entries}

with open(os.path.join(OUT, "findings.json"), "w") as f:
    json.dump(findings, f, indent=1)

# ---- the HTML ---------------------------------------------------------------
def esc(s): return str(s).replace("&", "&amp;").replace("<", "&lt;")

H = []
H.append("""<meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>QuakeM5 — performance sweep findings</title><style>
:root{color-scheme:light dark;--bg:#14100e;--panel:#1d1815;--rule:#332b25;--tx:#d3c9bc;--dim:#8a7d6e;--fire:#e0762f;--murk:#7ea6ba;--good:#7fbf6a;--bad:#c8483a;
font-family:Charter,Georgia,serif}
@media(prefers-color-scheme:light){:root{--bg:#efe7db;--panel:#e6dccd;--rule:#cfc0aa;--tx:#2b241d;--dim:#6a5c4c;--fire:#b4551a;--murk:#3d6b80;--good:#3c7a2c;--bad:#9c2418}}
body{margin:0;background:var(--bg);color:var(--tx);font-size:16px;line-height:1.55}
.wrap{max-width:60rem;margin:0 auto;padding:2.5rem 1.25rem 5rem}
h1{font-size:1.9rem;margin:0 0 .3rem}h2{font-size:1.15rem;margin:2.2rem 0 .4rem;color:var(--fire)}
.sub{color:var(--dim);margin:0 0 1.6rem}
table{border-collapse:collapse;width:100%;font-size:.85rem;font-variant-numeric:tabular-nums}
th,td{text-align:right;padding:.3rem .55rem;border-bottom:1px solid var(--rule);white-space:nowrap}
th:first-child,td:first-child{text-align:left}
thead th{font-family:ui-monospace,Menlo,monospace;font-size:.68rem;letter-spacing:.1em;text-transform:uppercase;color:var(--dim);font-weight:400}
.bar{display:inline-block;height:.65em;background:var(--bad);vertical-align:baseline}
.bar.up{background:var(--good)}
.noise{color:var(--dim)}.noise .bar{opacity:.35}
code{font-family:ui-monospace,Menlo,monospace;font-size:.82em;background:var(--panel);border:1px solid var(--rule);border-radius:3px;padding:.05em .3em}
pre{background:var(--panel);border:1px solid var(--rule);border-radius:4px;padding:.8rem;overflow-x:auto;font-size:.78rem;line-height:1.4}
.scroll{overflow-x:auto}
</style><div class=wrap>
<h1>QuakeM5 — performance sweep</h1>
<p class=sub>Every arm is Seb's live config plus one change, interleaved round-robin against
baseline on the same bed. Figures are medians; the warmup is discarded; anything inside the
null-arm floor is greyed as noise. Negative Δ = the arm costs frames relative to the look we
play; positive Δ = frames returned for switching something off or down.</p>""")

for bed in beds:
    b = findings["beds"].get("%s phase-%s" % bed)
    if not b: continue
    base = b["baseline"]
    H.append(f"<h2>bed {esc(bed[0])} — phase {esc(bed[1])}</h2>")
    H.append(f"<p class=sub>baseline {base['fps']:.1f} fps (1s-min {base['min1s']:.0f}) over {base['n']} rounds; "
             f"noise floor ±{b['floor_pct']:.2f}%</p><div class=scroll><table>")
    has_stage = any(e["trace_ms"] is not None for e in b["arms"])
    H.append("<thead><tr><th>arm</th><th>fps</th><th>Δ%</th><th></th><th>1s-min</th><th>Δmin%</th>"
             + ("<th>trace ms</th><th>fog ms</th><th>shaft ms</th>" if has_stage else "")
             + "<th>n</th></tr></thead><tbody>")
    for e in b["arms"]:
        cls = " class=noise" if e["noise"] else ""
        w = min(abs(e["dpct"]) * 3.0, 240.0)
        up = " up" if e["dpct"] > 0 else ""
        stage = ""
        if has_stage:
            stage = "".join(f"<td>{('%.2f' % e[k]) if e[k] is not None else '—'}</td>"
                            for k in ("trace_ms", "fog_ms", "shaft_ms"))
        H.append(f"<tr{cls}><td><code>{esc(e['arm'])}</code></td><td>{e['fps']:.1f}</td>"
                 f"<td>{e['dpct']:+.1f}</td><td><span class='bar{up}' style='width:{w:.0f}px'></span></td>"
                 f"<td>{e['min1s']:.0f}</td><td>{e['dminpct']:+.1f}</td>{stage}<td>{e['n']}</td></tr>")
    H.append("</tbody></table></div>")

if os.path.exists(AUD):
    H.append("<h2>renderer audit — what actually ran</h2><pre>")
    H.append(esc(open(AUD).read()))
    H.append("</pre>")

H.append("""<h2>reading the numbers</h2>
<p>An arm that RETURNS a large Δ when switched off is a candidate for optimisation, not for
switching off — the question the follow-up sessions answer is how much of that cost is
intrinsic and how much is implementation. The dither-class candidates (costs that exist to
hide artefacts: march steps vs banding, history vs shimmer grain, buffer resolution vs the
bilinear upsample the murk still uses) are the first place to look for wins that improve the
picture and the frame rate together.</p></div>""")

with open(os.path.join(OUT, "report.html"), "w") as f:
    f.write("\n".join(H))
print(f"wrote {OUT}/report.html and {OUT}/findings.json "
      f"({sum(len(b['arms']) for b in findings['beds'].values())} arms over {len(beds)} beds)")
