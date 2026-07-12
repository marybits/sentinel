import { useSentinelData } from "./utils/apiAdapters";
import NodeStatusCard from "./components/NodeStatusCard";
import AlertFeed from "./components/AlertFeed";
import TelemetryPanel from "./components/TelemetryPanel";
import ArcticMap from "./components/ArcticMap";
import DemoControls from "./components/DemoControls";

const CONNECTION_META = {
  connecting: { label: "CONNECTING...", dot: "bg-amber-500 animate-pulse", text: "text-amber-500" },
  live: { label: "LIVE", dot: "bg-emerald-500", text: "text-emerald-500" },
  reconnecting: { label: "RECONNECTING...", dot: "bg-amber-500 animate-pulse", text: "text-amber-500" },
};

function App() {
  const { nodes, alerts, connectionStatus } = useSentinelData();
  const meta = CONNECTION_META[connectionStatus];
  const hasData = nodes.length > 0;

  return (
    <div className="min-h-screen bg-slate-950 text-slate-300 p-6">
      <header className="mb-6 flex items-center justify-between flex-wrap gap-2">
        <div>
          <h1 className="font-mono text-lg text-white uppercase tracking-widest">
            Sentinel — Arctic Sensor Network
          </h1>
          <p className="text-slate-500 text-sm">Command Center / Node Status</p>
        </div>
        <div className="flex items-center gap-2">
          <span className={`h-2 w-2 rounded-full ${meta.dot}`} />
          <span className={`font-mono text-xs uppercase tracking-widest ${meta.text}`}>{meta.label}</span>
        </div>
      </header>

      <div className="mb-6">
        <DemoControls nodes={nodes} />
      </div>

      <div className="grid grid-cols-1 lg:grid-cols-[1fr_320px] gap-6 mb-6">
        {hasData ? (
          <section className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-4">
            {nodes.map((node) => (
              <NodeStatusCard key={node.node_id} node={node} />
            ))}
          </section>
        ) : (
          <section className="bg-slate-900 border border-slate-800 rounded-sm p-8 flex items-center justify-center">
            <p className="font-mono text-sm text-slate-500 uppercase tracking-wider">
              {connectionStatus === "connecting"
                ? "Connecting to Sentinel backend..."
                : "No node data yet"}
            </p>
          </section>
        )}

        <AlertFeed alerts={alerts} />
      </div>

      <div className="grid grid-cols-1 lg:grid-cols-2 gap-6">
        <TelemetryPanel nodes={nodes} />
        <ArcticMap nodes={nodes} />
      </div>
    </div>
  );
}

export default App;
