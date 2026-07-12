import { useState } from "react";
import { FLASK_BASE_URL } from "../config";

// Live-judging demo panel: fakes node_2..node_5 going dark and coming back,
// so the sync animation / offline badge / alert feed can all be triggered
// on command instead of waiting for sensor_simulator.py's own random
// offline rolls. node_1 is real Pi hardware — deliberately not selectable
// here, see backend/app.py's DEMO_KILLABLE_NODES.
const KILLABLE_NODES = ["node_2", "node_3", "node_4", "node_5"];

export default function DemoControls({ nodes = [] }) {
  const [selected, setSelected] = useState(KILLABLE_NODES[0]);
  const [pending, setPending] = useState(null); // "kill" | "revive" | null
  const [error, setError] = useState(null);

  const selectedNode = nodes.find((n) => n.node_id === selected);
  // Assume alive until GET /nodes proves otherwise — avoids a flash of
  // "OFFLINE" before the first poll resolves.
  const isOnline = selectedNode?.online ?? true;

  async function sendCommand(action) {
    setPending(action);
    setError(null);
    try {
      const res = await fetch(`${FLASK_BASE_URL}/demo/${action}-node/${selected}`, {
        method: "POST",
      });
      if (!res.ok) throw new Error(`Backend responded ${res.status}`);
    } catch (err) {
      console.error(`demo ${action} failed:`, err);
      setError(`${action.toUpperCase()} FAILED — CHECK BACKEND`);
    } finally {
      setPending(null);
    }
  }

  return (
    <div className="bg-slate-900 border border-rose-900/40 rounded-sm p-3 flex items-center gap-3 flex-wrap">
      <p className="font-mono text-[10px] text-slate-500 uppercase tracking-widest whitespace-nowrap">
        Demo Controls
      </p>

      <select
        value={selected}
        onChange={(e) => setSelected(e.target.value)}
        className="bg-slate-950 border border-slate-700 rounded-sm px-2 py-1 font-mono text-xs text-slate-300 uppercase tracking-wider focus:outline-none focus:border-slate-500"
      >
        {KILLABLE_NODES.map((id) => (
          <option key={id} value={id}>
            {id}
          </option>
        ))}
      </select>

      <button
        onClick={() => sendCommand("kill")}
        disabled={pending !== null}
        className="font-mono text-xs uppercase tracking-widest px-3 py-1 rounded-sm border border-rose-600 text-rose-500 hover:bg-rose-600 hover:text-white transition-colors disabled:opacity-40 disabled:cursor-not-allowed"
      >
        {pending === "kill" ? "Killing..." : "Kill Node"}
      </button>

      <button
        onClick={() => sendCommand("revive")}
        disabled={pending !== null}
        className="font-mono text-xs uppercase tracking-widest px-3 py-1 rounded-sm border border-emerald-600 text-emerald-500 hover:bg-emerald-600 hover:text-white transition-colors disabled:opacity-40 disabled:cursor-not-allowed"
      >
        {pending === "revive" ? "Reviving..." : "Revive Node"}
      </button>

      {selectedNode && (
        <span
          className={`font-mono text-[10px] uppercase tracking-widest ${
            isOnline ? "text-emerald-500" : "text-rose-500 animate-pulse"
          }`}
        >
          {isOnline ? "● Online" : "● Offline"}
        </span>
      )}

      {error && (
        <span className="font-mono text-[10px] text-rose-500 uppercase tracking-widest">{error}</span>
      )}
    </div>
  );
}
