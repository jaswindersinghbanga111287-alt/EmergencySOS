import { useState } from "react";
import { api, formToObject, post } from "../api.js";
import { Empty, Message, PanelHeading, Pill, SectionIntro } from "../components/ui.jsx";

export default function Records() {
  const [msg, setMsg] = useState(null);
  const [history, setHistory] = useState(null);
  const [historyError, setHistoryError] = useState("");

  async function save(e) {
    e.preventDefault();
    const form = e.currentTarget;
    try {
      await post("/records", formToObject(form));
      setMsg({ text: "Medical record saved." });
      form.reset();
    } catch (err) {
      setMsg({ text: err.message, error: true });
    }
  }

  async function lookup(e) {
    e.preventDefault();
    const id = new FormData(e.currentTarget).get("patient_id");
    try {
      setHistory(await api(`/records/${encodeURIComponent(id)}`));
      setHistoryError("");
    } catch (err) {
      setHistory(null);
      setHistoryError(err.message);
    }
  }

  return (
    <>
      <SectionIntro title="Medical records" subtitle="Add symptoms, diagnosis, treatment, and admission status." />
      <div className="two-col">
        <article className="panel">
          <PanelHeading title="Add medical record" subtitle="Records are linked to a patient ID." />
          <form className="form-grid" onSubmit={save}>
            <label>Patient ID<input name="patient_id" type="number" min="1" required /></label>
            <label>
              Admission status
              <select name="admission_status" defaultValue="Waiting">
                <option>Waiting</option>
                <option>Admitted</option>
                <option>Under Treatment</option>
                <option>Discharged</option>
              </select>
            </label>
            <label className="full">Symptoms<textarea name="symptoms" rows={2} required /></label>
            <label className="full">Diagnosis<textarea name="diagnosis" rows={2} /></label>
            <label className="full">Treatment<textarea name="treatment" rows={2} /></label>
            <button className="btn btn-primary full" type="submit">Save medical record</button>
          </form>
          <Message value={msg} />
        </article>

        <article className="panel">
          <PanelHeading title="Patient history" subtitle="All records for one patient, newest first" />
          <form className="search-row" onSubmit={lookup}>
            <input name="patient_id" type="number" min="1" required placeholder="Enter patient ID" />
            <button className="btn btn-primary">Show records</button>
          </form>
          {historyError && <Empty>{historyError}</Empty>}
          {history && history.length === 0 && <Empty>No records for this patient yet.</Empty>}
          {history && history.length > 0 && (
            <div className="records-list">
              {history.map((r) => (
                <div className="result-box" key={r.record_id}>
                  <b>{r.record_date}</b> <Pill value={r.admission_status} /><br />
                  <b>Symptoms:</b> {r.symptoms}<br />
                  <b>Diagnosis:</b> {r.diagnosis || "—"}<br />
                  <b>Treatment:</b> {r.treatment || "—"}
                </div>
              ))}
            </div>
          )}
        </article>
      </div>
    </>
  );
}
