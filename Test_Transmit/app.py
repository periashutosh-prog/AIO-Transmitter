"""
AIO-Transmitter UDP catcher + live dashboard.

Listens for the 6-byte broadcast control packet on UDP :5000 and serves a
live DPAD + analog-gauge dashboard over HTTP via Flask on :7000 (kept off
5000 so the UDP socket and the web server never fight for the same port).

Packet format (6 bytes):
  [0] 0xA1            packet-type ID
  [1] btns low byte    DPAD_1 bits 0-4 (UP,DOWN,LEFT,RIGHT,CENTER)
  [2] btns high byte   DPAD_2 bits 5-9 (UP,DOWN,LEFT,RIGHT,CENTER)
  [3] analog_1 (0-100)
  [4] analog_2 (0-100)
  [5] link mode: 1=Broadcast, 0=Unicast

Link mode comes from the packet itself (byte 5), not from which local socket
happened to receive it. Relying on socket binding (specific-IP vs 0.0.0.0) to
infer broadcast/unicast is flaky across OSes — Windows in particular doesn't
guarantee strict most-specific-match delivery, so the same packet can show up
on both sockets and flicker between modes. Having the transmitter say so
directly is the reliable source of truth.
"""

import socket
import threading
import time

from flask import Flask, jsonify, render_template_string

UDP_PORT = 5000
HTTP_PORT = 7000
PACKET_ID = 0xA1


