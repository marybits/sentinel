const SEVERITY_META = {
  critical: { dot: "bg-rose-500", text: "text-rose-500" },
  warning: { dot: "bg-amber-500", text: "text-amber-500" },
};

function formatRelativeTime(unixSeconds) {
  const deltaSec = Math.max(0, Math.floor(Date.now() / 1000) - unixSeconds);
  if (deltaSec < 60) return `${deltaSec}s ago`;
  const deltaMin = Math.floor(deltaSec / 60);
  if (deltaMin < 60) return `${deltaMin}m ago`;
  return `${Math.floor(deltaMin / 60)}h ago`;
}

function humanizeType(type) {
  return type.replace(/_/g, " ").toUpperCase();
}

function AlertRow({ alert }) {
  const meta = SEVERITY_META[alert.severity] ?? SEVERITY_META.warning;

  return (
    <div className="flex items-start gap-3 border-b border-slate-800 px-3 py-2 last:border-b-0">
      <span className={`mt-1 h-2 w-2 shrink-0 rounded-full ${meta.dot}`} />
      <div className="flex-1 min-w-0">
        <div className="flex items-center justify-between gap-2">
          <span className={`font-mono text-[10px] uppercase tracking-wider ${meta.text}`}>
            {humanizeType(alert.type)}
          </span>
          <span className="font-mono text-[10px] text-slate-500 shrink-0">
            {formatRelativeTime(alert.timestamp)}
          </span>
        </div>
        <p className="text-slate-300 text-sm">{alert.message}</p>
      </div>
    </div>
  );
}

export default function AlertFeed({ alerts }) {
  const sorted = [...alerts].sort((a, b) => b.timestamp - a.timestamp);

  return (
    <div className="bg-slate-900 border border-slate-800 rounded-sm flex flex-col">
      <div className="px-3 py-2 border-b border-slate-800">
        <h2 className="font-mono text-xs text-slate-400 uppercase tracking-widest">
          Alert Feed
        </h2>
      </div>
      <div className="max-h-80 overflow-y-auto">
        {sorted.length === 0 ? (
          <p className="text-slate-500 text-sm px-3 py-4">No alerts.</p>
        ) : (
          sorted.map((alert) => <AlertRow key={alert.id} alert={alert} />)
        )}
      </div>
    </div>
  );
}