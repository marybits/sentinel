import {
  LineChart,
  Line,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  ResponsiveContainer,
} from "recharts";

// Tactical/cyberpunk radar HUD for node_1's ultrasonic movement classifier
// (hub/main.c's trigger_radar_classification -> infer_radar.py ->
// radar_model.tflite). Replaces the old temp+proximity chart for node_1
// only — other nodes keep TelemetryPanel's existing Environmental view.

const AXIS_TICK_STYLE = { fill: "#64748b", fontFamily: "monospace", fontSize: 11 };
const AXIS_LINE = { stroke: "#1e293b" };
const TOOLTIP_STYLE = {
  backgroundColor: "#000000",
  border: "1px solid #1e293b",
  borderRadius: 2,
  fontFamily: "monospace",
  fontSize: 12,
};

const CLASSIFICATION_META = {
  static: { label: "STATIC / NOISE", color: "#39ff6a", pulse: false },
  approaching: { label: "HUMAN APPROACH", color: "#ff2d55", pulse: true },
  passing: { label: "PASSING BY", color: "#faff00", pulse: false },
  unknown: { label: "UNKNOWN / NO SIGNAL", color: "#94a3b8", pulse: false },
};

export default function RadarTelemetry({ distanceHistory, aiClassification, nodeId, location }) {
  const meta = CLASSIFICATION_META[aiClassification] ?? CLASSIFICATION_META.unknown;
  const hasData = Array.isArray(distanceHistory) && distanceHistory.length > 0;

  const chartData = hasData
    ? distanceHistory.map((d, i) => ({
        t: `T-${distanceHistory.length - 1 - i}`,
        distance: d,
      }))
    : [];

  return (
    <div
      className="rounded-sm p-4 border bg-black transition-colors"
      style={{
        borderColor: meta.color,
        boxShadow: `0 0 16px 0 ${meta.color}55, inset 0 0 28px 0 ${meta.color}14`,
      }}
    >
      <div className="flex items-center justify-between gap-2 mb-3 flex-wrap">
        <p className="font-mono text-[10px] text-slate-500 uppercase tracking-widest">
          Radar · {nodeId} · {location}
        </p>
        <div className="flex items-center gap-2">
          <span
            className={`h-2 w-2 rounded-full ${meta.pulse ? "animate-pulse" : ""}`}
            style={{ backgroundColor: meta.color, boxShadow: `0 0 8px 2px ${meta.color}` }}
          />
          <span
            className={`font-mono text-[10px] uppercase tracking-widest ${meta.pulse ? "animate-pulse" : ""}`}
            style={{ color: meta.color }}
          >
            {hasData ? "Tracking" : "Idle"}
          </span>
        </div>
      </div>

      {!hasData ? (
        <div className="h-[220px] flex flex-col items-center justify-center gap-3">
          <span className="h-2 w-2 rounded-full bg-slate-600 animate-pulse" />
          <p className="font-mono text-xs text-slate-600 uppercase tracking-[0.2em]">
            Awaiting radar data...
          </p>
        </div>
      ) : (
        <div className="relative h-[220px]">
          <ResponsiveContainer width="100%" height="100%">
            <LineChart data={chartData} margin={{ top: 28, right: 8, left: -12, bottom: 0 }}>
              <CartesianGrid stroke="#1e293b" strokeDasharray="3 3" />
              <XAxis dataKey="t" tick={AXIS_TICK_STYLE} axisLine={AXIS_LINE} tickLine={AXIS_LINE} />
              <YAxis
                tick={AXIS_TICK_STYLE}
                axisLine={AXIS_LINE}
                tickLine={AXIS_LINE}
                label={{ value: "cm", angle: -90, position: "insideLeft", fill: "#64748b", fontSize: 11 }}
              />
              <Tooltip contentStyle={TOOLTIP_STYLE} labelStyle={{ color: "#94a3b8" }} />
              <Line
                type="monotone"
                dataKey="distance"
                stroke={meta.color}
                strokeWidth={2.5}
                dot={{ r: 2, fill: meta.color, stroke: meta.color }}
                isAnimationActive={false}
              />
            </LineChart>
          </ResponsiveContainer>

          <div className="pointer-events-none absolute inset-x-0 top-0 flex justify-center">
            <span
              className={`font-mono text-xs sm:text-sm font-bold uppercase tracking-wider px-3 py-1 rounded-sm bg-black/70 ${
                meta.pulse ? "animate-pulse" : ""
              }`}
              style={{ color: meta.color, textShadow: `0 0 8px ${meta.color}, 0 0 20px ${meta.color}` }}
            >
              AI Classification: {meta.label}
            </span>
          </div>
        </div>
      )}
    </div>
  );
}
