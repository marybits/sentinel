'''Simulates 4 fake sensor nodes (node_2 - node_5) sending readings to the
Flask backend every 2s. Includes:
  - gradual battery drain
  - occasional random anomalies (proximity, temperature spike)
  - occasional random "offline" periods, with store-and-forward:
    readings queue up locally while offline, then flush as a batch
    on reconnect (this is what your dashboard's sync animation reacts to)

Run:
    python sensor_simulator.py
'''
import random
import time
import requests

BACKEND_URL = "http://localhost:5000/data"
NODE_IDS = ["node_2", "node_3", "node_4", "node_5"]
SEND_INTERVAL_SEC = 2

# Per-node simulated state
state = {
    node_id: {
        "temp": round(random.uniform(18, 24), 1),
        "humidity": round(random.uniform(30, 60), 1),
        "distance": round(random.uniform(100, 400), 1),
        "battery": round(random.uniform(70, 100), 1),
        "offline": False,
        "buffer": [],  # readings queued while offline
    }
    for node_id in NODE_IDS
}


def drift(value, amount, lo, hi):
    value += random.uniform(-amount, amount)
    return max(lo, min(hi, value))


def maybe_trigger_anomaly(s):
    #potential trigger for anomalies.
    if random.random() < 0.03:
        s["distance"] = round(random.uniform(5, 25), 1)  # proximity alert
    if random.random() < 0.02:
        s["temp"] = round(random.uniform(36, 42), 1)  # temp alert


def maybe_toggle_offline(node_id, s):
    if not s["offline"] and random.random() < 0.01:
        s["offline"] = True
        print(f"[{node_id}] went OFFLINE")
    elif s["offline"] and random.random() < 0.15:
        s["offline"] = False
        print(f"[{node_id}] back ONLINE — flushing {len(s['buffer'])} buffered readings")
        try:
            requests.post(BACKEND_URL, json={"node_id": node_id, "batch": s["buffer"]}, timeout=3)
        except requests.exceptions.RequestException as e:
            print(f"[{node_id}] flush failed: {e}")
        s["buffer"] = []


def build_reading(s):
    s["temp"] = drift(s["temp"], 0.5, 15, 42)
    s["humidity"] = drift(s["humidity"], 1, 20, 80)
    s["distance"] = drift(s["distance"], 15, 5, 400)
    s["battery"] = max(0, s["battery"] - random.uniform(0.02, 0.08))  # steady drain
    maybe_trigger_anomaly(s)
    return {
        "temp": round(s["temp"], 1),
        "humidity": round(s["humidity"], 1),
        "distance": round(s["distance"], 1),
        "battery": round(s["battery"], 1),
    }


def main():
    print(f"Simulating {len(NODE_IDS)} nodes -> {BACKEND_URL}  (Ctrl+C to stop)")
    while True:
        for node_id in NODE_IDS:
            s = state[node_id]
            maybe_toggle_offline(node_id, s)
            reading = build_reading(s)

            if s["offline"]:
                reading["node_id"] = node_id
                reading["ts"] = time.time()
                s["buffer"].append(reading)
                continue

            payload = {"node_id": node_id, **reading}
            try:
                requests.post(BACKEND_URL, json=payload, timeout=3)
            except requests.exceptions.RequestException as e:
                print(f"[{node_id}] send failed, backend down?: {e}")

        time.sleep(SEND_INTERVAL_SEC)


if __name__ == "__main__":
    main()