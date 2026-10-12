#!/usr/bin/env python3
"""
echoes-charts.py — live, read-only charts for Echoes of the Machine.

Serves one page (default port 8003) that visualises the fleet from data that
already exists; it never writes anything and does not touch echoes-server:

  * /var/log/echoes/echoes-server.log (+ rotated .1 .. .5)
      STARTUP / STALE / RECOVERED / PREV_BOOT / PREV_DIAG / Sonoff lines
  * http://127.0.0.1:8002/nodes      live status + group order (optional)
  * a sniffer JSON Lines file          ESP-NOW SOUND detections (optional)

Standard library only (no pip install).  Usage:

    python3 echoes-charts.py [--port 8003] [--log-dir /var/log/echoes]
                             [--server http://127.0.0.1:8002]
                             [--sniffer /home/nixon/logs/sound.jsonl]
"""

import argparse
import json
import os
import re
import threading
import time
import urllib.request
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse, parse_qs

KEEP_S = 8 * 24 * 3600          # history kept in memory (covers the 7-day view)
STALE_ONLINE_S = 180            # same threshold as echoes-server's STALE warning


def _now():
    # ECHOES_CHARTS_NOW lets you replay an old log as if it were live (testing).
    return float(os.environ.get("ECHOES_CHARTS_NOW") or time.time())

LINE_RE = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) \| \w+\s*\| (.*)$")
RE_START = re.compile(r"STARTUP\s+(\S+)\s+(?:id=(\S+)\s+)?type=(\S+)\s+fw=(\S+)"
                      r"(?:\s+ip=\S+)?(?:\s+RESET=(\S+))?")
RE_STALE = re.compile(r"STALE\s+(\S+)\s+id=(\S+)\s+type=(\S+)\s+last_seen=(\S+)")
RE_RECOV = re.compile(r"RECOVERED\s+(\S+)\s+id=(\S+)\s+type=(\S+)")
RE_PREVB = re.compile(r"PREV_BOOT\s+(\S+)\s+prev_reset=(\S+)")
RE_PREVD = re.compile(r"PREV_DIAG\s+(\S+)\s+cause=(\S+)")
RE_SONOFF = re.compile(r"Sonoff (\d+) .*→ (\S+)")
CRASH_REASONS = {"PANIC", "TASK_WDT", "INT_WDT", "BROWNOUT"}


def _epoch(s, fmt="%Y-%m-%d %H:%M:%S"):
    return time.mktime(datetime.strptime(s, fmt).timetuple())


def parse_line(line):
    m = LINE_RE.match(line)
    if not m:
        return None
    t, msg = _epoch(m.group(1)), m.group(2)
    if msg.startswith("STARTUP"):
        x = RE_START.search(msg)
        if x:
            return {"t": t, "k": "start", "mac": x[1], "id": x[2], "type": x[3],
                    "fw": x[4], "reset": x[5]}
    elif msg.startswith("STALE"):
        x = RE_STALE.search(msg)
        if x:
            try:
                last = time.mktime(datetime.fromisoformat(x[4]).timetuple())
            except ValueError:
                last = t
            return {"t": t, "k": "stale", "mac": x[1], "id": x[2], "type": x[3],
                    "last": last}
    elif msg.startswith("RECOVERED"):
        x = RE_RECOV.search(msg)
        if x:
            return {"t": t, "k": "recov", "mac": x[1], "id": x[2], "type": x[3]}
    elif msg.startswith("PREV_BOOT"):
        x = RE_PREVB.search(msg)
        if x:
            return {"t": t, "k": "prevb", "mac": x[1], "why": x[2]}
    elif msg.startswith("PREV_DIAG"):
        x = RE_PREVD.search(msg)
        if x:
            return {"t": t, "k": "prevd", "mac": x[1], "cause": x[2]}
    elif msg.startswith("Sonoff"):
        x = RE_SONOFF.search(msg)
        if x:
            return {"t": t, "k": "sonoff", "n": int(x[1]), "state": x[2]}
    return None