def get_local_ip():
    """Best-effort local LAN IP (the address the AIO-Transmitter would target
    for unicast). Uses the classic UDP-connect trick — no packet is actually
    sent, it just makes the OS pick the right outbound interface."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


LOCAL_IP = get_local_ip()

state_lock = threading.Lock()
state = {
    "connected": False,
    "last_seen": 0.0,
    "source_ip": "",
    "local_ip": LOCAL_IP,
    "link_type": "",        # "broadcast" or "unicast"
    "dpad1": [False] * 5,   # UP, DOWN, LEFT, RIGHT, CENTER
    "dpad2": [False] * 5,
    "analog1": 0,
    "analog2": 0,
    "packet_count": 0,
}


def _handle_packet(data, addr, link_type):
    if len(data) < 6 or data[0] != PACKET_ID:
        return

    btns = data[1] | (data[2] << 8)
    with state_lock:
        state["connected"] = True
        state["last_seen"] = time.time()
        state["source_ip"] = addr[0]
        state["link_type"] = link_type
        state["dpad1"] = [bool(btns & (1 << i)) for i in range(5)]
        state["dpad2"] = [bool(btns & (1 << (i + 5))) for i in range(5)]
        state["analog1"] = data[3]
        state["analog2"] = data[4]
        state["packet_count"] += 1


def udp_unicast_listener():
    # Bound to our specific IP (not 0.0.0.0) — the OS only routes packets
    # addressed exactly to this host here, never broadcast traffic, so
    # anything arriving on this socket is definitely AIO-Transmitter's
    # "Unicast" mode.
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((LOCAL_IP, UDP_PORT))
    print(f"[UDP] Unicast listener on {LOCAL_IP}:{UDP_PORT}")

    while True:
        data, addr = sock.recvfrom(64)
        _handle_packet(data, addr, "unicast")


def udp_broadcast_listener():
    # Bound to the wildcard address — broadcast datagrams (dest e.g.
    # 192.168.1.255) only match a 0.0.0.0-bound socket, since they don't
    # match the specific-IP socket above. Anything landing here is "Broadcast".
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", UDP_PORT))
    print(f"[UDP] Broadcast listener on 0.0.0.0:{UDP_PORT}")

    while True:
        data, addr = sock.recvfrom(64)
        _handle_packet(data, addr, "broadcast")


def staleness_watcher():
    # Flip "connected" off if no packet has arrived in 1.5s, so the UI
    # doesn't keep showing a frozen last-known pose after the TX stops.
    while True:
        time.sleep(0.5)
        with state_lock:
            if state["connected"] and time.time() - state["last_seen"] > 1.5:
                state["connected"] = False
                state["link_type"] = ""


app = Flask(__name__)

PAGE = """
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>AIO-Transmitter — UDP Catcher</title>
<style>
  :root {
    --bg: #0e1116;
    --panel: #161b22;
    --line: #30363d;
    --accent: #58a6ff;
    --ok: #3fb950;
    --off: #f85149;
    --text: #e6edf3;
    --dim: #8b949e;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    background: var(--bg);
    color: var(--text);
    font-family: 'Segoe UI', system-ui, sans-serif;
    display: flex;
    flex-direction: column;
    align-items: center;
    padding: 24px 12px 48px;
  }
  h1 { font-size: 18px; font-weight: 600; margin: 0 0 4px; letter-spacing: 0.5px; }
  .sub { color: var(--dim); font-size: 13px; margin-bottom: 20px; }
  .status {
    display: inline-flex; align-items: center; gap: 6px;
    padding: 4px 10px; border-radius: 12px; font-size: 12px;
    background: var(--panel); border: 1px solid var(--line);
  }
  .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--off); }
  .dot.on { background: var(--ok); box-shadow: 0 0 6px var(--ok); }

  .badge {
    display: none;
    align-items: center; gap: 5px;
    padding: 4px 10px; border-radius: 12px; font-size: 11px;
    font-weight: 600; letter-spacing: 0.5px;
    border: 1px solid var(--line);
  }
  .badge.show { display: inline-flex; }
  .badge.broadcast { color: #d29922; border-color: #d29922; background: rgba(210,153,34,0.12); }
  .badge.unicast    { color: var(--accent); border-color: var(--accent); background: rgba(88,166,255,0.12); }

  .stage {
    display: flex; gap: 28px; align-items: center; margin-top: 28px;
    flex-wrap: wrap; justify-content: center;
  }
  .panel {
    background: var(--panel); border: 1px solid var(--line);
    border-radius: 12px; padding: 18px 22px;
    display: flex; flex-direction: column; align-items: center; gap: 10px;
  }
  .panel-label { font-size: 12px; color: var(--dim); letter-spacing: 1px; text-transform: uppercase; }

  .dpad { display: grid; grid-template-columns: 36px 36px 36px; grid-template-rows: 36px 36px 36px; gap: 4px; }
  .key {
    background: #21262d; border: 1px solid var(--line); border-radius: 6px;
    display: flex; align-items: center; justify-content: center;
    font-size: 13px; color: var(--dim); transition: all 0.06s ease;
  }
  .key.active { background: var(--accent); color: #fff; border-color: var(--accent); box-shadow: 0 0 10px rgba(88,166,255,0.6); }
  .k-up    { grid-area: 1 / 2 / 2 / 3; }
  .k-left  { grid-area: 2 / 1 / 3 / 2; }
  .k-center{ grid-area: 2 / 2 / 3 / 3; }
  .k-right { grid-area: 2 / 3 / 3 / 4; }
  .k-down  { grid-area: 3 / 2 / 4 / 3; }

  .gauges { display: flex; gap: 22px; margin-top: 28px; }
  .gauge-block { display: flex; flex-direction: column; align-items: center; gap: 8px; }
  .gauge-track {
    width: 28px; height: 140px; background: #21262d; border: 1px solid var(--line);
    border-radius: 8px; position: relative; overflow: hidden;
  }
  .gauge-fill {
    position: absolute; bottom: 0; left: 0; right: 0;
    background: linear-gradient(180deg, var(--accent), #1f6feb);
    transition: height 0.08s linear;
  }
  .gauge-val { font-size: 13px; font-weight: 600; }
  .gauge-name { font-size: 11px; color: var(--dim); letter-spacing: 1px; }

  .meta { margin-top: 26px; font-size: 12px; color: var(--dim); display: flex; gap: 18px; flex-wrap: wrap; justify-content: center; }
  .meta span b { color: var(--text); }

  .tabs { display: flex; gap: 8px; margin-bottom: 22px; }
  .tab-btn {
    background: var(--panel); border: 1px solid var(--line); color: var(--dim);
    padding: 8px 16px; border-radius: 8px; font-size: 13px; font-weight: 600;
    cursor: pointer; letter-spacing: 0.3px;
  }
  .tab-btn.active { color: #fff; border-color: var(--accent); background: rgba(88,166,255,0.14); }
  .view { display: none; flex-direction: column; align-items: center; width: 100%; }
  .view.active { display: flex; }

  .sim-wrap { display: flex; flex-direction: column; align-items: center; gap: 14px; margin-top: 6px; }
  #simCanvas { border-radius: 10px; border: 1px solid var(--line); display: block; }
  .sim-help { font-size: 12px; color: var(--dim); text-align: center; }
  .sim-help b { color: var(--text); }
  .reset-btn {
    background: var(--panel); border: 1px solid var(--line); color: var(--text);
    padding: 6px 14px; border-radius: 8px; font-size: 12px; cursor: pointer;
  }
  .reset-btn:hover { border-color: var(--accent); }
</style>
</head>
<body>
  <h1>AIO-TRANSMITTER &mdash; UDP CATCHER</h1>
  <div class="sub">This device: <b id="localIp">-</b>:5000 &nbsp;|&nbsp; Dashboard on :7000</div>

  <div class="tabs">
    <button class="tab-btn active" data-view="dashboardView">Live Dashboard</button>
    <button class="tab-btn" data-view="simView">RC Car Simulator</button>
  </div>

  <div id="dashboardView" class="view active">
  <div class="status">
    <span class="dot" id="dot"></span><span id="statusText">Waiting for packets...</span>
    <span class="badge broadcast" id="badgeBroadcast">&#128225; BROADCAST</span>
    <span class="badge unicast" id="badgeUnicast">&#127919; UNICAST</span>
  </div>

  <div class="stage">
    <div class="panel">
      <div class="panel-label">DPAD 1</div>
      <div class="dpad">
        <div class="key k-up" id="d1-up">&#9650;</div>
        <div class="key k-left" id="d1-left">&#9664;</div>
        <div class="key k-center" id="d1-center">&#9679;</div>
        <div class="key k-right" id="d1-right">&#9654;</div>
        <div class="key k-down" id="d1-down">&#9660;</div>
      </div>
    </div>

    <div class="gauges">
      <div class="gauge-block">
        <div class="gauge-track"><div class="gauge-fill" id="bar1" style="height:0%"></div></div>
        <div class="gauge-val" id="val1">0</div>
        <div class="gauge-name">ANALOG 1</div>
      </div>
      <div class="gauge-block">
        <div class="gauge-track"><div class="gauge-fill" id="bar2" style="height:0%"></div></div>
        <div class="gauge-val" id="val2">0</div>
        <div class="gauge-name">ANALOG 2</div>
      </div>
    </div>

    <div class="panel">
      <div class="panel-label">DPAD 2</div>
      <div class="dpad">
        <div class="key k-up" id="d2-up">&#9650;</div>
        <div class="key k-left" id="d2-left">&#9664;</div>
        <div class="key k-center" id="d2-center">&#9679;</div>
        <div class="key k-right" id="d2-right">&#9654;</div>
        <div class="key k-down" id="d2-down">&#9660;</div>
      </div>
    </div>
  </div>

  <div class="meta">
    <span>Source IP: <b id="srcIp">-</b></span>
    <span>Packets received: <b id="pktCount">0</b></span>
  </div>
  </div>

  <div id="simView" class="view">
    <div class="sim-wrap">
      <canvas id="simCanvas" width="640" height="400"></canvas>
      <div class="sim-help">
        DPAD1 UP/DOWN = forward/back, DPAD2 LEFT/RIGHT = tank-turn (pivots in place
        with no forward input) &mdash; or keyboard <b>W S</b> / <b>A D</b>
      </div>
      <button class="reset-btn" id="simReset">Reset car</button>
    </div>
  </div>

<script>
  const dpadKeys = ["up", "down", "left", "right", "center"];
  let localIpSet = false;

  // [up, down, left, right, center] for each pad — shared with the simulator loop
  // below, so real transmitter input and the RC Car Simulator drive off the same
  // source. Sim uses DPAD1 UP/DOWN for throttle and DPAD2 LEFT/RIGHT for tank-turn.
  let latestDpad1 = [false, false, false, false, false];
  let latestDpad2 = [false, false, false, false, false];

  async function poll() {
    try {
      const res = await fetch("/state");
      const s = await res.json();

      if (!localIpSet && s.local_ip) {
        document.getElementById("localIp").textContent = s.local_ip;
        localIpSet = true;
      }

      document.getElementById("dot").classList.toggle("on", s.connected);
      document.getElementById("statusText").textContent = s.connected
        ? "Receiving packets"
        : "Waiting for packets...";

      document.getElementById("badgeBroadcast").classList.toggle("show", s.connected && s.link_type === "broadcast");
      document.getElementById("badgeUnicast").classList.toggle("show", s.connected && s.link_type === "unicast");

      dpadKeys.forEach((k, i) => {
        document.getElementById("d1-" + k).classList.toggle("active", s.dpad1[i]);
        document.getElementById("d2-" + k).classList.toggle("active", s.dpad2[i]);
      });

      document.getElementById("bar1").style.height = s.analog1 + "%";
      document.getElementById("bar2").style.height = s.analog2 + "%";
      document.getElementById("val1").textContent = s.analog1;
      document.getElementById("val2").textContent = s.analog2;

      document.getElementById("srcIp").textContent = s.source_ip || "-";
      document.getElementById("pktCount").textContent = s.packet_count;

      latestDpad1 = s.dpad1;
      latestDpad2 = s.dpad2;
    } catch (e) {
      // server momentarily unreachable - ignore and retry next tick
    }
  }

  setInterval(poll, 60);
  poll();

  // ---- Tab switching ----
  document.querySelectorAll(".tab-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      document.querySelectorAll(".tab-btn").forEach((b) => b.classList.remove("active"));
      document.querySelectorAll(".view").forEach((v) => v.classList.remove("active"));
      btn.classList.add("active");
      document.getElementById(btn.dataset.view).classList.add("active");
    });
  });

  // ---- RC Car Simulator ----
  // Tank/differential steering: LEFT and RIGHT are pure rotation, independent of
  // throttle, so the car can pivot in place — this is what distinguishes it from
  // Ackermann (car-style) steering, where turning requires forward motion.
  const keys = { w: false, a: false, s: false, d: false };
  window.addEventListener("keydown", (e) => {
    const k = e.key.toLowerCase();
    if (k in keys) { keys[k] = true; e.preventDefault(); }
  });
  window.addEventListener("keyup", (e) => {
    const k = e.key.toLowerCase();
    if (k in keys) { keys[k] = false; e.preventDefault(); }
  });

  const simCanvas = document.getElementById("simCanvas");
  const simCtx = simCanvas.getContext("2d");
  const MAX_SPEED = 130;  // px/s
  const TURN_RATE = 2.6;  // rad/s
  const CAR_MARGIN = 20;

  function freshCar() {
    return { x: simCanvas.width / 2, y: simCanvas.height / 2, heading: 0 };
  }
  let car = freshCar();
  document.getElementById("simReset").addEventListener("click", () => { car = freshCar(); });

  function trackPath() {
    const pad = 40, r = 90, w = simCanvas.width, h = simCanvas.height;
    simCtx.beginPath();
    simCtx.moveTo(pad + r, pad);
    simCtx.lineTo(w - pad - r, pad);
    simCtx.arcTo(w - pad, pad, w - pad, pad + r, r);
    simCtx.lineTo(w - pad, h - pad - r);
    simCtx.arcTo(w - pad, h - pad, w - pad - r, h - pad, r);
    simCtx.lineTo(pad + r, h - pad);
    simCtx.arcTo(pad, h - pad, pad, h - pad - r, r);
    simCtx.lineTo(pad, pad + r);
    simCtx.arcTo(pad, pad, pad + r, pad, r);
    simCtx.closePath();
  }

  function drawTrack() {
    simCtx.fillStyle = "#132015";
    simCtx.fillRect(0, 0, simCanvas.width, simCanvas.height);

    simCtx.lineJoin = "round";
    simCtx.strokeStyle = "#0b0d10";
    simCtx.lineWidth = 70;
    trackPath();
    simCtx.stroke();

    simCtx.strokeStyle = "#e6edf3";
    simCtx.setLineDash([14, 14]);
    simCtx.lineWidth = 2;
    trackPath();
    simCtx.stroke();
    simCtx.setLineDash([]);
  }

  function drawCar() {
    simCtx.save();
    simCtx.translate(car.x, car.y);
    simCtx.rotate(car.heading);
    simCtx.fillStyle = "#58a6ff";
    simCtx.beginPath();
    simCtx.moveTo(12, 0);
    simCtx.lineTo(-9, 7);
    simCtx.lineTo(-5, 0);
    simCtx.lineTo(-9, -7);
    simCtx.closePath();
    simCtx.fill();
    simCtx.strokeStyle = "#1f6feb";
    simCtx.lineWidth = 1.5;
    simCtx.stroke();
    simCtx.restore();
  }

  let lastTs = null;
  function simStep(ts) {
    if (lastTs === null) lastTs = ts;
    const dt = Math.min((ts - lastTs) / 1000, 0.05);
    lastTs = ts;

    const up    = keys.w || latestDpad1[0];
    const down  = keys.s || latestDpad1[1];
    const left  = keys.a || latestDpad2[2];
    const right = keys.d || latestDpad2[3];

    const throttle = (up ? 1 : 0) - (down ? 1 : 0);
    const turn = (right ? 1 : 0) - (left ? 1 : 0);

    car.heading += turn * TURN_RATE * dt;
    const speed = throttle * MAX_SPEED;
    car.x += speed * Math.cos(car.heading) * dt;
    car.y += speed * Math.sin(car.heading) * dt;

    car.x = Math.max(CAR_MARGIN, Math.min(simCanvas.width - CAR_MARGIN, car.x));
    car.y = Math.max(CAR_MARGIN, Math.min(simCanvas.height - CAR_MARGIN, car.y));

    drawTrack();
    drawCar();

    requestAnimationFrame(simStep);
  }
  requestAnimationFrame(simStep);
</script>
</body>
</html>
"""


@app.route("/")
def index():
    return render_template_string(PAGE)


@app.route("/state")
def get_state():
    with state_lock:
        return jsonify(state)


if __name__ == "__main__":
    threading.Thread(target=udp_unicast_listener, daemon=True).start()
    threading.Thread(target=udp_broadcast_listener, daemon=True).start()
    threading.Thread(target=staleness_watcher, daemon=True).start()
    print(f"[HTTP] Dashboard at http://{LOCAL_IP}:{HTTP_PORT}")
    app.run(host="0.0.0.0", port=HTTP_PORT, debug=False)
