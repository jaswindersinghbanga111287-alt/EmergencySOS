// Shared runner for the OS Concepts and DBMS Concepts pages.
// The catalog and every result come from the C++ backend; this file only displays them.
import { useMemo, useState } from "react";
import { post, useApi } from "../api.js";
import { Empty, Pill } from "./ui.jsx";

const label = (key) => key.replace(/_/g, " ").replace(/^\w/, (c) => c.toUpperCase());

export function formatValue(v) {
  if (v === null || v === undefined || v === "") return "—";
  if (typeof v === "boolean") return v ? "Yes" : "No";
  if (typeof v === "number") {
    if (Number.isInteger(v)) return Math.abs(v) < 10000 ? String(v) : v.toLocaleString(); // keeps error codes like 1213 readable
    return v.toLocaleString(undefined, { maximumFractionDigits: 2 });
  }
  if (Array.isArray(v)) return v.length ? v.map(formatValue).join("  →  ") : "—";
  if (typeof v === "object") return JSON.stringify(v);
  return String(v);
}

export function AutoTable({ rows }) {
  if (!rows || rows.length === 0) return <Empty>No rows returned.</Empty>;
  const columns = [...new Set(rows.flatMap((r) => Object.keys(r)))];
  return (
    <div className="table-wrap">
      <table>
        <thead>
          <tr>{columns.map((c) => <th key={c}>{label(c)}</th>)}</tr>
        </thead>
        <tbody>
          {rows.map((row, i) => (
            <tr key={i}>{columns.map((c) => <td key={c}>{formatValue(row[c])}</td>)}</tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

function Summary({ summary }) {
  const entries = Object.entries(summary ?? {});
  if (!entries.length) return null;
  return (
    <div className="kv-grid">
      {entries.map(([k, v]) => (
        <div className="kv" key={k}>
          <small>{label(k)}</small>
          <b>{formatValue(v)}</b>
        </div>
      ))}
    </div>
  );
}

// `order` = process ids in a fixed order, so each process keeps its colour in every chart.
function Gantt({ slices, order }) {
  const end = Math.max(1, ...slices.map((s) => s.end));
  const ids = order ?? [...new Set(slices.filter((s) => s.id !== "idle").map((s) => s.id))];
  return (
    <div className="gantt">
      <div className="gantt-bar">
        {slices.map((s, i) => (
          <div
            key={i}
            className={`gantt-slice ${s.id === "idle" ? "g-idle" : `g${ids.indexOf(s.id) % 8}`}`}
            style={{ flexGrow: s.end - s.start }}
            title={`${s.name}: ${s.start} → ${s.end}`}
          >
            <span>{s.id === "idle" ? "idle" : s.id}</span>
          </div>
        ))}
      </div>
      <div className="gantt-axis">
        <span>0</span>
        <span>{end}</span>
      </div>
    </div>
  );
}

function Schedules({ schedules }) {
  return (
    <div className="schedule-list">
      {schedules.map((s) => (
        <div className="schedule" key={s.name}>
          <div className="schedule-head">
            <div>
              <b>{s.name}</b>
              <small>{s.how}</small>
            </div>
            <div className="schedule-stats">
              <span>Avg wait <b>{formatValue(s.avg_waiting)}</b></span>
              <span>Avg turnaround <b>{formatValue(s.avg_turnaround)}</b></span>
              <span>Lowest-priority wait <b>{formatValue(s.lowest_priority_wait)}</b></span>
            </div>
          </div>
          <Gantt slices={s.gantt} order={s.rows.map((r) => r.process)} />
          <details>
            <summary>Per-process times</summary>
            <AutoTable rows={s.rows} />
          </details>
        </div>
      ))}
    </div>
  );
}

function Log({ events }) {
  if (!events?.length) return null;
  return (
    <div className="result-section">
      <h4>Execution log ({events.length} events, from the C++ threads)</h4>
      <ol className="event-log">
        {events.map((e, i) => (
          <li key={i}>
            <span className="event-time">{e.t_ms} ms</span>
            <span className="event-thread">{e.thread}</span>
            <span>{e.event}</span>
          </li>
        ))}
      </ol>
    </div>
  );
}

function Statements({ statements }) {
  if (!statements?.length) return null;
  return (
    <details className="result-section">
      <summary>SQL executed by the C++ server ({statements.length} statements)</summary>
      <ol className="sql-list">
        {statements.map((s, i) => (
          <li key={i}>
            <span className="event-thread">{s.connection === "A" || s.connection === "B" ? `Conn ${s.connection}` : s.connection}</span>
            <code>{s.sql}</code>
          </li>
        ))}
      </ol>
    </details>
  );
}

export function ConceptResult({ result }) {
  return (
    <div className="concept-result">
      <Summary summary={result.summary} />
      {result.schedules && <Schedules schedules={result.schedules} />}
      {(result.tables ?? []).map((t, i) => (
        <div className="result-section" key={i}>
          <h4>{t.title}</h4>
          {t.sql && <pre className="code-block">{t.sql}</pre>}
          <AutoTable rows={t.rows} />
        </div>
      ))}
      <Statements statements={result.statements} />
      <Log events={result.log} />
      <p className="ran-in">
        Ran in the C++ backend{result.process_id ? ` (process ${result.process_id})` : ""} in {formatValue(result.duration_ms)} ms
      </p>
    </div>
  );
}

function ParamField({ p, value, onChange }) {
  if (p.type === "select")
    return (
      <label>
        {p.label}
        <select value={value} onChange={(e) => onChange(e.target.value)}>
          {p.options.map((o) => <option key={o.value} value={o.value}>{o.label}</option>)}
        </select>
      </label>
    );
  return (
    <label>
      {p.label}
      <input
        type={p.type === "number" ? "number" : "text"}
        min={p.min}
        max={p.max}
        value={value}
        placeholder={p.type === "text" ? "optional" : undefined}
        onChange={(e) => onChange(e.target.value)}
      />
    </label>
  );
}

function ConceptCard({ concept, index, runPrefix }) {
  const [params, setParams] = useState(() => Object.fromEntries(concept.params.map((p) => [p.key, p.default ?? ""])));
  const [state, setState] = useState({ running: false, result: null, error: "" });

  async function run() {
    setState((s) => ({ ...s, running: true, error: "" }));
    try {
      const body = Object.fromEntries(Object.entries(params).filter(([, v]) => v !== ""));
      const result = await post(`${runPrefix}/${concept.id}`, body);
      setState({ running: false, result, error: "" });
    } catch (e) {
      setState({ running: false, result: null, error: e.message });
    }
  }

  return (
    <article className={`panel concept-card ${state.result ? "expanded" : ""}`}>
      <div className="concept-head">
        <span className={`stat-icon ${index % 2 ? "purple" : "blue"}`}>{index + 1}</span>
        <div>
          <h3>{concept.title}</h3>
          <Pill value={concept.category} />
        </div>
      </div>
      <p className="concept-desc">{concept.description}</p>
      <div className="where-used">
        <b>Used in the app:</b> {concept.where_used}
      </div>
      <div className="concept-actions">
        {concept.params.map((p) => (
          <ParamField key={p.key} p={p} value={params[p.key]} onChange={(v) => setParams((s) => ({ ...s, [p.key]: v }))} />
        ))}
        <button className="btn btn-primary" onClick={run} disabled={state.running}>
          {state.running ? "Running in C++…" : state.result ? "↻ Run again" : "▶ Run in C++"}
        </button>
      </div>
      {state.error && <div className="message error">{state.error}</div>}
      {state.result && <ConceptResult result={state.result} />}
    </article>
  );
}

export default function ConceptLab({ catalogPath, runPrefix, refreshKey, children }) {
  const { data, error, loading } = useApi(catalogPath, refreshKey);
  const [category, setCategory] = useState("All");
  const concepts = data?.concepts ?? [];
  const categories = useMemo(() => ["All", ...new Set(concepts.map((c) => c.category))], [concepts]);
  const visible = concepts.filter((c) => category === "All" || c.category === category);

  return (
    <>
      {children?.(data)}
      {error && <Empty>{error}</Empty>}
      {loading && !data && <Empty>Loading concepts from the C++ backend…</Empty>}
      {concepts.length > 0 && (
        <div className="chip-row">
          {categories.map((c) => (
            <button key={c} className={`chip ${c === category ? "active" : ""}`} onClick={() => setCategory(c)}>
              {c}
            </button>
          ))}
        </div>
      )}
      <div className="concept-grid">
        {visible.map((c) => (
          <ConceptCard key={c.id} concept={c} index={concepts.indexOf(c)} runPrefix={runPrefix} />
        ))}
      </div>
    </>
  );
}