class LogCache:
    """Incrementally follows the server log (handles rotation)."""

    def __init__(self, log_dir):
        self.base = Path(log_dir) / "echoes-server.log"
        self.lock = threading.Lock()
        self.events = []
        self.inode = None
        self.offset = 0

    def _read(self, path, start=0):
        out = []
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            f.seek(start)
            for line in f:
                e = parse_line(line)
                if e:
                    out.append(e)
            return out, f.tell()

    def refresh(self):
        with self.lock:
            try:
                st = self.base.stat()
            except FileNotFoundError:
                return self.events
            if st.st_ino != self.inode or st.st_size < self.offset:
                # first load or rotation: rebuild from rotated files + current
                evs = []
                for i in range(5, 0, -1):
                    p = Path(f"{self.base}.{i}")
                    if p.exists() and (_now() - p.stat().st_mtime < KEEP_S or 'ECHOES_CHARTS_NOW' in os.environ):
                        evs += self._read(p)[0]
                cur, off = self._read(self.base)
                self.events, self.inode, self.offset = evs + cur, st.st_ino, off
            elif st.st_size > self.offset:
                new, self.offset = self._read(self.base, self.offset)
                self.events += new
            cutoff = _now() - KEEP_S
            if self.events and self.events[0]["t"] < cutoff:
                self.events = [e for e in self.events if e["t"] >= cutoff]
            return list(self.events)


class SnifferCache:
    """Follows a sniffer_monitor.py JSON Lines file; keeps SOUND detections."""

    def __init__(self, path):
        self.path = Path(path) if path else None
        self.lock = threading.Lock()
        self.sounds = []
        self.inode = None
        self.offset = 0

    def refresh(self):
        if not self.path:
            return None
        with self.lock:
            try:
                st = self.path.stat()
            except FileNotFoundError:
                return None
            if st.st_ino != self.inode or st.st_size < self.offset:
                self.sounds, self.offset, self.inode = [], 0, st.st_ino
            with open(self.path, "r", encoding="utf-8", errors="replace") as f:
                f.seek(self.offset)
                for line in f:
                    try:
                        r = json.loads(line)
                    except ValueError:
                        continue
                    if r.get("type") == "SOUND" and r.get("wall") and r.get("detection"):
                        try:
                            t = _epoch(r["wall"][:19], "%Y-%m-%dT%H:%M:%S")
                        except ValueError:
                            continue
                        self.sounds.append((t, r["detection"], r.get("mac", "")))
                self.offset = f.tell()
            cutoff = _now() - KEEP_S
            self.sounds = [s for s in self.sounds if s[0] >= cutoff]
            return list(self.sounds)


def _union(iv):
    out = []
    for a, b in sorted(iv):
        if out and a <= out[-1][1]:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return out


def _subtract(iv, cut):
    """iv minus cut (both lists of [a, b])."""
    out = []
    for a, b in iv:
        cur = a
        for c, d in cut:
            if d <= cur or c >= b:
                continue
            if c > cur:
                out.append([cur, c])
            cur = max(cur, d)
        if cur < b:
            out.append([cur, b])
    return out


def _intersect(iv, other):
    return _subtract(iv, _subtract(iv, other))


def fetch_nodes(server):
    try:
        with urllib.request.urlopen(server.rstrip("/") + "/nodes", timeout=3) as r:
            return json.loads(r.read().decode())
    except Exception:
        return None


