// Adapts the Flask API's responses into the canonical shapes our
// components expect, and owns the live fetch/poll loop against that API.
// Three real-world wrinkles this papers over:
//
// 1. GET /nodes returns a MIXED array: node_1 comes straight from the Pi
//    (hub/main.c's shape), nodes 2-5 come from the backend's simulator
//    loop (a different shape). normalizeNode() reconciles both.
// 2. GET /alerts merges MongoDB alerts (type/severity, ISO timestamp, no
//    id) with Pi alerts (type/classification, unix timestamp, has id).
//    normalizeAlert() reconciles both.
// 3. is_syncing/sync_backlog only exist for simulated nodes (2-5) — the
//    Pi's own POST /sensor-data protocol has no batch-flush concept yet,
//    so node_1 never reports a sync in progress. See backend/app.py's
//    SYNC_DISPLAY_SEC for how long a flush stays visible after it happens.
// 4. distance_history/ai_classification only exist for node_1 (the radar
//    movement classifier — see hub/main.c's trigger_radar_classification
//    and RadarTelemetry.jsx). Flask passes the Pi's GET /nodes response
//    through untouched, so these fields need no backend changes at all.

import { useEffect, useState } from "react";
import { FLASK_BASE_URL, POLL_INTERVAL_MS } from "../config";

const OFFLINE_AGE_SEC = 15; // mirrors backend/app.py's OFFLINE_THRESHOLD_SEC

export function normalizeNode(raw) {
  const isPiNode = "temperature_c" in raw;

  if (isPiNode) {
    return {
      node_id: raw.node_id,
      location: raw.location,
      lat: raw.lat,
      lon: raw.lon,
      temperature_c: raw.temperature_c,
      humidity_pct: raw.humidity_pct,
      distance_cm: raw.distance_cm,
      proximity_alert: raw.proximity_alert,
      battery_pct: null,
      online: raw.online,
      is_syncing: false,
      sync_backlog: 0,
      last_seen: raw.last_seen,
      distance_history: raw.distance_history ?? [],
      ai_classification: raw.ai_classification || "unknown",
    };
  }

  // Simulated node shape: { node_id, location, lat, lon, temp, humidity,
  // battery, status, seconds_since_seen, battery_minutes_left }
  return {
    node_id: raw.node_id,
    location: raw.location,
    lat: raw.lat,
    lon: raw.lon,
    temperature_c: raw.temp,
    humidity_pct: raw.humidity,
    distance_cm: null,
    proximity_alert: false,
    battery_pct: raw.battery,
    online: (raw.seconds_since_seen ?? Infinity) <= OFFLINE_AGE_SEC,
    is_syncing: raw.is_syncing ?? false,
    sync_backlog: raw.sync_backlog ?? 0,
    last_seen: null,
  };
}

function toUnixSeconds(timestamp) {
  if (typeof timestamp === "number") return timestamp;
  const parsed = Date.parse(timestamp);
  return Number.isNaN(parsed) ? Math.floor(Date.now() / 1000) : Math.floor(parsed / 1000);
}

// Pi alerts don't carry a severity field — derive one from type so they
// color the same way Mongo alerts (which do have severity) do.
const PI_ALERT_SEVERITY = {
  proximity_alert: "critical",
  node_offline: "critical",
  temp_high: "warning",
  temp_low: "warning",
  humidity_high: "warning",
};

export function normalizeAlert(raw) {
  return {
    id: raw.id ?? `${raw.node_id}-${raw.type}-${raw.timestamp}`,
    node_id: raw.node_id,
    type: raw.type,
    message: raw.message,
    classification: raw.classification ?? "",
    severity: raw.severity ?? PI_ALERT_SEVERITY[raw.type] ?? "warning",
    timestamp: toUnixSeconds(raw.timestamp),
  };
}

// Live data source for the whole dashboard — polls GET /nodes + GET /alerts
// on Flask every POLL_INTERVAL_MS, normalizes both, and tracks connection
// state so the UI can distinguish "never connected yet" from "was live,
// now failing" instead of just going stale silently.
//
// connectionStatus:
//   "connecting"   — no successful poll yet (first load, or Flask down from the start)
//   "live"         — most recent poll succeeded
//   "reconnecting" — was live before, current poll is failing (keeps last-known-good data on screen)
export function useSentinelData() {
  const [nodes, setNodes] = useState([]);
  const [alerts, setAlerts] = useState([]);
  const [connectionStatus, setConnectionStatus] = useState("connecting");

  useEffect(() => {
    let cancelled = false;
    let timeoutId;
    let everConnected = false;

    async function poll() {
      try {
        const [nodesRes, alertsRes] = await Promise.all([
          fetch(`${FLASK_BASE_URL}/nodes`),
          fetch(`${FLASK_BASE_URL}/alerts`),
        ]);

        if (!nodesRes.ok || !alertsRes.ok) {
          throw new Error(`Backend responded ${nodesRes.status}/${alertsRes.status}`);
        }

        const [rawNodes, rawAlerts] = await Promise.all([
          nodesRes.json(),
          alertsRes.json(),
        ]);

        if (!cancelled) {
          setNodes(rawNodes.map(normalizeNode));
          setAlerts(rawAlerts.map(normalizeAlert));
          setConnectionStatus("live");
          everConnected = true;
        }
      } catch (err) {
        // Flask unreachable or returned an error — keep showing last-known-good
        // data (if any) instead of crashing, and surface a status the UI can
        // render instead of silently going stale.
        console.error("Poll failed:", err);
        if (!cancelled) {
          setConnectionStatus(everConnected ? "reconnecting" : "connecting");
        }
      } finally {
        if (!cancelled) {
          timeoutId = setTimeout(poll, POLL_INTERVAL_MS);
        }
      }
    }

    poll();

    return () => {
      cancelled = true;
      clearTimeout(timeoutId);
    };
  }, []);

  return { nodes, alerts, connectionStatus };
}