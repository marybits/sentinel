// Static mock time-series for TelemetryPanel (Task 3.4 / 3.5).
// Environmental series follows node_1 (Inuvik). The proximity dip at
// 14:22 mirrors the "person" alert already in mockAlertData.js.
export const mockEnvironmentalData = [
  { time: "14:00", temperature: -7.8, proximity: 210 },
  { time: "14:02", temperature: -8.1, proximity: 198 },
  { time: "14:04", temperature: -8.4, proximity: 205 },
  { time: "14:06", temperature: -8.6, proximity: 190 },
  { time: "14:08", temperature: -8.9, proximity: 175 },
  { time: "14:10", temperature: -9.1, proximity: 240 },
  { time: "14:12", temperature: -9.3, proximity: 260 },
  { time: "14:14", temperature: -9.0, proximity: 230 },
  { time: "14:16", temperature: -8.7, proximity: 200 },
  { time: "14:18", temperature: -8.5, proximity: 180 },
  { time: "14:20", temperature: -8.6, proximity: 150 },
  { time: "14:22", temperature: -8.5, proximity: 22 },
  { time: "14:24", temperature: -8.4, proximity: 195 },
  { time: "14:26", temperature: -8.5, proximity: 182 },
];

// Battery degradation series follows node_4 (Resolute) — ends at 15%,
// matching its battery_pct in mockSensorData.js.
export const mockBatteryData = [
  { time: "10:00", battery_pct: 47 },
  { time: "10:30", battery_pct: 43 },
  { time: "11:00", battery_pct: 39 },
  { time: "11:30", battery_pct: 35 },
  { time: "12:00", battery_pct: 31 },
  { time: "12:30", battery_pct: 27 },
  { time: "13:00", battery_pct: 24 },
  { time: "13:30", battery_pct: 21 },
  { time: "14:00", battery_pct: 18 },
  { time: "14:30", battery_pct: 16 },
  { time: "15:00", battery_pct: 15 },
];