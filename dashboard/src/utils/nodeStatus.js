// Shared node status classification — single source of truth so status
// cards, alerts, and map markers can't drift out of sync.
// Mirrors backend/app.py's classify_status() thresholds exactly, so the
// card badge always agrees with whatever triggered the alert in the feed.
// Backend has no separate "warning" tier for high temp (jumps straight to
// critical at TEMP_ALERT_C), so warning/critical high are set equal here.
const TEMP_WARNING_LOW_C = -20;   // matches TEMP_LOW_WARN_C
const TEMP_WARNING_HIGH_C = 32;   // matches TEMP_ALERT_C
const TEMP_CRITICAL_LOW_C = -40;  // matches TEMP_LOW_CRITICAL_C
const TEMP_CRITICAL_HIGH_C = 32;  // matches TEMP_ALERT_C
const HUMIDITY_HIGH_PCT = 80;     // frontend-only nicety — backend doesn't alert on this
const BATTERY_LOW_PCT = 20;       // matches BATTERY_LOW_PCT

function getTemperatureSeverity(tempC) {
  if (tempC < TEMP_CRITICAL_LOW_C || tempC > TEMP_CRITICAL_HIGH_C) return "critical";
  if (tempC < TEMP_WARNING_LOW_C || tempC > TEMP_WARNING_HIGH_C) return "warning";
  return "normal";
}

export function getNodeStatus(node) {
  if (!node.online) return "offline";
  if (node.proximity_alert) return "alert";

  const tempSeverity = getTemperatureSeverity(node.temperature_c);
  if (tempSeverity === "critical") return "critical";
  if (tempSeverity === "warning") return "warning";

  if (node.humidity_pct > HUMIDITY_HIGH_PCT) return "warning";
  if (node.battery_pct !== null && node.battery_pct < BATTERY_LOW_PCT) return "warning";

  if (node.is_syncing) return "syncing";

  return "normal";
}

export const STATUS_META = {
  normal:   { label: "ONLINE",   dot: "bg-emerald-500", text: "text-emerald-500" },
  warning:  { label: "WARNING",  dot: "bg-amber-500",   text: "text-amber-500" },
  critical: { label: "CRITICAL", dot: "bg-rose-500",    text: "text-rose-500" },
  alert:    { label: "ALERT",    dot: "bg-rose-500",    text: "text-rose-500" },
  offline:  { label: "OFFLINE",  dot: "bg-rose-500",    text: "text-rose-500" },
  syncing:  { label: "SYNCING",  dot: "bg-cyan-500 animate-pulse", text: "text-cyan-500 animate-pulse" },
};

// Same palette as raw hex, for contexts that can't take Tailwind classes
// (e.g. Leaflet's pathOptions).
export const STATUS_HEX = {
  normal: "#10b981",
  warning: "#f59e0b",
  critical: "#f43f5e",
  alert: "#f43f5e",
  offline: "#f43f5e",
  syncing: "#06b6d4",
};