import { useEffect, useState } from "react";
import { mockSensorData } from "./data/mockSensorData";
import { mockAlertData } from "./data/mockAlertData";
import { normalizeNode, normalizeAlert } from "./utils/apiAdapters";
import NodeStatusCard from "./components/NodeStatusCard";
import AlertFeed from "./components/AlertFeed";
import TelemetryPanel from "./components/TelemetryPanel";
import ArcticMap from "./components/ArcticMap";
import { FLASK_BASE_URL, POLL_INTERVAL_MS } from "./config";

function App() {
  const [nodes, setNodes] = useState(mockSensorData);
  const [alerts, setAlerts] = useState(mockAlertData.map(normalizeAlert));

  useEffect(() => {
    let cancelled = false;
    let timeoutId;

    async function poll() {
      try {
        const [nodesRes, alertsRes] = await Promise.all([
          fetch(`${FLASK_BASE_URL}/nodes`),
          fetch(`${FLASK_BASE_URL}/alerts`),
        ]);

        if (!nodesRes.ok || !alertsRes.ok) {
          throw new Error(`Backend responded ${nodesRes.status}/${alertsRes.status}`);
        }

        const [rawNodes, rawAlerts] = await Promise.all([
          nodesRes.json(),
          alertsRes.json(),
        ]);

        if (!cancelled) {
          setNodes(rawNodes.map(normalizeNode));
          setAlerts(rawAlerts.map(normalizeAlert));
        }
      } catch (err) {
        // Backend down or unreachable — keep showing last-known-good data
        // instead of crashing the dashboard.
        console.error("Poll failed, keeping last known state:", err);
      } finally {
        if (!cancelled) {
          timeoutId = setTimeout(poll, POLL_INTERVAL_MS);
        }
      }
    }

    poll();

    return () => {
      cancelled = true;
      clearTimeout(timeoutId);
    };
  }, []);

  return (
    <div className="min-h-screen bg-slate-950 text-slate-300 p-6">
      <header className="mb-6">
        <h1 className="font-mono text-lg text-white uppercase tracking-widest">
          Sentinel — Arctic Sensor Network
        </h1>
        <p className="text-slate-500 text-sm">Command Center / Node Status</p>
      </header>

      <div className="grid grid-cols-1 lg:grid-cols-[1fr_320px] gap-6 mb-6">
        <section className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-4">
          {nodes.map((node) => (
            <NodeStatusCard key={node.node_id} node={node} />
          ))}
        </section>

        <AlertFeed alerts={alerts} />
      </div>

      <div className="grid grid-cols-1 lg:grid-cols-2 gap-6">
        <TelemetryPanel />
        <ArcticMap nodes={nodes} />
      </div>
    </div>
  );
}

export default App;