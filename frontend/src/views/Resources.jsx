import { useEffect, useMemo, useState } from "react";
import { formToObject, post, useApi } from "../api.js";
import { DataTable, Message, PanelHeading, Pill, SectionIntro } from "../components/ui.jsx";

// Reload while this page is open so auto-released patients disappear on their own.
const POLL_MS = 15000;

function formatMinutes(min) {
  if (min < 60) return `${min} min`;
  const h = Math.floor(min / 60);
  const m = min % 60;
  return m ? `${h}h ${m}m` : `${h}h`;
}

function formatRemaining(ms) {
  const total = Math.max(0, Math.ceil(ms / 1000));
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = String(total % 60).padStart(2, "0");
  return h ? `${h}h ${String(m).padStart(2, "0")}m ${s}s` : `${m}m ${s}s`;
}

// Live countdown. `seconds` comes from the server, `loadedAt` is when we received it.
function Countdown({ seconds, loadedAt, now }) {
  if (seconds === null || seconds === undefined) return <span className="countdown none">No timer</span>;
  const left = Number(seconds) * 1000 - (now - loadedAt);
  if (left <= 0) return <span className="countdown due">Releasing…</span>;
  return <span className={`countdown ${left <= 5 * 60 * 1000 ? "soon" : ""}`}>{formatRemaining(left)}</span>;
}

