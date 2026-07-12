import { useEffect, useState } from "react";
import {
  LineChart,
  Line,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  ResponsiveContainer,
  ReferenceLine,
} from "recharts";
import { FLASK_BASE_URL } from "../config";
import RadarTelemetry from "./RadarTelemetry";

// Power Systems polls node_4 (simulated, readings land in Flask's SQLite
// `readings` table via POST /data) via GET /history, same as before.
//
// The old Environmental tab (temp+proximity chart for node_1, via its own
// GET /history poll) is gone — node_1 pivoted from a Pi Camera+OpenCV
// intruder classifier to an HC-SR04 + TFLite movement classifier. That
// data (distance_history/ai_classification) rides along on the same
// GET /nodes poll that already drives the node cards/map (see
// apiAdapters.js's useSentinelData), so it's passed in via the `nodes`
// prop instead of a second independent poll.
const HISTORY_POLL_MS = 5000;
const HISTORY_LIMIT = 40;

function formatTimeLabel(unixSeconds) {
  const d = new Date(unixSeconds * 1000);
  return d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
}

// Generic "poll GET /history?node_id=X, map rows, keep last-known-good on
// failure" hook. Starts empty — no mock fallback — so an empty chart
// honestly means "no live data yet", not fake numbers.
function useHistoryPoll(nodeId, mapRow) {
  const [data, setData] = useState([]);

  useEffect(() => {
    let cancelled = false;
    let timeoutId;

    async function poll() {
      try {
        const res = await fetch(
          `${FLASK_BASE_URL}/history?node_id=${nodeId}&limit=${HISTORY_LIMIT}`
        );
        if (!res.ok) throw new Error(`Backend responded ${res.status}`);
        const rows = await res.json();

        if (!cancelled && rows.length > 0) {
          setData(rows.map(mapRow).filter(Boolean));
        }
      } catch (err) {
        // Backend down or unreachable — keep showing last-known-good data.
        console.error(`History poll failed for ${nodeId}, keeping last known state:`, err);
      } finally {
        if (!cancelled) {
          timeoutId = setTimeout(poll, HISTORY_POLL_MS);
        }
      }
    }

    poll();

    return () => {
      cancelled = true;
      clearTimeout(timeoutId);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [nodeId]);

  return data;
}

function useBatteryHistory() {
  return useHistoryPoll("node_4", (r) =>
    r.battery !== null && r.battery !== undefined
      ? { time: formatTimeLabel(r.timestamp), battery_pct: r.battery }
      : null
  );
}

const AXIS_TICK_STYLE = {
  fill: "#94a3b8",
  fontFamily: "monospace",
  fontSize: 12,
};
const AXIS_LINE = { stroke: "#1e293b" };

const TOOLTIP_STYLE = {
  backgroundColor: "#0f172a",
  border: "1px solid #1e293b",
  borderRadius: 2,
  fontFamily: "monospace",
  fontSize: 12,
};

const TABS = [
  { id: "radar", label: "RADAR" },
  { id: "power", label: "POWER SYSTEMS" },
];

function ChartPlaceholder() {
  return (
    <div className="h-[280px] flex items-center justify-center">
      <p className="font-mono text-xs text-slate-600 uppercase tracking-wider">
        Waiting for data...
      </p>
    </div>
  );
}

function PowerChart() {
  const data = useBatteryHistory();
  if (data.length === 0) return <ChartPlaceholder />;

  const latest = data[data.length - 1].battery_pct;
  const lineColor = latest < 20 ? "#f43f5e" : "#10b981";

  return (
    <ResponsiveContainer width="100%" height={280}>
      <LineChart data={data}>
        <CartesianGrid stroke="#1e293b" strokeDasharray="3 3" />
        <XAxis dataKey="time" tick={AXIS_TICK_STYLE} axisLine={AXIS_LINE} tickLine={AXIS_LINE} />
        <YAxis
          domain={[0, 100]}
          tick={AXIS_TICK_STYLE}
          axisLine={AXIS_LINE}
          tickLine={AXIS_LINE}
          label={{ value: "%", angle: -90, position: "insideLeft", fill: "#94a3b8", fontSize: 12 }}
        />
        <Tooltip contentStyle={TOOLTIP_STYLE} labelStyle={{ color: "#94a3b8" }} />
        <ReferenceLine
          y={20}
          stroke="#f43f5e"
          strokeDasharray="4 4"
          label={{ value: "20% THRESHOLD", position: "insideTopRight", fill: "#f43f5e", fontSize: 10, fontFamily: "monospace" }}
        />
        <Line type="monotone" dataKey="battery_pct" stroke={lineColor} strokeWidth={2} dot={false} />
      </LineChart>
    </ResponsiveContainer>
  );
}

export default function TelemetryPanel({ nodes = [] }) {
  const [activeTab, setActiveTab] = useState("radar");
  const node1 = nodes.find((n) => n.node_id === "node_1");

  return (
    <div className="bg-slate-900 border border-slate-800 rounded-sm p-4">
      <div className="flex items-center gap-6 border-b border-slate-800 mb-4">
        {TABS.map((tab) => (
          <button
            key={tab.id}
            onClick={() => setActiveTab(tab.id)}
            className={`font-mono text-sm uppercase tracking-wider pb-2 -mb-px border-b-2 transition-colors ${
              activeTab === tab.id
                ? "border-emerald-500 text-emerald-500"
                : "border-transparent text-slate-500 hover:text-slate-300"
            }`}
          >
            {tab.label}
          </button>
        ))}
      </div>

      {activeTab === "radar" ? (
        <RadarTelemetry
          distanceHistory={node1?.distance_history}
          aiClassification={node1?.ai_classification}
          nodeId="node_1"
          location={node1?.location ?? "Inuvik"}
        />
      ) : (
        <>
          <p className="font-mono text-[10px] text-slate-500 uppercase tracking-wider mb-2">
            NODE_4 · RESOLUTE — Battery Degradation (live)
          </p>
          <PowerChart />
        </>
      )}
    </div>
  );
}
