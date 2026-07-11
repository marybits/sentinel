// Adapts the Flask API's responses into the canonical shapes our
// components expect. Two real-world wrinkles this papers over:
//
// 1. GET /nodes returns a MIXED array: node_1 comes straight from the Pi
//    (hub/main.c's shape), nodes 2-5 come from the backend's simulator
//    loop (a different shape). normalizeNode() reconciles both.
// 2. GET /alerts merges MongoDB alerts (type/severity, ISO timestamp, no
//    id) with Pi alerts (type/classification, unix timestamp, has id).
//    normalizeAlert() reconciles both.

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
    is_syncing: false,
    sync_backlog: 0,
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