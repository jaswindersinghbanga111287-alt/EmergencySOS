import { useEffect, useState } from "react";
import { api } from "./api.js";
import Dashboard from "./views/Dashboard.jsx";
import Patients from "./views/Patients.jsx";
import Queue from "./views/Queue.jsx";
import Resources from "./views/Resources.jsx";
import Records from "./views/Records.jsx";
import OSConcepts from "./views/OSConcepts.jsx";
import DBMSConcepts from "./views/DBMSConcepts.jsx";

const VIEWS = [
  { id: "dashboard", icon: "▦", label: "Dashboard", Component: Dashboard },
  { id: "patients", icon: "♙", label: "Patients", Component: Patients },
  { id: "queue", icon: "≡", label: "Emergency Queue", Component: Queue },
  { id: "resources", icon: "▤", label: "Hospital Resources", Component: Resources },
  { id: "records", icon: "▣", label: "Medical Records", Component: Records },
  { id: "os-concepts", icon: "⚙", label: "OS Concepts", Component: OSConcepts },
  { id: "dbms-concepts", icon: "⛁", label: "DBMS Concepts", Component: DBMSConcepts },
];

export default function App() {
  const [view, setView] = useState("dashboard");
  const [refreshKey, setRefreshKey] = useState(0);
  const [online, setOnline] = useState(null);
  const [toast, setToast] = useState("");

  // Check the C++ backend every 15 seconds and show it in the sidebar.
  useEffect(() => {
    const check = () => api("/health").then(() => setOnline(true)).catch(() => setOnline(false));
    check();
    const timer = setInterval(check, 15000);
    return () => clearInterval(timer);
  }, [refreshKey]);

  useEffect(() => {
    if (!toast) return;
    const timer = setTimeout(() => setToast(""), 3600);
    return () => clearTimeout(timer);
  }, [toast]);

  const current = VIEWS.find((v) => v.id === view) ?? VIEWS[0];
  const { Component } = current;
  const today = new Date().toLocaleDateString(undefined, { weekday: "short", month: "short", day: "numeric", year: "numeric" });

  return (
    <>
      <aside className="sidebar">
        <div className="brand">
          <span className="brand-mark">+</span>
          <div>
            <strong>EmergencySOS</strong>
            <small>Hospital Operations</small>
          </div>
        </div>
        <nav>
          {VIEWS.map((v) => (
            <button key={v.id} className={`nav-link ${v.id === view ? "active" : ""}`} onClick={() => setView(v.id)}>
              {v.icon} <span>{v.label}</span>
            </button>
          ))}
        </nav>
        <div className="sidebar-bottom">
          <span className={`status-dot ${online === false ? "offline" : ""}`} />
          C++ backend
          <small>{online === null ? "Checking…" : online ? "Connected to MySQL" : "Not reachable"}</small>
        </div>
      </aside>

      <main className="main">
        <header className="topbar">
          <div>
            <p className="eyebrow">HOSPITAL OPERATIONS</p>
            <h1>{current.label}</h1>
          </div>
          <div className="top-actions">
            <span id="today">{today}</span>
            <button
              className="btn btn-outline"
              onClick={() => {
                setRefreshKey((k) => k + 1);
                setToast("Refreshing data…");
              }}
            >
              ↻ Refresh
            </button>
          </div>
        </header>

        <section className="view active">
          <Component refreshKey={refreshKey} goTo={setView} notify={setToast} />
        </section>

        <footer>EmergencySOS · Hospital Resource &amp; Patient Management System</footer>
      </main>

      <div className={`toast ${toast ? "show" : ""}`} role="status">{toast}</div>
    </>
  );
}
