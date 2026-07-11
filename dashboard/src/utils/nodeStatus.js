// Shared node status classification — single source of truth so status
// cards, alerts, and map markers can't drift out of sync.
const TEMP_WARNING_LOW_C = -35;
const TEMP_WARNING_HIGH_C = 15;
const TEMP_CRITICAL_LOW_C = -45;
const TEMP_CRITICAL_HIGH_C = 25;
const HUMIDITY_HIGH_PCT = 80;
const BATTERY_LOW_PCT = 20;

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