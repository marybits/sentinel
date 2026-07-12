"""
Sentinel backend — Flask API

Endpoints:
  POST /data    <- nodes (real ESP32 or simulator) send readings here
  GET  /nodes   -> dashboard polls this for current status of all nodes
  GET  /alerts  -> dashboard polls this for the alert feed

Run:
    python app.py
Runs on http://localhost:5000 by default.
"""

import os
import time
import sqlite3
from datetime import datetime, timezone
import requests

from dotenv import load_dotenv
load_dotenv()  # reads .env in this folder and sets the env vars below

from flask import Flask, request, jsonify
from flask_cors import CORS
from pymongo import MongoClient
from pymongo.server_api import ServerApi

# raspberry pi
PI_URL = os.environ.get("PI_URL", "http://172.20.10.9:8080")
PI_TIMEOUT_SEC = 2  # keep short so a dead Pi doesn't hang the dashboard's request


#env vars
MONGO_URI = os.environ.get("MONGO_URI", "mongodb+srv://<user>:<password>@cluster0.xxxxx.mongodb.net/?retryWrites=true&w=majority")

SQLITE_PATH = os.environ.get("SQLITE_PATH", "sentinel.db")  # lives next to app.py


# alert thresholds
OFFLINE_THRESHOLD_SEC = 15  # no data for this long -> node marked offline
SYNC_DISPLAY_SEC = 5        # how long GET /nodes keeps reporting is_syncing after a batch flush
TEMP_ALERT_C = 32           # temp above this -> high-temp alert (overheating/fire risk)
TEMP_LOW_WARN_C = -20       # temp below this -> cold warning (battery drain, equipment stress)
TEMP_LOW_CRITICAL_C = -40   # temp below this -> critical cold alert (frostbite/hypothermia/equipment failure)
BATTERY_LOW_PCT = 20        # battery below this -> low battery alert

# ----------------------------------------------------------------
# App + DB setup
# ----------------------------------------------------------------
app = Flask(__name__)
CORS(app)  # dashboard runs on a different port/origin — needs this

mongo_client = MongoClient(MONGO_URI, server_api=ServerApi("1"))
db = mongo_client["sentinel"]
alerts_collection = db["alerts"]
events_collection = db["events"]

# SQLite for time-series telemetry (readings history, for the graphs)
sqlite_conn = sqlite3.connect(SQLITE_PATH, check_same_thread=False)
sqlite_conn.execute("""
    CREATE TABLE IF NOT EXISTS readings (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        node_id TEXT NOT NULL,
        location TEXT,
        lat REAL,
        lon REAL,
        temp REAL,
        humidity REAL,
        battery REAL,
        timestamp REAL NOT NULL
    )
""")
sqlite_conn.commit()

# ----------------------------------------------------------------
# In-memory state — fast reads for the dashboard, no query round trip.
# This is the source of truth for GET /nodes. InfluxDB is written to
# in parallel purely for the time-series graphs.
# ----------------------------------------------------------------
# node_id -> {temp, humidity, battery, last_seen, status, history: [battery readings]}
nodes_state = {}

# ----------------------------------------------------------------
# Demo kill-switch — for the live judging demo, fakes a node going dark
# on command. sensor_simulator.py is a separate OS process we don't
# control the lifecycle of (and don't want to — it's still generating
# realistic drift/battery drain in the background), so instead of
# signaling it directly, we intercept its POSTs here: while a node_id is
# in g_killed_nodes, incoming POST /data for it is accepted (so the
# simulator sees a normal 200 and doesn't trip its own error handling)
# but diverted into kill_buffer instead of updating live state. GET
# /nodes reports it offline immediately (not after OFFLINE_THRESHOLD_SEC)
# so judges see the reaction the instant the button is pressed. Revive
# flushes kill_buffer through the same _ingest_reading() path the
# simulator's own reconnect-flush uses, so the sync animation fires too.
# ----------------------------------------------------------------
g_killed_nodes = set()
DEMO_KILLABLE_NODES = {"node_2", "node_3", "node_4", "node_5"}  # node_1 is real hardware


def classify_status(node_id, data, last_seen_age):
    if data.get("temp") is not None and data["temp"] > TEMP_ALERT_C:
        return "red"
    if data.get("temp") is not None and data["temp"] < TEMP_LOW_CRITICAL_C:
        return "red"
    if data.get("battery") is not None and data["battery"] < BATTERY_LOW_PCT:
        return "yellow"
    if data.get("temp") is not None and data["temp"] < TEMP_LOW_WARN_C:
        return "yellow"
    return "green"


