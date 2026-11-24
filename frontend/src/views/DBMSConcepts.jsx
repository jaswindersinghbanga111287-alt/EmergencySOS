// Database concepts — every demo runs real SQL against MySQL from the C++ server
// (backend/src/dbms_concepts.hpp). Demos that change data always roll back.
import ConceptLab from "../components/ConceptLab.jsx";
import { PanelHeading, SectionIntro } from "../components/ui.jsx";

function InstalledObjects({ installed }) {
  if (!installed?.length) return null;
  const failed = installed.filter((o) => !o.ok);
  return (
    <article className="panel">
      <PanelHeading
        title="Database objects created by the C++ server at start-up"
        subtitle={`${installed.length - failed.length} of ${installed.length} ready — views, stored routines, triggers, audit table and indexes`}
      />
      <div className="chip-row static">
        {installed.map((o) => (
          <span key={o.name} className={`object-chip ${o.ok ? "ok" : "failed"}`} title={o.error || "Ready"}>
            <small>{o.kind}</small> {o.name}
          </span>
        ))}
      </div>
      {failed.length > 0 && (
        <div className="notice">
          Some objects could not be created (hover for the MySQL message). Usually the MySQL user needs the CREATE ROUTINE / TRIGGER
          privilege, or <code>log_bin_trust_function_creators=1</code> when binary logging is on.
        </div>
      )}
    </article>
  );
}

export default function DBMSConcepts({ refreshKey }) {
  return (
    <>
      <SectionIntro
        title="Database (DBMS) Concepts"
        subtitle="Each concept is C++ code that runs real SQL on the hospital database. Demos that change data are rolled back."
      />
      <ConceptLab catalogPath="/dbms/concepts" runPrefix="/dbms/run" refreshKey={refreshKey}>
        {(data) => <InstalledObjects installed={data?.installed} />}
      </ConceptLab>
      <div className="notice">
        <b>Where the code is:</b> backend/src/dbms_concepts.hpp (objects + demos), backend/src/db_pool.hpp (transactions, deadlock
        retry) and backend/src/main.cpp (row locks in allocations, joins in the queue and resources routes).
      </div>
    </>
  );
}
