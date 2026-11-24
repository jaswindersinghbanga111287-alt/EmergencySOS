import { useApi } from "../api.js";
import { DataTable, PanelHeading, Pill } from "../components/ui.jsx";

export default function Dashboard({ refreshKey, goTo }) {
  const { data, error, loading } = useApi("/dashboard", refreshKey);
  const stat = (v) => (v ?? "—");

  return (
    <>
      <div className="welcome">
        <div>
          <p className="eyebrow light">EMERGENCY CARE • RESOURCE CONTROL</p>
          <h2>Hospital overview</h2>
          <p>Monitor patients, emergency priority, and resource availability.</p>
        </div>
        <div className="welcome-icon">✚</div>
      </div>

      <div className="stats-grid">
        <StatCard label="Registered patients" icon="♙" color="blue" value={stat(data?.totalPatients)} note="Total patient records" />
        <StatCard label="Waiting emergencies" icon="⚑" color="red" value={stat(data?.waitingPatients)} note="Waiting or in treatment" />
        <StatCard label="Available beds" icon="▤" color="green" value={stat(data?.availableBeds)} note="Ready for admission" />
        <StatCard label="Available staff" icon="♧" color="purple" value={stat(data?.availableStaff)} note="Marked available" />
      </div>

      <div className="content-grid">
        <article className="panel">
          <PanelHeading title="Emergency queue" subtitle="Highest priority appears first">
            <button className="text-button" onClick={() => goTo("queue")}>View queue →</button>
          </PanelHeading>
          <DataTable
            loading={loading}
            error={error}
            rows={data?.queue}
            rowKey="queue_id"
            columns={[
              { label: "Patient ID", render: (p) => p.patient_id },
              { label: "Patient", render: (p) => p.name },
              { label: "Priority", render: (p) => <Pill value={p.priority_label} /> },
              { label: "Status", render: (p) => <Pill value={p.queue_status} /> },
            ]}
          />
        </article>

        <article className="panel quick-panel">
          <PanelHeading title="Quick actions" subtitle="Common hospital tasks" />
          <QuickAction icon="＋" color="blue" title="Patient management" text="Register or search for a patient" onClick={() => goTo("patients")} />
          <QuickAction icon="≡" color="red" title="Emergency queue" text="Review triage priority and status" onClick={() => goTo("queue")} />
          <QuickAction icon="▤" color="green" title="Allocate resources" text="Manage beds, staff, and equipment" onClick={() => goTo("resources")} />
        </article>
      </div>

      <div className="notice">
        <b>Connected architecture:</b> This React interface sends requests to the C++ REST API, which reads and writes the MySQL database.
      </div>
    </>
  );
}

function StatCard({ label, icon, color, value, note }) {
  return (
    <article className="stat-card">
      <div className="stat-top">
        <span>{label}</span>
        <span className={`stat-icon ${color}`}>{icon}</span>
      </div>
      <strong>{value}</strong>
      <small>{note}</small>
    </article>
  );
}

function QuickAction({ icon, color, title, text, onClick }) {
  return (
    <button className="quick-action" onClick={onClick}>
      <span className={`quick-icon ${color}`}>{icon}</span>
      <span>
        <b>{title}</b>
        <small>{text}</small>
      </span>
      <span>›</span>
    </button>
  );
}
