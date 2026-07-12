import { getNodeStatus, STATUS_META } from "../utils/nodeStatus";

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
  const label = status === "syncing" ? `SYNCING ${node.sync_backlog} PENDING...` : meta.label;

  return (
    <div className="bg-slate-900 border border-slate-800 rounded-sm p-4 flex flex-col gap-3">
      <div className="flex items-center justify-between gap-2 flex-wrap">
        <div>
          <p className="font-mono text-xs text-slate-400 uppercase tracking-wider">{node.node_id}</p>
          <p className="text-slate-300 text-sm">{node.location}</p>
        </div>
        <div className="flex items-center gap-2">
          <span className={`h-2 w-2 rounded-full ${meta.dot}`} />
          <span className={`font-mono text-xs uppercase tracking-widest ${meta.text}`}>{label}</span>
        </div>
      </div>

      <div className="grid grid-cols-2 gap-2 font-mono text-sm">
        <Metric label="TEMP" value={`${(node.temperature_c ?? 0).toFixed(1)}°C`} />
        <Metric label="HUM" value={`${node.humidity_pct}%`} />
        {node.distance_cm !== null && <Metric label="DIST" value={`${node.distance_cm}cm`} />}
        {node.battery_pct !== null && <Metric label="BATT" value={`${node.battery_pct}%`} />}
      </div>
    </div>
  );
}