def estimate_battery_minutes_left(node_id):
    """Simple linear drain-rate estimate from the last few readings."""
    history = nodes_state.get(node_id, {}).get("battery_history", [])
    if len(history) < 2:
        return None

    # Use the most recent readings, weighted toward recency
    recent = history[-6:]
    (t0, b0), (t1, b1) = recent[0], recent[-1]
    elapsed_sec = t1 - t0
    if elapsed_sec <= 0:
        return None

    drain_per_sec = (b0 - b1) / elapsed_sec
    if drain_per_sec <= 0:
        return None  # battery not draining (or charging) — can't estimate

    seconds_left = b1 / drain_per_sec
    return round(seconds_left / 60, 1)


def write_alert(node_id, alert_type, message, severity="warning"):
    """Persist an alert event to MongoDB Atlas."""
    doc = {
        "node_id": node_id,
        "type": alert_type,
        "message": message,
        "severity": severity,
        "timestamp": datetime.now(timezone.utc),
    }
    alerts_collection.insert_one(doc)


def write_event(node_id, event_type, message):
    """Persist a non-alert event (offline, reconnect, sync) to MongoDB Atlas."""
    doc = {
        "node_id": node_id,
        "type": event_type,
        "message": message,
        "timestamp": datetime.now(timezone.utc),
    }
    events_collection.insert_one(doc)


def write_to_sqlite(node_id, data):
    try:
        sqlite_conn.execute(
            "INSERT INTO readings (node_id, location, lat, lon, temp, humidity, battery, timestamp) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            (
                node_id,
                data.get("location"),
                data.get("lat"),
                data.get("lon"),
                data.get("temp"),
                data.get("humidity"),
                data.get("battery"),
                time.time(),
            ),
        )
        sqlite_conn.commit()
    except Exception as e:
        print(f"[sqlite] write failed for {node_id}: {e}")

def fetch_pi_nodes():
    '''Fetch node_1's live status from the Pi. Returns [] if Pi is unreachable —
    Flask should keep serving simulated nodes even if the Pi drops offline.'''
    try:
        resp = requests.get(f"{PI_URL}/nodes", timeout=PI_TIMEOUT_SEC)
        resp.raise_for_status()
        return resp.json()
    except (requests.exceptions.RequestException, ValueError) as e:
        print(f"[pi] /nodes fetch failed: {e}")
        return []

def fetch_pi_alerts():
    '''Fetch alert events from the Pi. Returns [] if unreachable.'''
    try:
        resp = requests.get(f"{PI_URL}/alerts", timeout=PI_TIMEOUT_SEC)
        resp.raise_for_status()
        return resp.json()
    except (requests.exceptions.RequestException, ValueError) as e:
        print(f"[pi] /alerts fetch failed: {e}")
        return []

def fetch_pi_history(node_id, limit):
    '''Fetch time-series readings for a node from the Pi's own sensor_data
    table (currently just node_1 — its temp/humidity/proximity history never
    passes through Flask's /data, so it isn't in our local SQLite). Returns
    [] if unreachable.'''
    try:
        resp = requests.get(
            f"{PI_URL}/history",
            params={"node_id": node_id, "limit": limit},
            timeout=PI_TIMEOUT_SEC,
        )
        resp.raise_for_status()
        return resp.json()
    except (requests.exceptions.RequestException, ValueError) as e:
        print(f"[pi] /history fetch failed: {e}")
        return []
# ----------------------------------------------------------------
# Routes
# ----------------------------------------------------------------

@app.route("/data", methods=["POST"])
def receive_data():
    """
    Expected JSON body, from real ESP32 or simulator:
    {
      "node_id": "node_1",
      "temp": 22.5,
      "humidity": 45.0,
      "battery": 87.0
    }
    Simulator may also send a batch on reconnect:
    { "node_id": "node_2", "batch": [ {...}, {...}, ... ] }
    """
    data = request.get_json(force=True)
    if not data or "node_id" not in data:
        return jsonify({"error": "node_id required"}), 400

    node_id = data["node_id"]
    now = time.time()

    if node_id in g_killed_nodes:
        # Demo kill switch is on for this node — buffer instead of
        # applying live, same shape as the simulator's own offline
        # buffer, so /demo/revive-node/<node_id> can flush it through
        # the normal batch path below. Still returns 200 so the
        # simulator's own retry/offline logic doesn't get involved.
        state = nodes_state.setdefault(node_id, {"battery_history": []})
        buffer = state.setdefault("kill_buffer", [])
        if "batch" in data:
            buffer.extend(data["batch"])
        else:
            buffer.append(data)
        state["kill_buffer"] = buffer[-50:]  # cap, same as sensor_simulator.py's own buffer
        return jsonify({"status": "ok", "buffered": True}), 200

    # Handle batched store-and-forward flush from an offline node
    if "batch" in data:
        readings = data["batch"]
        for r in readings:
            _ingest_reading(node_id, r, now)
        # Record the flush so GET /nodes can report is_syncing for a few
        # seconds — the flush itself completes within this single request,
        # so there's no window where a poll would catch it "in progress";
        # this is what actually drives the dashboard's sync animation.
        state = nodes_state.setdefault(node_id, {"battery_history": []})
        state["last_sync_count"] = len(readings)
        state["last_sync_at"] = now
        write_event(node_id, "sync", f"{len(readings)} buffered readings synced")
        return jsonify({"status": "ok", "synced": len(readings)}), 200

    _ingest_reading(node_id, data, now)
    return jsonify({"status": "ok"}), 200


