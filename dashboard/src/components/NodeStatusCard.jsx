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

function getNodeStatus(node) {
  if (!node.online) return "offline";
  if (node.proximity_alert) return "alert";

  const tempSeverity = getTemperatureSeverity(node.temperature_c);
  if (tempSeverity === "critical") return "critical";
  if (tempSeverity === "warning") return "warning";

  if (node.humidity_pct > HUMIDITY_HIGH_PCT) return "warning";
  if (node.battery_pct !== null && node.battery_pct < BATTERY_LOW_PCT) return "warning";

  return "normal";
}

const STATUS_META = {
  normal:   { label: "ONLINE",   dot: "bg-emerald-500", text: "text-emerald-500" },
  warning:  { label: "WARNING",  dot: "bg-amber-500",   text: "text-amber-500" },
  critical: { label: "CRITICAL", dot: "bg-rose-500",    text: "text-rose-500" },
  alert:    { label: "ALERT",    dot: "bg-rose-500",    text: "text-rose-500" },
  offline:  { label: "OFFLINE",  dot: "bg-rose-500",    text: "text-rose-500" },
};

function Metric({ label, value }) {
  return (
    <div className="flex flex-col">
      <span className="text-slate-500 text-[10px] uppercase tracking-wider">{label}</span>
      <span className="text-white">{value}</span>
    </div>
  );
}

export default function NodeStatusCard({ node }) {
  const status = getNodeStatus(node);
  const meta = STATUS_META[status];

  return (
    <div className="bg-slate-900 border border-slate-800 rounded-sm p-4 flex flex-col gap-3">
      <div className="flex items-center justify-between">
        <div>
          <p className="font-mono text-xs text-slate-400 uppercase tracking-wider">{node.node_id}</p>
          <p className="text-slate-300 text-sm">{node.location}</p>
        </div>
        <div className="flex items-center gap-2">
          <span className={`h-2 w-2 rounded-full ${meta.dot}`} />
          <span className={`font-mono text-xs uppercase tracking-widest ${meta.text}`}>{meta.label}</span>
        </div>
      </div>

      <div className="grid grid-cols-2 gap-2 font-mono text-sm">
        <Metric label="TEMP" value={`${node.temperature_c.toFixed(1)}°C`} />
        <Metric label="HUM" value={`${node.humidity_pct}%`} />
        {node.distance_cm !== null && <Metric label="DIST" value={`${node.distance_cm}cm`} />}
        {node.battery_pct !== null && <Metric label="BATT" value={`${node.battery_pct}%`} />}
      </div>
    </div>
  );
}