def build(events, sounds, nodes, hours):
    now = _now()
    t0 = now - hours * 3600
    info = {}
    for e in events:
        if "mac" in e and e.get("id"):
            info.setdefault(e["mac"], {})["id"] = e["id"]
        if "mac" in e and e.get("type"):
            info.setdefault(e["mac"], {})["type"] = e["type"]

    # Rows in dashboard (nodes.csv) order with group breaks; fall back to log.
    rows, group = [], 1
    live = {}
    if nodes:
        for n in nodes:
            if n.get("separator"):
                group += 1
                continue
            rows.append({"mac": n["mac"], "id": n.get("id") or "?",
                         "type": n.get("node_type") or info.get(n["mac"], {}).get("type", "?"),
                         "group": group, "fw": n.get("firmware")})
            live[n["mac"]] = n
    else:
        for mac, d in sorted(info.items(), key=lambda kv: (len(kv[1].get("id", "")), kv[1].get("id", ""))):
            rows.append({"mac": mac, "id": d.get("id", "?"), "type": d.get("type", "?"),
                         "group": 1, "fw": None})

    # Nodes without a catalogue id (spares, bench boards) are not part of any
    # Sonoff group: give each its own group so it can't mask a group power-off.
    for r in rows:
        if not r["id"] or ":" in r["id"] or r["id"] in ("?", "None", "unknown"):
            r["group"] = "x:" + r["mac"]
            r["id"] = r["mac"][-5:]

    # Silent intervals per node, reboots, watchdog resets, crashes.
    silent, open_since, marks = {}, {}, []
    last_fw = {}
    for e in events:
        k, mac = e["k"], e.get("mac")
        if k == "stale":
            open_since.setdefault(mac, e["last"])
        elif k in ("recov", "start") and mac in open_since:
            silent.setdefault(mac, []).append([open_since.pop(mac), e["t"]])
        if k == "start":
            last_fw[mac] = e["fw"]
            kind = "wdt" if e["reset"] == "WDT" else ("crash" if e["reset"] in CRASH_REASONS else "boot")
            marks.append({"t": e["t"], "mac": mac, "kind": kind, "fw": e["fw"],
                          "reset": e["reset"] or "normal"})
        elif k == "prevd":
            marks.append({"t": e["t"], "mac": mac, "kind": "diag", "cause": e["cause"]})
    for mac, since in open_since.items():
        ts = (live.get(mac) or {}).get("last_seen_ts")
        if ts and now - ts < STALE_ONLINE_S and ts > since:
            continue
        silent.setdefault(mac, []).append([since, now])

    # Live status says silent but the log has no STALE line yet: show it too.
    for mac, n in live.items():
        ts = n.get("last_seen_ts")
        if ts and now - ts >= STALE_ONLINE_S and not any(b >= now - 1 for a, b in silent.get(mac, [])):
            silent.setdefault(mac, []).append([ts, now])

    def clip(iv):
        return [[max(a, t0), min(b, now)] for a, b in iv if b > t0]

    for r in rows:
        r["silent"] = _union(clip(silent.get(r["mac"], [])))
        r["fw"] = r["fw"] if r["fw"] not in (None, "unknown") else last_fw.get(r["mac"])
        ts = (live.get(r["mac"]) or {}).get("last_seen_ts")
        if ts is not None:
            r["online"] = (now - ts) < STALE_ONLINE_S
        else:
            r["online"] = not any(b >= now - 1 for a, b in r["silent"])
    win_marks = [m for m in marks if m["t"] >= t0]

    # Split each node's silence: "fault" while another node in its group was
    # online, "group off" while the whole group was silent (power / Sonoff).
    seen = {e["mac"] for e in events if "mac" in e and e["t"] >= t0}
    for r in rows:
        ts = (live.get(r["mac"]) or {}).get("last_seen_ts")
        r["nodata"] = r["mac"] not in seen and not (ts and ts >= t0) and not r["silent"]
    online_iv = {r["mac"]: ([] if r["nodata"] else _subtract([[t0, now]], r["silent"]))
                 for r in rows}
    for r in rows:
        others = _union([iv for o in rows if o["group"] == r["group"] and o is not r
                         for iv in online_iv[o["mac"]]])
        fault = _intersect(r["silent"], others)
        # Ignore slivers shorter than the server's STALE threshold (e.g. nodes
        # of one group booting a few seconds apart after power-on).
        r["fault"] = [iv for iv in fault if iv[1] - iv[0] >= STALE_ONLINE_S]
        r["groupoff"] = _union(_subtract(r["silent"], r["fault"]))
        del r["silent"]

    # Sonoff power lanes.
    sonoff, state = {}, {}
    for e in events:
        if e["k"] == "sonoff" and e["state"] in ("ON", "OFF"):
            n = e["n"]
            if e["state"] == "OFF" and n not in state:
                state[n] = e["t"]
            elif e["state"] == "ON" and n in state:
                sonoff.setdefault(n, []).append([state.pop(n), e["t"]])
    for n, since in state.items():
        sonoff.setdefault(n, []).append([since, now])
    sonoff = {n: clip(iv) for n, iv in sonoff.items()}

    # Hourly (or 6-hourly for long windows) buckets.
    step = 3600 if hours <= 48 else 6 * 3600
    nb = int((now - t0) // step) + 1
    start = now - nb * step
    def bucket(t):
        i = int((t - start) // step)
        return i if 0 <= i < nb else None
    reboots = {k: [0] * nb for k in ("boot", "wdt", "crash")}
    for m in win_marks:
        if m["kind"] in reboots:
            i = bucket(m["t"])
            if i is not None:
                reboots[m["kind"]][i] += 1
    det = None
    if sounds is not None:
        det = {k: [0] * nb for k in ("WHISTLE", "VOICE", "CLAP", "BIRDSONG")}
        for t, d, _ in sounds:
            i = bucket(t)
            if i is not None and d in det:
                det[d][i] += 1

    minimal = [r for r in rows if r["type"] == "echoes-minimal"]
    full = [r for r in rows if r["type"] == "echoes-full"]
    fws = {}
    for r in rows:
        if r["fw"]:
            fws[r["fw"]] = fws.get(r["fw"], 0) + 1
    faults = sum(1 for r in rows for a, b in r["fault"] if b - a >= 300)
    fault_min = [sum(1 for r in grp for a, b in r["fault"] if b - a >= 300) for grp in (
        [r for r in rows if r["type"] == "echoes-minimal"],
        [r for r in rows if r["type"] == "echoes-full"])]
    return {
        "now": now, "t0": t0, "hours": hours, "live": bool(nodes),
        "tz_offset": time.localtime(now).tm_gmtoff, "tz_name": time.strftime("%Z"),
        "rows": rows, "marks": win_marks, "sonoff": sonoff,
        "buckets": {"start": start, "step": step, "n": nb,
                    "reboots": reboots, "detections": det},
        "tiles": {
            "online": sum(r["online"] and not r["nodata"] for r in rows), "total": len(rows),
            "minimal_online": sum(r["online"] for r in minimal), "minimal": len(minimal),
            "full_online": sum(r["online"] for r in full), "full": len(full),
            "wdt": sum(1 for m in win_marks if m["kind"] == "wdt"),
            "crash": sum(1 for m in win_marks if m["kind"] == "crash"),
            "faults": faults, "faults_minimal": fault_min[0], "faults_full": fault_min[1],
            "firmware": fws,
        },
    }


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Echoes Fleet Charts</title>
<style>
:root{color-scheme:light;
 --bg:#f4f3f0;--surface:#fcfcfb;--line:#e4e2dc;--grid:#ecebe6;
 --text:#0b0b0b;--text2:#52514e;--muted:#7c7b76;
 --s1:#2a78d6;--s2:#eb6834;--s3:#1baf7a;--s4:#eda100;
 --good:#0ca30c;--warn:#fab219;--crit:#d03b3b;--off:#c9c7bf;--online:#b9d7c0;}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){color-scheme:dark;
 --bg:#111110;--surface:#1a1a19;--line:#2e2e2b;--grid:#252523;
 --text:#f0efec;--text2:#c3c2b7;--muted:#8f8e86;
 --s1:#3987e5;--s2:#d95926;--s3:#199e70;--s4:#c98500;--off:#45443f;--online:#2f6a40;}}
:root[data-theme="dark"]{color-scheme:dark;
 --bg:#111110;--surface:#1a1a19;--line:#2e2e2b;--grid:#252523;
 --text:#f0efec;--text2:#c3c2b7;--muted:#8f8e86;
 --s1:#3987e5;--s2:#d95926;--s3:#199e70;--s4:#c98500;--off:#45443f;--online:#2f6a40;}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);
 font:14px/1.4 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:1200px;margin:0 auto;padding:20px 16px 40px}
header{display:flex;flex-wrap:wrap;gap:12px;align-items:baseline;justify-content:space-between}
h1{font-size:20px;margin:0}
.sub{color:var(--text2);font-size:13px}
.ctrl{display:flex;gap:6px}
.ctrl button{border:1px solid var(--line);background:var(--surface);color:var(--text);
 border-radius:6px;padding:5px 12px;cursor:pointer;font:inherit}
.ctrl button[aria-pressed="true"]{background:var(--text);color:var(--surface);border-color:var(--text)}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:10px;margin:16px 0}
.tile{background:var(--surface);border:1px solid var(--line);border-radius:10px;padding:12px 14px}
.tile .v{font-size:28px;font-weight:600;font-variant-numeric:tabular-nums}
.tile .l{color:var(--text2);font-size:13px}
.tile .d{color:var(--muted);font-size:12px;margin-top:2px}
.card{background:var(--surface);border:1px solid var(--line);border-radius:10px;padding:14px;margin-top:12px}
.card h2{font-size:15px;margin:0 0 2px}
.card p{margin:0 0 10px;color:var(--text2);font-size:13px}
.legend{display:flex;flex-wrap:wrap;gap:14px;font-size:12px;color:var(--text2);margin:4px 0 8px}
.legend span{display:inline-flex;align-items:center;gap:6px}
.sw{width:12px;height:12px;border-radius:3px;display:inline-block}
.mk{width:0;height:12px;border-left:2px solid;display:inline-block}
svg{display:block;width:100%;height:auto;overflow:visible}
svg text{fill:var(--text2);font-size:11px}
.scroll{overflow-x:auto}
#tip{position:fixed;pointer-events:none;background:var(--text);color:var(--surface);
 padding:6px 9px;border-radius:6px;font-size:12px;white-space:pre;display:none;z-index:9;max-width:320px}
.warnbar{background:var(--surface);border:1px solid var(--line);border-left:4px solid var(--warn);
 padding:8px 12px;border-radius:8px;margin-top:10px;font-size:13px;display:none}
</style></head>
<body><main>
<header>
 <div><h1>Echoes fleet</h1><div class="sub" id="updated">loading…</div></div>
 <div class="ctrl" role="group" aria-label="Time range">
  <button data-h="6">6 h</button><button data-h="24">24 h</button><button data-h="168">7 days</button>
 </div>
</header>
<div class="warnbar" id="nolive">Live status unavailable — echoes-server (/nodes) not reachable; showing log data only.</div>
<section class="tiles" id="tiles"></section>

<section class="card">
 <h2>Node timeline</h2>
 <p>One row per node, in dashboard order and groups. Red marks a node that stopped polling while the rest of its group kept running — the lockup signature. Times in <span id="tzname">server time</span>.</p>
 <div class="legend">
  <span><i class="sw" style="background:var(--online)"></i>online</span>
  <span><i class="sw" style="background:var(--crit)"></i>✖ silent while its group was online</span>
  <span><i class="sw" style="background:var(--off)"></i>whole group silent / Sonoff off</span>
  <span><i class="mk" style="border-color:var(--text2)"></i>reboot</span>
  <span><i class="mk" style="border-color:var(--warn);border-left-width:3px"></i>⚠ watchdog self-recovery</span>
  <span><i class="mk" style="border-color:var(--crit);border-left-width:3px"></i>✖ crash / brownout</span>
 </div>
 <div class="scroll"><svg id="timeline" role="img" aria-label="Per-node online and silent periods"></svg></div>
</section>

<section class="card">
 <h2>Reboots per <span class="unit">hour</span></h2>
 <p>Normal boots (power-on, updates, remote restarts), watchdog self-recoveries, and crashes.</p>
 <div class="legend" id="rebootLegend"></div>
 <svg id="reboots" role="img" aria-label="Reboots per time bucket by type"></svg>
</section>

<section class="card" id="detCard">
 <h2>Sound detections per <span class="unit">hour</span></h2>
 <p id="detNote">ESP-NOW SOUND frames from full nodes, from the sniffer log.</p>
 <div class="legend" id="detLegend"></div>
 <svg id="detections" role="img" aria-label="Sound detections per time bucket by type"></svg>
</section>
</main>
<div id="tip"></div>
<script>
const $=s=>document.querySelector(s);
let hours=24; try{hours=+localStorage.getItem("echoesHours")||24}catch(e){}
const css=v=>getComputedStyle(document.documentElement).getPropertyValue(v).trim();
let TZ=0;const DAYS=["Sun","Mon","Tue","Wed","Thu","Fri","Sat"];
const z=t=>new Date((t+TZ)*1000),p2=n=>String(n).padStart(2,"0");
const hm=t=>{const d=z(t);return p2(d.getUTCHours())+":"+p2(d.getUTCMinutes())};
const fmtT=t=>DAYS[z(t).getUTCDay()]+" "+hm(t);
const fmtD=s=>s<3600?Math.round(s/60)+" min":(s/3600).toFixed(1)+" h";
const tip=$("#tip");
function showTip(ev,txt){tip.textContent=txt;tip.style.display="block";
 const x=Math.min(ev.clientX+12,innerWidth-tip.offsetWidth-8),y=ev.clientY+14;
 tip.style.left=x+"px";tip.style.top=y+"px";}
function hideTip(){tip.style.display="none"}
const NS="http://www.w3.org/2000/svg";
function el(tag,attrs,parent){const e=document.createElementNS(NS,tag);
 for(const k in attrs)e.setAttribute(k,attrs[k]);if(parent)parent.appendChild(e);return e;}
function hover(e,txt){e.addEventListener("mousemove",ev=>showTip(ev,txt));e.addEventListener("mouseleave",hideTip);}

function timeTicks(t0,t1,W,x){const span=t1-t0;
 const step=span<=7*3600?3600:span<=26*3600?3*3600:24*3600;
 const out=[];let t=Math.ceil((t0+TZ)/step)*step-TZ;
 for(;t<=t1;t+=step){
  out.push({x:x(t),label:step>=86400?DAYS[z(t).getUTCDay()]+" "+z(t).getUTCDate():hm(t)});}
 return out;}

function drawTimeline(D){
 const svg=$("#timeline");svg.innerHTML="";
 const L=64,R=12,rowH=13,gap=2,laneH=8;
 const groups=[...new Set(D.rows.map(r=>r.group))];
 const lanes=Object.keys(D.sonoff).map(Number).sort();
 const W=Math.max(svg.parentElement.clientWidth,640);
 const x=t=>L+(t-D.t0)/(D.now-D.t0)*(W-L-R);
 let y=4;
 // Sonoff lanes
 const laneTop=y;
 lanes.forEach(n=>{
  el("text",{x:L-6,y:y+laneH-1,"text-anchor":"end"},svg).textContent="Sonoff "+n;
  el("rect",{x:L,y:y,width:W-L-R,height:laneH,fill:"var(--grid)",rx:2},svg);
  D.sonoff[n].forEach(([a,b])=>{const r=el("rect",{x:x(a),y:y,width:Math.max(1,x(b)-x(a)),height:laneH,fill:"var(--off)",rx:2},svg);
   hover(r,`Sonoff ${n} off\n${fmtT(a)} → ${fmtT(b)} (${fmtD(b-a)})`);});
  y+=laneH+3;});
 if(lanes.length)y+=6;
 let lastGroup=null;const rowsTop=y;
 D.rows.forEach(r=>{
  if(lastGroup!==null&&r.group!==lastGroup){y+=6;}
  lastGroup=r.group;
  el("text",{x:L-6,y:y+rowH-3,"text-anchor":"end"},svg).textContent=r.id+(r.type==="echoes-minimal"?" ·m":"");
  const base=el("rect",{x:L,y:y,width:W-L-R,height:rowH-gap,fill:r.nodata?"var(--grid)":"var(--online)",rx:2},svg);
  hover(base,`${r.id}  (${r.type.replace("echoes-","")}, fw ${r.fw||"?"})\n${r.nodata?"no data in this period":r.online?"online now":"silent now"}`);
  r.groupoff.forEach(([a,b])=>{const s=el("rect",{x:x(a),y:y,width:Math.max(1,x(b)-x(a)),height:rowH-gap,fill:"var(--off)",rx:2},svg);
   hover(s,`${r.id}: whole group silent (powered off?)\n${fmtT(a)} → ${fmtT(b)} (${fmtD(b-a)})`);});
  r.fault.forEach(([a,b])=>{const s=el("rect",{x:x(a),y:y,width:Math.max(3,x(b)-x(a)),height:rowH-gap,fill:"var(--crit)",rx:2},svg);
   hover(s,`✖ ${r.id} silent while its group was online\n${fmtT(a)} → ${fmtT(b)} (${fmtD(b-a)})`);});
  r.y=y;y+=rowH;});
 const rowY={};D.rows.forEach(r=>rowY[r.mac]=r.y);
 D.marks.forEach(m=>{if(!(m.mac in rowY)||m.kind==="diag")return;
  const col=m.kind==="wdt"?"var(--warn)":m.kind==="crash"?"var(--crit)":"var(--text2)";
  const X=x(m.t),Y=rowY[m.mac];
  el("line",{x1:X,x2:X,y1:Y-1,y2:Y+rowH-gap+1,stroke:col,"stroke-width":m.kind==="boot"?1.5:3},svg);
  const hit=el("rect",{x:X-4,y:Y-1,width:8,height:rowH+1,fill:"transparent"},svg);
  const what=m.kind==="wdt"?"⚠ watchdog self-recovery":m.kind==="crash"?"✖ crash: "+m.reset:"reboot";
  hover(hit,`${what}\n${fmtT(m.t)}  fw ${m.fw}`);});
 // axis
 const yAxis=y+4;
 timeTicks(D.t0,D.now,W,x).forEach(t=>{
  el("line",{x1:t.x,x2:t.x,y1:laneTop,y2:yAxis,stroke:"var(--line)","stroke-width":1,"stroke-dasharray":"2 3"},svg).style.pointerEvents="none";
  el("text",{x:t.x,y:yAxis+12,"text-anchor":"middle"},svg).textContent=t.label;});
 svg.setAttribute("viewBox",`0 0 ${W} ${yAxis+18}`);svg.setAttribute("width",W);
 svg.parentElement.querySelectorAll("line").forEach(l=>{});
}

function drawStack(svgSel,legendSel,B,series,D){
 const svg=$(svgSel);svg.innerHTML="";svg.style.height="";
 const W=Math.max(svg.parentElement.clientWidth-4,320),H=170,L=34,R=8,T=8,Bm=22;
 const n=B.n,tot=[...Array(n)].map((_,i)=>series.reduce((s,k)=>s+k.vals[i],0));
 const max=Math.max(1,...tot),nice=max<=4?4:Math.ceil(max/4)*4;
 const y=v=>T+(H-T-Bm)*(1-v/nice);
 const bw=(W-L-R)/n,barW=Math.max(2,Math.min(28,bw-2));
 for(let g=0;g<=4;g++){const v=nice*g/4;
  el("line",{x1:L,x2:W-R,y1:y(v),y2:y(v),stroke:"var(--grid)"},svg);
  el("text",{x:L-6,y:y(v)+4,"text-anchor":"end"},svg).textContent=Math.round(v);}
 for(let i=0;i<n;i++){let acc=0;const cx=L+i*bw+(bw-barW)/2;
  const segs=series.filter(s=>s.vals[i]>0);
  segs.forEach((s,j)=>{const v=s.vals[i];const top=y(acc+v),bot=y(acc);
   const h=Math.max(1,bot-top-(j>0?2:0));
   el("rect",{x:cx,y:top,width:barW,height:h,fill:s.color,rx:j===segs.length-1?Math.min(4,barW/2):0},svg);
   acc+=v;});
  const t=B.start+i*B.step;
  const hit=el("rect",{x:L+i*bw,y:T,width:bw,height:H-T-Bm,fill:"transparent"},svg);
  const lines=series.map(s=>`${s.label}: ${s.vals[i]}`).join("\n");
  hover(hit,`${fmtT(t)} – ${fmtT(t+B.step)}\n${lines}`);}
 timeTicks(B.start,B.start+n*B.step,W,t=>L+(t-B.start)/B.step*bw).forEach(t=>{
  if(t.x<L||t.x>W-R)return;el("text",{x:t.x,y:H-6,"text-anchor":"middle"},svg).textContent=t.label;});
 svg.setAttribute("viewBox",`0 0 ${W} ${H}`);
 $(legendSel).innerHTML=series.map(s=>`<span><i class="sw" style="background:${s.color}"></i>${s.label} (${s.vals.reduce((a,b)=>a+b,0)})</span>`).join("");
}

function render(D){
 TZ=D.tz_offset||0;$("#tzname").textContent=(D.tz_name||"server")+" (server time)";
 $("#updated").textContent=`Updated ${hm(D.now)} · last ${D.hours>=168?"7 days":D.hours+" h"} · refreshes every 30 s`;
 $("#nolive").style.display=D.live?"none":"block";
 document.querySelectorAll(".unit").forEach(u=>u.textContent=D.buckets.step>3600?"6 hours":"hour");
 const T=D.tiles,fw=Object.entries(T.firmware).sort((a,b)=>b[1]-a[1]).map(([v,c])=>`${v}: ${c}`).join(" · ")||"—";
 const tiles=[
  {v:`${T.online}/${T.total}`,l:"Nodes online now",d:`minimal ${T.minimal_online}/${T.minimal} · full ${T.full_online}/${T.full}`},
  {v:T.faults,l:"✖ Unexpected silences ≥ 5 min",d:`minimal ${T.faults_minimal} · full ${T.faults_full}`},
  {v:T.wdt,l:"⚠ Watchdog self-recoveries",d:"STARTUP … RESET=WDT"},
  {v:T.crash,l:"✖ Crashes / brownouts",d:"PANIC, TASK_WDT, INT_WDT, BROWNOUT"},
  {v:Object.keys(T.firmware).length===1?Object.keys(T.firmware)[0]:Object.keys(T.firmware).length+" versions",l:"Firmware",d:fw}];
 $("#tiles").innerHTML=tiles.map(t=>`<div class="tile"><div class="v">${t.v}</div><div class="l">${t.l}</div><div class="d">${t.d}</div></div>`).join("");
 drawTimeline(D);
 const R=D.buckets.reboots;
 drawStack("#reboots","#rebootLegend",D.buckets,[
  {label:"normal boot",vals:R.boot,color:"var(--s1)"},
  {label:"⚠ watchdog",vals:R.wdt,color:"var(--warn)"},
  {label:"✖ crash",vals:R.crash,color:"var(--crit)"}],D);
 const det=D.buckets.detections;
 if(det){$("#detNote").textContent="ESP-NOW SOUND frames from full nodes, from the sniffer log.";
  drawStack("#detections","#detLegend",D.buckets,[
   {label:"whistle",vals:det.WHISTLE,color:"var(--s1)"},{label:"voice",vals:det.VOICE,color:"var(--s2)"},
   {label:"clap",vals:det.CLAP,color:"var(--s3)"},{label:"birdsong",vals:det.BIRDSONG,color:"var(--s4)"}],D);}
 else{$("#detNote").textContent="No sniffer log found. To show detections, run the sniffer with: --log /var/log/echoes/sound.jsonl";
  $("#detections").innerHTML="";$("#detections").removeAttribute("viewBox");$("#detections").style.height="0";$("#detLegend").innerHTML="";}
}
let last=null;
async function load(){try{const r=await fetch("api/data?hours="+hours);last=await r.json();render(last);}
 catch(e){$("#updated").textContent="Could not load data: "+e;}}
document.querySelectorAll(".ctrl button").forEach(b=>{b.setAttribute("aria-pressed",+b.dataset.h===hours);
 b.onclick=()=>{hours=+b.dataset.h;try{localStorage.setItem("echoesHours",hours)}catch(e){}
  document.querySelectorAll(".ctrl button").forEach(x=>x.setAttribute("aria-pressed",x===b));load();};});
addEventListener("resize",()=>last&&render(last));
load();setInterval(load,30000);
</script></body></html>
"""


def main():
    ap = argparse.ArgumentParser(description="Read-only live charts for Echoes")
    ap.add_argument("--port", type=int, default=8003)
    ap.add_argument("--log-dir", default=os.environ.get("ECHOES_LOG_DIR", "/var/log/echoes"))
    ap.add_argument("--server", default="http://127.0.0.1:8002")
    ap.add_argument("--sniffer", default="/var/log/echoes/sound.jsonl",
                    help="sniffer JSON Lines file (optional; ignored if missing)")
    a = ap.parse_args()

    logs, sniff = LogCache(a.log_dir), SnifferCache(a.sniffer)

    class H(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def _send(self, code, body, ctype):
            data = body.encode() if isinstance(body, str) else body
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            u = urlparse(self.path)
            if u.path in ("/", "/index.html"):
                self._send(200, PAGE, "text/html; charset=utf-8")
            elif u.path == "/api/data":
                try:
                    hours = float(parse_qs(u.query).get("hours", ["24"])[0])
                except ValueError:
                    hours = 24
                hours = min(max(hours, 1), 168)
                d = build(logs.refresh(), sniff.refresh(), fetch_nodes(a.server), hours)
                self._send(200, json.dumps(d), "application/json")
            else:
                self._send(404, "not found", "text/plain")

    print(f"Echoes charts on http://0.0.0.0:{a.port}  (logs: {a.log_dir}, "
          f"server: {a.server}, sniffer: {a.sniffer or 'none'})", flush=True)
    ThreadingHTTPServer(("0.0.0.0", a.port), H).serve_forever()


if __name__ == "__main__":
    main()
