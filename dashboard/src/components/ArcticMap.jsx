import { MapContainer, TileLayer, CircleMarker, Popup } from "react-leaflet";
import { getNodeStatus, STATUS_HEX } from "../utils/nodeStatus";

const ARCTIC_CENTER = [64.0, -100.0];
const DEFAULT_ZOOM = 3;

export default function ArcticMap({ nodes }) {
  return (
    <div className="bg-slate-900 border border-slate-800 rounded-sm p-4">
      <p className="font-mono text-[10px] text-slate-500 uppercase tracking-wider mb-2">
        Arctic Node Map
      </p>
      <div className="h-[360px] overflow-hidden rounded-sm">
        <MapContainer
          center={ARCTIC_CENTER}
          zoom={DEFAULT_ZOOM}
          scrollWheelZoom={false}
          className="h-full w-full"
        >
          <TileLayer
            url="https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png"
            attribution='&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors &copy; <a href="https://carto.com/attributions">CARTO</a>'
          />
          {nodes
            .filter((node) => typeof node.lat === "number" && typeof node.lon === "number")
            .map((node) => {
              const status = getNodeStatus(node);
              const color = STATUS_HEX[status];
              return (
                <CircleMarker
                  key={node.node_id}
                  center={[node.lat, node.lon]}
                  radius={8}
                  pathOptions={{ color, fillColor: color, fillOpacity: 0.8, weight: 2 }}
                >
                  <Popup>
                    <span className="font-mono text-xs">
                      {node.node_id} — {node.location}
                    </span>
                  </Popup>
                </CircleMarker>
              );
            })}
        </MapContainer>
      </div>
    </div>
  );
}