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

from dotenv import load_dotenv
load_dotenv()  # reads .env in this folder and sets the env vars below

from flask import Flask, request, jsonify
from flask_cors import CORS
from pymongo import MongoClient
from pymongo.server_api import ServerApi

# ----------------------------------------------------------------
# Config (env vars, with hackathon-friendly fallbacks)
# ----------------------------------------------------------------
MONGO_URI = os.environ.get("MONGO_URI", "mongodb+srv://<user>:<password>@cluster0.xxxxx.mongodb.net/?retryWrites=true&w=majority")

SQLITE_PATH = os.environ.get("SQLITE_PATH", "sentinel.db")  # lives next to app.py

OFFLINE_THRESHOLD_SEC = 8   # no data for this long -> node marked offline
PROXIMITY_ALERT_CM = 30     # distance below this -> proximity alert
TEMP_ALERT_C = 35           # temp above this -> temperature alert
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
        temp REAL,
        humidity REAL,
        distance REAL,
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
# node_id -> {temp, humidity, distance, battery, last_seen, status, history: [battery readings]}
nodes_state = {}


def classify_status(node_id, data, last_seen_age):
    """Return 'red' | 'yellow' | 'green' for a node's current reading."""
    if last_seen_age > OFFLINE_THRESHOLD_SEC:
        return "red"
    if data.get("distance") is not None and data["distance"] < PROXIMITY_ALERT_CM:
        return "red"
    if data.get("temp") is not None and data["temp"] > TEMP_ALERT_C:
        return "red"
    if data.get("battery") is not None and data["battery"] < BATTERY_LOW_PCT:
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
            "INSERT INTO readings (node_id, temp, humidity, distance, battery, timestamp) VALUES (?, ?, ?, ?, ?, ?)",
            (
                node_id,
                data.get("temp"),
                data.get("humidity"),
                data.get("distance"),
                data.get("battery"),
                time.time(),
            ),
        )
        sqlite_conn.commit()
    except Exception as e:
        print(f"[sqlite] write failed for {node_id}: {e}")


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
      "distance": 120.0,
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

    # Handle batched store-and-forward flush from an offline node
    if "batch" in data:
        readings = data["batch"]
        for r in readings:
            _ingest_reading(node_id, r, now)
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
    state["distance"] = data.get("distance")
    state["battery"] = data.get("battery")
    state["last_seen"] = now

    if data.get("battery") is not None:
        state["battery_history"].append((now, float(data["battery"])))
        state["battery_history"] = state["battery_history"][-50:]  # cap history

    status = classify_status(node_id, data, last_seen_age=0)
    state["status"] = status

    # Fire alerts on state transitions (avoid spamming one per reading)
    if status == "red" and prev_status != "red":
        if data.get("distance") is not None and data["distance"] < PROXIMITY_ALERT_CM:
            write_alert(node_id, "proximity", f"{node_id}: object within {data['distance']}cm", "critical")
        elif data.get("temp") is not None and data["temp"] > TEMP_ALERT_C:
            write_alert(node_id, "temperature", f"{node_id}: temperature at {data['temp']}°C", "critical")
    if status == "yellow" and prev_status != "yellow":
        if data.get("battery") is not None and data["battery"] < BATTERY_LOW_PCT:
            write_alert(node_id, "battery", f"{node_id}: battery low at {data['battery']}%", "warning")

    if not was_known:
        write_event(node_id, "connected", f"{node_id} came online")

    write_to_sqlite(node_id, data)


@app.route("/nodes", methods=["GET"])
def get_nodes():
    now = time.time()
    result = []
    for node_id, state in nodes_state.items():
        age = now - state.get("last_seen", 0)
        status = "red" if age > OFFLINE_THRESHOLD_SEC else state.get("status", "green")
        result.append({
            "node_id": node_id,
            "temp": state.get("temp"),
            "humidity": state.get("humidity"),
            "distance": state.get("distance"),
            "battery": state.get("battery"),
            "status": status,
            "seconds_since_seen": round(age, 1),
            "battery_minutes_left": estimate_battery_minutes_left(node_id),
        })
    return jsonify(result)


@app.route("/alerts", methods=["GET"])
def get_alerts():
    limit = int(request.args.get("limit", 50))
    alerts = list(
        alerts_collection.find({}, {"_id": 0})
        .sort("timestamp", -1)
        .limit(limit)
    )
    for a in alerts:
        a["timestamp"] = a["timestamp"].isoformat()
    return jsonify(alerts)


@app.route("/history", methods=["GET"])
def get_history():
    """Time-series readings for graphs. ?node_id=node_2&limit=100"""
    node_id = request.args.get("node_id")
    limit = int(request.args.get("limit", 100))

    if node_id:
        rows = sqlite_conn.execute(
            "SELECT node_id, temp, humidity, distance, battery, timestamp FROM readings "
            "WHERE node_id = ? ORDER BY timestamp DESC LIMIT ?",
            (node_id, limit),
        ).fetchall()
    else:
        rows = sqlite_conn.execute(
            "SELECT node_id, temp, humidity, distance, battery, timestamp FROM readings "
            "ORDER BY timestamp DESC LIMIT ?",
            (limit,),
        ).fetchall()

    result = [
        {
            "node_id": r[0],
            "temp": r[1],
            "humidity": r[2],
            "distance": r[3],
            "battery": r[4],
            "timestamp": r[5],
        }
        for r in reversed(rows)  # oldest first, easier for charts
    ]
    return jsonify(result)


@app.route("/health", methods=["GET"])
def health():
    return jsonify({"status": "ok", "nodes_tracked": len(nodes_state)})


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=True)