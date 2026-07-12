# 🛰️ Sentinel
### Edge AI-powered Arctic sensor network that never goes offline.

![Dashboard Demo](assets/dashboard-demo.png)

Built at **cuHacking 2026** · [marybits/sentinel](https://github.com/marybits/sentinel)

---

## ❄️ The Problem

Canada's Arctic covers 4 million square kilometers and includes over 220 remote communities living with **zero reliable internet infrastructure**. The Canadian Rangers patrol these regions blind: they have no means to monitor the environment, detect perimeter threats, or receive alerts when equipment fails.

Traditional monitoring systems are built on a fragile paradigm: they stop working the moment connectivity drops. In the Arctic, that means they are effectively offline, always.
---

## 🛡️ The Solution

Sentinel is a resilient edge-AI sensor network built around a single principle: **the system must keep working, even when the connection doesn't.**

```
[ESP32 Field Node]
  DHT11 + HC-SR04
       │
       │ WiFi HTTP POST
       ▼
[Raspberry Pi 5 — QNX 8.0]          [Simulated Nodes 2–5]
  C Hub · SQLite · TFLite  ◄──────── Flask Backend
       │                                    │
       ▼                                    ▼
  Local persistence               MongoDB Atlas (alerts)
       │                                    │
       └──────────────┬─────────────────────┘
                      ▼
              [React Dashboard]
         Leaflet map · RadarTelemetry
         Alert feed · Kill/Revive demo
```

---

## 🧠 Key Features

- **Edge AI** — TFLite neural network runs directly on QNX, classifying movement as `approaching`, `passing by`, or `static/noise` from ultrasonic distance history
- **Offline-first** — field nodes buffer readings locally when the hub is unreachable and flush automatically on reconnect (store-and-forward)
- **SQLite persistence** — all telemetry stored locally on the Pi, independent of cloud
- **Live tactical dashboard** — Arctic Leaflet map, real-time radar waveform with AI classification, alert feed, battery degradation graph
- **Kill/Revive demo** — simulate node failure and recovery live during judging

---

## ⚡ Why QNX?

Linux processes tasks *when it has time*. QNX guarantees task execution within a **precise, deterministic timeframe**, same RTOS used in defense, aerospace, and medical devices.

When a perimeter breach is detected, QNX reacts in microseconds. That's the requirement in safety-critical systems.

---

## 🔧 Tech Stack

| Layer | Technology |
|---|---|
| Edge Hub OS | QNX 8.0 Non-Commercial |
| Hub program | C · SQLite3 · TFLite runtime |
| Field node | ESP32 · DHT11 · HC-SR04 |
| Backend | Python · Flask · MongoDB Atlas |
| Frontend | React · Vite · Tailwind · Leaflet · Recharts |

---

## 🚀 Getting Started

**Hub (on Pi via SSH):**
```bash
ssh qnxuser@<pi-ip>
cd ~/sentinel/hub && make
SENTINEL_GPIO_SIM=0 ./hub
```

**Backend:**
```bash
cd backend
python3 app.py &
python3 sensor_simulator.py
```

**Dashboard:**
```bash
cd dashboard && npm install && npm run dev
```

---

## 🗺️ Roadmap

- **ESP-NOW** — direct device-to-device radio, no WiFi router needed
- **LoRa** — multi-kilometer range for true Arctic deployment
- **Solar power management** — real battery monitoring for off-grid nodes

---

*Built in 36 hours by Mary Araujo & Julia · cuHacking 2026*