def _ingest_reading(node_id, data, now):
    was_known = node_id in nodes_state
    prev_status = nodes_state.get(node_id, {}).get("status")

    state = nodes_state.setdefault(node_id, {"battery_history": []})
    state["temp"] = data.get("temp")
    state["humidity"] = data.get("humidity")
    state["battery"] = data.get("battery")
    state["last_seen"] = now
    
    state["location"] = data.get("location")
    state["lat"] = data.get("lat")
    state["lon"] = data.get("lon")

    if data.get("battery") is not None:
        state["battery_history"].append((now, float(data["battery"])))
        state["battery_history"] = state["battery_history"][-50:]  # cap history

    status = classify_status(node_id, data, last_seen_age=0)
    state["status"] = status

    # Fire alerts on state transitions (avoid spamming one per reading)
    if status == "red" and prev_status != "red":
        if data.get("temp") is not None and data["temp"] > TEMP_ALERT_C:
            write_alert(node_id, "temperature", f"{node_id}: temperature at {data['temp']}°C", "critical")
        elif data.get("temp") is not None and data["temp"] < TEMP_LOW_CRITICAL_C:
            write_alert(node_id, "temperature_cold", f"{node_id}: extreme cold at {data['temp']}°C — frostbite/equipment failure risk", "critical")
    if status == "yellow" and prev_status != "yellow":
        if data.get("battery") is not None and data["battery"] < BATTERY_LOW_PCT:
            write_alert(node_id, "battery", f"{node_id}: battery low at {data['battery']}%", "warning")
        elif data.get("temp") is not None and data["temp"] < TEMP_LOW_WARN_C:
            write_alert(node_id, "temperature_cold", f"{node_id}: cold approaching danger threshold at {data['temp']}°C", "warning")

    if not was_known:
        write_event(node_id, "connected", f"{node_id} came online")

    write_to_sqlite(node_id, data)


@app.route("/nodes", methods=["GET"])
def get_nodes():
    now = time.time()
    result = []

    # Real node(s) from the Pi
    pi_nodes = fetch_pi_nodes()
    result.extend(pi_nodes)

    # Simulated nodes (node_2 - node_5)
    for node_id, state in nodes_state.items():
        if node_id in g_killed_nodes:
            # Force offline immediately rather than waiting out
            # OFFLINE_THRESHOLD_SEC — the demo wants an instant reaction
            # the moment KILL NODE is pressed.
            age = OFFLINE_THRESHOLD_SEC + 1
            status = "red"
        else:
            age = now - state.get("last_seen", 0)
            status = "red" if age > OFFLINE_THRESHOLD_SEC else state.get("status", "green")

        last_sync_at = state.get("last_sync_at")
        is_syncing = last_sync_at is not None and (now - last_sync_at) < SYNC_DISPLAY_SEC

        result.append({
            "node_id": node_id,
            "location": state.get("location"),
            "lat": state.get("lat"),
            "lon": state.get("lon"),
            "temp": state.get("temp"),
            "humidity": state.get("humidity"),
            "battery": state.get("battery"),
            "status": status,
            "seconds_since_seen": round(age, 1),
            "battery_minutes_left": estimate_battery_minutes_left(node_id),
            "is_syncing": is_syncing,
            "sync_backlog": state.get("last_sync_count", 0) if is_syncing else 0,
        })
    return jsonify(result)


@app.route("/demo/kill-node/<node_id>", methods=["POST"])
def demo_kill_node(node_id):
    """Live-demo kill switch. node_1 is real Pi hardware, not ours to fake
    offline from here — only node_2..node_5 (sensor_simulator.py) qualify."""
    if node_id not in DEMO_KILLABLE_NODES:
        return jsonify({"error": f"{node_id} can't be killed — only {sorted(DEMO_KILLABLE_NODES)} are demo nodes"}), 400

    g_killed_nodes.add(node_id)
    nodes_state.setdefault(node_id, {"battery_history": []})
    write_alert(node_id, "node_offline", f"{node_id}: manually disconnected (demo)", "critical")
    write_event(node_id, "killed", f"{node_id} killed for demo")
    return jsonify({"status": "ok", "node_id": node_id, "killed": True}), 200