export default function Resources({ refreshKey, notify }) {
  const { data, error, loading, reload } = useApi("/resources", refreshKey);
  const [msg, setMsg] = useState(null);
  const [releaseMsg, setReleaseMsg] = useState(null);
  const [now, setNow] = useState(() => Date.now());
  const loadedAt = useMemo(() => Date.now(), [data]);

  useEffect(() => {
    const tick = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(tick);
  }, []);
  useEffect(() => {
    const poll = setInterval(reload, POLL_MS);
    return () => clearInterval(poll);
  }, [reload]);

  const policy = data?.release_policy ?? [];

  async function allocate(e) {
    e.preventDefault();
    const form = e.currentTarget;
    const d = formToObject(form);
    if (!Number(d.bed_id) && !Number(d.staff_id) && !Number(d.equipment_id)) {
      setMsg({ text: "Select at least one resource.", error: true });
      return;
    }
    try {
      const result = await post("/allocations", d);
      setMsg({ text: `Allocation #${result.allocation_id}: ${result.message}` });
      form.reset();
      reload();
    } catch (err) {
      setMsg({ text: err.message, error: true });
    }
  }

  async function release(id) {
    try {
      await post(`/allocations/${id}/release`);
      notify(`Allocation #${id} released`);
      reload();
    } catch (err) {
      setMsg({ text: err.message, error: true });
    }
  }

  async function releasePatient(e) {
    e.preventDefault();
    const form = e.currentTarget;
    const id = Number(new FormData(form).get("patient_id"));
    if (!id) return;
    if (!window.confirm(`Release patient #${id}? All their beds, staff and equipment will be returned and their queue entry closed.`)) return;
    try {
      const result = await post(`/patients/${id}/release`);
      setReleaseMsg({ text: result.message });
      notify(`Patient #${id} released`);
      form.reset();
      reload();
    } catch (err) {
      setReleaseMsg({ text: err.message, error: true });
    }
  }

  const common = { loading, error };

  return (
    <>
      <SectionIntro title="Hospital resources" subtitle="View bed, staff, and equipment availability.">
        <button className="btn btn-primary" onClick={reload}>↻ Load resources</button>
      </SectionIntro>

      <div className="resource-grid">
        <article className="panel">
          <PanelHeading title="Beds" subtitle="Ward and occupancy status" />
          <DataTable {...common} rows={data?.beds} rowKey="bed_id" columns={[
            { label: "ID", render: (x) => x.bed_id },
            { label: "Bed", render: (x) => x.bed_number },
            { label: "Ward", render: (x) => x.ward },
            { label: "Type", render: (x) => x.bed_type },
            { label: "Status", render: (x) => <Pill value={x.bed_status} /> },
          ]} />
        </article>
        <article className="panel">
          <PanelHeading title="Staff" subtitle="Doctors, nurses, technicians" />
          <DataTable {...common} rows={data?.staff} rowKey="staff_id" columns={[
            { label: "ID", render: (x) => x.staff_id },
            { label: "Staff", render: (x) => x.staff_name },
            { label: "Role", render: (x) => x.role },
            { label: "Status", render: (x) => <Pill value={x.staff_status} /> },
          ]} />
        </article>
        <article className="panel">
          <PanelHeading title="Equipment" subtitle="Available stock" />
          <DataTable {...common} rows={data?.equipment} rowKey="equipment_id" columns={[
            { label: "ID", render: (x) => x.equipment_id },
            { label: "Equipment", render: (x) => x.equipment_name },
            { label: "Available", render: (x) => x.available_quantity },
            { label: "Total", render: (x) => x.total_quantity },
          ]} />
        </article>
      </div>

      <article className="panel form-panel">
        <PanelHeading title="Allocate resources" subtitle="Use 0 for a resource you do not want to assign. Resources are released automatically when the severity timer runs out." />
        {policy.length > 0 && (
          <div className="policy-row">
            <span>Auto-release timers:</span>
            {policy.map((p) => (
              <span key={p.priority} className="policy-item">
                <Pill value={p.label} /> {formatMinutes(p.minutes)}
              </span>
            ))}
          </div>
        )}
        <form className="form-grid allocation-grid" onSubmit={allocate}>
          <label>Patient ID<input name="patient_id" type="number" min="1" required /></label>
          <label>
            Severity (timer)
            <select name="priority" defaultValue="">
              <option value="">From queue priority</option>
              {(policy.length ? policy : [1, 2, 3, 4].map((p) => ({ priority: p, label: ["", "Critical", "High", "Medium", "Low"][p] }))).map((p) => (
                <option key={p.priority} value={p.priority}>
                  {p.priority} – {p.label}{p.minutes ? ` (${formatMinutes(p.minutes)})` : ""}
                </option>
              ))}
            </select>
          </label>
          <label>Bed ID<input name="bed_id" type="number" min="0" defaultValue="0" required /></label>
          <label>Staff ID<input name="staff_id" type="number" min="0" defaultValue="0" required /></label>
          <label>Equipment ID<input name="equipment_id" type="number" min="0" defaultValue="0" required /></label>
          <button className="btn btn-primary full" type="submit">Allocate resources</button>
        </form>
      </article>
      <Message value={msg} />

      <article className="panel form-panel">
        <PanelHeading title="Release patient" subtitle="Release a patient early by patient ID: returns all their resources and closes their queue entry." />
        <form className="search-row" onSubmit={releasePatient}>
          <input name="patient_id" type="number" min="1" required placeholder="Enter patient ID" aria-label="Patient ID to release" />
          <button className="btn btn-danger" type="submit">Release patient</button>
        </form>
      </article>
      <Message value={releaseMsg} />

      <article className="panel">
        <PanelHeading title="Active allocations" subtitle="Each allocation is released automatically when its timer reaches zero" />
        <DataTable {...common} rows={data?.allocations} rowKey="allocation_id" columns={[
          { label: "ID", render: (a) => a.allocation_id },
          { label: "Patient", render: (a) => `#${a.patient_id} ${a.patient_name}` },
          { label: "Severity", render: (a) => (a.priority_label ? <Pill value={a.priority_label} /> : "—") },
          { label: "Bed", render: (a) => a.bed_number ?? "—" },
          { label: "Staff", render: (a) => a.staff_name ?? "—" },
          { label: "Equipment", render: (a) => a.equipment_name ?? "—" },
          { label: "Since", render: (a) => a.allocation_time },
          { label: "Auto-release in", render: (a) => <Countdown seconds={a.seconds_remaining} loadedAt={loadedAt} now={now} /> },
          { label: "", render: (a) => <button className="btn btn-outline btn-sm" onClick={() => release(a.allocation_id)}>Release</button> },
        ]} />
      </article>
    </>
  );
}
