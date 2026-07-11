import { mockSensorData } from "./data/mockSensorData";
import { mockAlertData } from "./data/mockAlertData";
import NodeStatusCard from "./components/NodeStatusCard";
import AlertFeed from "./components/AlertFeed";

function App() {
  return (
    <div className="min-h-screen bg-slate-950 text-slate-300 p-6">
      <header className="mb-6">
        <h1 className="font-mono text-lg text-white uppercase tracking-widest">
          Sentinel — Arctic Sensor Network
        </h1>
        <p className="text-slate-500 text-sm">Command Center / Node Status</p>
      </header>

      <div className="grid grid-cols-1 lg:grid-cols-[1fr_320px] gap-6">
        <section className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-4">
          {mockSensorData.map((node) => (
            <NodeStatusCard key={node.node_id} node={node} />
          ))}
        </section>

        <AlertFeed alerts={mockAlertData} />
      </div>
    </div>
  );
}

export default App;