@app.route("/demo/revive-node/<node_id>", methods=["POST"])
def demo_revive_node(node_id):
    """Clears the kill switch and flushes whatever POSTs piled up in
    kill_buffer while the node was "dead", through the same
    _ingest_reading() path the simulator's own reconnect-flush uses — so
    GET /nodes reports is_syncing and the dashboard plays the sync
    animation, same as a real store-and-forward reconnect."""
    if node_id not in DEMO_KILLABLE_NODES:
        return jsonify({"error": f"{node_id} can't be revived — only {sorted(DEMO_KILLABLE_NODES)} are demo nodes"}), 400

    g_killed_nodes.discard(node_id)
    state = nodes_state.setdefault(node_id, {"battery_history": []})
    buffer = state.pop("kill_buffer", [])
    now = time.time()

    for r in buffer:
        _ingest_reading(node_id, r, now)

    state["last_sync_count"] = len(buffer)
    state["last_sync_at"] = now
    state["last_seen"] = now
    write_event(node_id, "revived", f"{node_id} revived for demo — {len(buffer)} buffered readings flushed")
    return jsonify({"status": "ok", "node_id": node_id, "revived": True, "flushed": len(buffer)}), 200


def _alert_sort_key(alert):
    """Mongo alerts carry an ISO string timestamp, Pi alerts carry a unix
    int — sorting the merged list on the raw field crashes with
    TypeError: '<' not supported between instances of 'int' and 'str'.
    Normalize both to a comparable unix float here."""
    ts = alert.get("timestamp", 0)
    if isinstance(ts, str):
        try:
            return datetime.fromisoformat(ts).timestamp()
        except (ValueError, TypeError):
            return 0
    return ts or 0


@app.route("/alerts", methods=["GET"])
def get_alerts():
    limit = int(request.args.get("limit", 50))

    #mongo
    try:
        sim_alerts = list(
            alerts_collection.find({}, {"_id": 0})
            .sort("timestamp", -1)
            .limit(limit)
        )
        for a in sim_alerts:
            a["timestamp"] = a["timestamp"].isoformat()
    except Exception as e:
        # Mongo Atlas unreachable/misconfigured — don't let the whole
        # endpoint 500 just because the cloud side is down.
        print(f"[mongo] /alerts fetch failed: {e}")
        sim_alerts = []

    #pi
    pi_alerts = fetch_pi_alerts()

    combined = sim_alerts + pi_alerts
    combined.sort(key=_alert_sort_key, reverse=True)

    return jsonify(combined[:limit])



@app.route("/history", methods=["GET"])
def get_history():
    """Time-series readings for graphs. ?node_id=node_2&limit=100

    node_1 (the Pi) keeps its own sensor_data history — its readings never
    pass through POST /data, so our local SQLite has nothing for it. Proxy
    those requests to the Pi's GET /history instead of querying locally.
    """
    node_id = request.args.get("node_id")
    limit = int(request.args.get("limit", 100))

    if node_id == "node_1":
        pi_rows = fetch_pi_history(node_id, limit)
        result = [
            {
                "node_id": r.get("node_id"),
                "location": r.get("location"),
                "lat": None,
                "lon": None,
                "temp": r.get("temperature"),
                "humidity": r.get("humidity"),
                "battery": None,
                "distance_cm": r.get("distance_cm"),
                "proximity_alert": r.get("proximity_alert"),
                "timestamp": r.get("timestamp"),
            }
            for r in pi_rows  # Pi already returns oldest first
        ]
        return jsonify(result)

    if node_id:
        rows = sqlite_conn.execute(
            "SELECT node_id, location, lat, lon, temp, humidity, battery, timestamp FROM readings "
            "WHERE node_id = ? ORDER BY timestamp DESC LIMIT ?",
            (node_id, limit),
        ).fetchall()
    else:
        rows = sqlite_conn.execute(
            "SELECT node_id, location, lat, lon, temp, humidity, battery, timestamp FROM readings "
            "ORDER BY timestamp DESC LIMIT ?",
            (limit,),
        ).fetchall()

    result = [
        {
            "node_id": r[0],
            "location": r[1],
            "lat": r[2],
            "lon": r[3],
            "temp": r[4],
            "humidity": r[5],
            "battery": r[6],
            "distance_cm": None,
            "proximity_alert": None,
            "timestamp": r[7],
        }
        for r in reversed(rows) # oldest first
    ]
    return jsonify(result)


@app.route("/health", methods=["GET"])
def health():
    return jsonify({"status": "ok", "nodes_tracked": len(nodes_state)})


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=True)