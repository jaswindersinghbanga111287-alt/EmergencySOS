export function Pill({ value }) {
  const raw = String(value ?? "Unknown");
  const cls = raw.toLowerCase().replace(/[^a-z ]/g, "").trim().replace(/\s+/g, "-");
  return <span className={`pill ${cls}`}>{raw}</span>;
}

export function Empty({ children }) {
  return <div className="empty-state">{children}</div>;
}

// columns: [{ label, render: (row) => node }]
export function DataTable({ columns, rows, loading, error, rowKey }) {
  if (error) return <Empty>{error}</Empty>;
  if (loading && !rows) return <Empty>Loading…</Empty>;
  if (!rows || rows.length === 0) return <Empty>No records found.</Empty>;
  return (
    <div className="table-wrap">
      <table>
        <thead>
          <tr>{columns.map((c) => <th key={c.label}>{c.label}</th>)}</tr>
        </thead>
        <tbody>
          {rows.map((row, i) => (
            <tr key={rowKey ? row[rowKey] : i}>
              {columns.map((c) => <td key={c.label}>{c.render(row)}</td>)}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

export function PanelHeading({ title, subtitle, children }) {
  return (
    <div className="panel-heading">
      <div>
        <h3>{title}</h3>
        {subtitle && <p>{subtitle}</p>}
      </div>
      {children}
    </div>
  );
}

export function Message({ value }) {
  if (!value?.text) return <div className="message" role="status" />;
  return (
    <div className={`message ${value.error ? "error" : "success"}`} role="status">
      {value.text}
    </div>
  );
}

export function SectionIntro({ title, subtitle, children }) {
  return (
    <div className="section-intro">
      <div>
        <h2>{title}</h2>
        <p>{subtitle}</p>
      </div>
      {children}
    </div>
  );
}
