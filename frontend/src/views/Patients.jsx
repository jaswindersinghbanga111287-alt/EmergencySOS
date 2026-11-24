import { useState } from "react";
import { api, formToObject, post } from "../api.js";
import { Message, PanelHeading, SectionIntro } from "../components/ui.jsx";

export default function Patients({ notify }) {
  const [msg, setMsg] = useState(null);
  const [patient, setPatient] = useState(null);
  const [searchError, setSearchError] = useState("");

  async function register(e) {
    e.preventDefault();
    const form = e.currentTarget;
    try {
      const result = await post("/patients", formToObject(form));
      setMsg({ text: `Patient registered. Patient ID: ${result.patient_id}` });
      notify(`Patient #${result.patient_id} registered`);
      form.reset();
    } catch (err) {
      setMsg({ text: err.message, error: true });
    }
  }

  async function search(e) {
    e.preventDefault();
    const id = new FormData(e.currentTarget).get("patient_id");
    try {
      setPatient(await api(`/patients/${encodeURIComponent(id)}`));
      setSearchError("");
    } catch (err) {
      setPatient(null);
      setSearchError(err.message);
    }
  }

  return (
    <>
      <SectionIntro title="Patient management" subtitle="Register a patient or search for an existing patient." />
      <div className="two-col">
        <article className="panel">
          <PanelHeading title="Register patient" subtitle="Enter patient details" />
          <form className="form-grid" onSubmit={register}>
            <label>Full name<input name="name" required maxLength={100} placeholder="Patient full name" /></label>
            <label>Age<input name="age" type="number" min="0" max="130" placeholder="Age" /></label>
            <label>
              Gender
              <select name="gender" defaultValue="">
                <option value="">Select gender</option>
                <option>Female</option>
                <option>Male</option>
                <option>Other</option>
              </select>
            </label>
            <label>Contact number<input name="contact" maxLength={15} placeholder="Phone number" /></label>
            <label>Aadhaar / patient identifier<input name="aadhaar" maxLength={12} pattern="\d{12}" title="Exactly 12 digits" placeholder="Optional, 12 digits" /></label>
            <label className="full">Address<textarea name="address" rows={2} maxLength={255} placeholder="Address" /></label>
            <button className="btn btn-primary full" type="submit">Register patient</button>
          </form>
        </article>

        <article className="panel">
          <PanelHeading title="Find patient" subtitle="Search using patient ID" />
          <form className="search-row" onSubmit={search}>
            <input name="patient_id" type="number" min="1" required placeholder="Enter patient ID" />
            <button className="btn btn-primary">Search</button>
          </form>
          <div className="result-box">
            {searchError ? searchError : patient ? (
              <>
                <b>Patient ID:</b> {patient.patient_id}<br />
                <b>Name:</b> {patient.name}<br />
                <b>Age:</b> {patient.age ?? "—"}<br />
                <b>Gender:</b> {patient.gender ?? "—"}<br />
                <b>Contact:</b> {patient.contact ?? "—"}<br />
                <b>Address:</b> {patient.address ?? "—"}<br />
                <b>Registered:</b> {patient.registration_time}
              </>
            ) : (
              "Patient details will appear here."
            )}
          </div>
        </article>
      </div>
      <Message value={msg} />
    </>
  );
}
