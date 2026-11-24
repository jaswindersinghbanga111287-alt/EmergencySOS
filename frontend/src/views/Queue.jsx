import { useState } from "react";
import { formToObject, post, useApi } from "../api.js";
import { DataTable, Message, PanelHeading, Pill, SectionIntro } from "../components/ui.jsx";

export default function Queue({ refreshKey, notify }) {
  const { data, error, loading, reload } = useApi("/queue", refreshKey);
  const [msg, setMsg] = useState(null);

  async function addToQueue(e) {
    e.preventDefault();
    const form = e.currentTarget;
    try {
      await post("/queue", formToObject(form));
      setMsg({ text: "Patient added to emergency queue." });
      form.reset();
      reload();
    } catch (err) {
      setMsg({ text: err.message, error: true });
    }
  }

  async function setStatus(row, status) {
    try {
      await post(`/queue/${row.queue_id}/status`, { status });
      notify(`${row.name}: ${status}`);
      reload();
    } catch (err) {
      setMsg({ text: err.message, error: true });
    }
  }

  return (
    <>
      <SectionIntro title="Emergency queue" subtitle="Prioritize patients according to triage severity.">
        <button className="btn btn-primary" onClick={reload}>↻ Load queue</button>
      </SectionIntro>

      <div className="panel">
        <DataTable
          loading={loading}
          error={error}
          rows={data}
          rowKey="queue_id"
          columns={[
            { label: "Queue ID", render: (r) => r.queue_id },
            { label: "Patient ID", render: (r) => r.patient_id },
            { label: "Name", render: (r) => r.name },
            { label: "Priority", render: (r) => <Pill value={r.priority_label} /> },
            { label: "Arrival", render: (r) => r.arrival_time },
            { label: "Status", render: (r) => <Pill value={r.queue_status} /> },
            {
              label: "Actions",
              render: (r) => (
                <div className="row-actions">
                  {r.queue_status === "Waiting" && (
                    <button className="btn btn-outline btn-sm" onClick={() => setStatus(r, "In Treatment")}>Start treatment</button>
                  )}
                  <button className="btn btn-outline btn-sm" onClick={() => setStatus(r, "Completed")}>Complete</button>
                </div>
              ),
            },
          ]}
        />
      </div>

      <div className="panel form-panel">
        <PanelHeading title="Add patient to queue" subtitle="Priority 1 is critical; priority 4 is low." />
        <form className="inline-form" onSubmit={addToQueue}>
          <label>Patient ID<input name="patient_id" type="number" min="1" required /></label>
          <label>
            Priority
            <select name="priority" required defaultValue="1">
              <option value="1">1 — Critical</option>
              <option value="2">2 — High</option>
              <option value="3">3 — Medium</option>
              <option value="4">4 — Low</option>
            </select>
          </label>
          <button className="btn btn-primary">Add to queue</button>
        </form>
      </div>
      <Message value={msg} />
    </>
  );
}
