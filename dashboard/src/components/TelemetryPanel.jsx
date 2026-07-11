import { useState } from "react";
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
  return (
    <ResponsiveContainer width="100%" height={280}>
      <LineChart data={mockEnvironmentalData}>
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
  const latest = mockBatteryData[mockBatteryData.length - 1].battery_pct;
  const lineColor = latest < 20 ? "#f43f5e" : "#10b981";

  return (
    <ResponsiveContainer width="100%" height={280}>
      <LineChart data={mockBatteryData}>
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
              NODE_1 · INUVIK
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
            NODE_4 · RESOLUTE — Battery Degradation
          </p>
          <PowerChart />
        </>
      )}
    </div>
  );
}