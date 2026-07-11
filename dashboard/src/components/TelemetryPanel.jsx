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
import { mockEnvironmentalData, mockBatteryData } from "../data/mockTelemetryData";
import { FLASK_BASE_URL } from "../config";

// Both tabs are live now. Power Systems polls node_4 (simulated, readings
// land in Flask's SQLite `readings` table via POST /data). Environmental
// polls node_1 — its temp/humidity/distance history lives in the Pi's own
// sensor_data table, so Flask's GET /history proxies node_1 requests to the
// Pi's GET /history route instead of querying the local table.
const HISTORY_POLL_MS = 5000;
const HISTORY_LIMIT = 40;

function formatTimeLabel(unixSeconds) {
  const d = new Date(unixSeconds * 1000);
  return d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
}

// Generic "poll GET /history?node_id=X, map rows, keep last-known-good on
// failure" hook shared by both charts.
function useHistoryPoll(nodeId, mapRow, initialData) {
  const [data, setData] = useState(initialData);

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
  return useHistoryPoll(
    "node_4",
    (r) =>
      r.battery !== null && r.battery !== undefined
        ? { time: formatTimeLabel(r.timestamp), battery_pct: r.battery }
        : null,
    mockBatteryData
  );
}

function useEnvironmentalHistory() {
  return useHistoryPoll(
    "node_1",
    (r) =>
      r.temp !== null && r.temp !== undefined
        ? {
            time: formatTimeLabel(r.timestamp),
            temperature: r.temp,
            proximity: r.distance_cm,
          }
        : null,
    mockEnvironmentalData
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
  { id: "environmental", label: "ENVIRONMENTAL" },
  { id: "power", label: "POWER SYSTEMS" },
];

function LegendDot({ color, label }) {
  return (
    <span className="flex items-center gap-1.5">
      <span className="h-2 w-2 rounded-full" style={{ backgroundColor: color }} />
      <span className="font-mono text-[10px] text-slate-500 uppercase tracking-wider">{label}</span>
    </span>
  );
}

function EnvironmentalChart() {
  const data = useEnvironmentalHistory();

  return (
    <ResponsiveContainer width="100%" height={280}>
      <LineChart data={data}>
        <CartesianGrid stroke="#1e293b" strokeDasharray="3 3" />
        <XAxis dataKey="time" tick={AXIS_TICK_STYLE} axisLine={AXIS_LINE} tickLine={AXIS_LINE} />
        <YAxis
          yAxisId="temp"
          tick={AXIS_TICK_STYLE}
          axisLine={AXIS_LINE}
          tickLine={AXIS_LINE}
          label={{ value: "°C", angle: -90, position: "insideLeft", fill: "#94a3b8", fontSize: 12 }}
        />
        <YAxis
          yAxisId="prox"
          orientation="right"
          tick={AXIS_TICK_STYLE}
          axisLine={AXIS_LINE}
          tickLine={AXIS_LINE}
          label={{ value: "cm", angle: 90, position: "insideRight", fill: "#94a3b8", fontSize: 12 }}
        />
        <Tooltip contentStyle={TOOLTIP_STYLE} labelStyle={{ color: "#94a3b8" }} />
        <ReferenceLine yAxisId="prox" y={30} stroke="#f43f5e" strokeDasharray="4 4" />
        <Line yAxisId="temp" type="monotone" dataKey="temperature" stroke="#10b981" strokeWidth={2} dot={false} />
        <Line yAxisId="prox" type="monotone" dataKey="proximity" stroke="#f59e0b" strokeWidth={2} dot={false} />
      </LineChart>
    </ResponsiveContainer>
  );
}

function PowerChart() {
  const data = useBatteryHistory();
  const latest = data.length > 0 ? data[data.length - 1].battery_pct : 0;
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

export default function TelemetryPanel() {
  const [activeTab, setActiveTab] = useState("environmental");

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

      {activeTab === "environmental" ? (
        <>
          <div className="flex items-center justify-between mb-2">
            <p className="font-mono text-[10px] text-slate-500 uppercase tracking-wider">
              NODE_1 · INUVIK (live)
            </p>
            <div className="flex items-center gap-4">
              <LegendDot color="#10b981" label="Temperature" />
              <LegendDot color="#f59e0b" label="Proximity" />
            </div>
          </div>
          <EnvironmentalChart />
        </>
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