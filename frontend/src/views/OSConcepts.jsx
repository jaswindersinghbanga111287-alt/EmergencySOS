// Operating System concepts — every demo runs as real threads inside the C++ server
// (backend/src/os_concepts.hpp). The "Live" panel shows the OS concepts the server is
// using right now to serve the hospital app.
import { useEffect } from "react";
import { useApi } from "../api.js";
import ConceptLab, { AutoTable, formatValue } from "../components/ConceptLab.jsx";
import { PanelHeading, SectionIntro } from "../components/ui.jsx";

const LIVE_POLL_MS = 3000;

function Stat({ title, value, note, tone = "blue" }) {
  return (
    <div className="stat-card live-stat">
      <div className="stat-top">
        {title}
        <span className={`live-dot ${tone}`} />
      </div>
      <strong>{formatValue(value)}</strong>
      <small>{note}</small>
    </div>
  );
}

function LivePanel({ refreshKey }) {
  const { data, error, reload } = useApi("/os/live", refreshKey);
  useEffect(() => {
    const t = setInterval(reload, LIVE_POLL_MS);
    return () => clearInterval(t);
  }, [reload]);

  if (error) return <div className="notice">Live OS data unavailable: {error}</div>;
  if (!data) return null;
  const pool = data.connection_pool;
  const tx = data.transactions;
  const timer = data.timer_thread;
  const server = data.server;
  const usedPct = pool.size ? Math.round((pool.in_use / pool.size) * 100) : 0;

  return (
    <article className="panel">
      <PanelHeading
        title="Live inside the C++ server"
        subtitle={`Process ${data.process_id} · up ${formatValue(data.uptime_seconds)} s · refreshes every 3 s`}
      >
        <span className="pill available">● Live</span>
      </PanelHeading>
      <div className="stats-grid live-grid">
        <Stat title="HTTP worker threads" value={data.http_thread_pool_size} note={`${server.worker_threads_seen} have served requests · ${data.hardware_threads} CPU threads`} />
        <Stat title="Requests" value={server.total_requests} note={`${server.in_flight} in flight · peak ${server.peak_in_flight} at once`} tone="purple" />
        <Stat title="DB connection pool" value={`${pool.in_use} / ${pool.size} in use`} note={`${pool.threads_waiting} waiting · ${pool.had_to_wait} waited · ${pool.timeouts} timeouts`} tone="green" />
        <Stat title="Transactions" value={tx.committed} note={`committed · ${tx.rolled_back} rolled back · ${tx.deadlock_retries} deadlock retries`} tone="red" />
      </div>
      <div className="pool-meter" title={`${usedPct}% of DB connections in use`}>
        <span style={{ width: `${usedPct}%` }} />
      </div>
      <div className="two-col">
        <div className="result-section">
          <h4>Background timer thread</h4>
          <div className="kv-grid">
            <div className="kv"><small>Running</small><b>{formatValue(timer.running)}</b></div>
            <div className="kv"><small>Wakes every</small><b>{timer.interval_seconds} s</b></div>
            <div className="kv"><small>Wake-ups</small><b>{formatValue(timer.wakeups)}</b></div>
            <div className="kv"><small>Auto releases</small><b>{formatValue(timer.auto_releases)}</b></div>
            <div className="kv"><small>Last wake</small><b>{timer.seconds_since_last_wake < 0 ? "—" : `${timer.seconds_since_last_wake} s ago`}</b></div>
            <div className="kv"><small>Longest pool wait</small><b>{formatValue(pool.max_wait_ms)} ms</b></div>
          </div>
        </div>
        <div className="result-section">
          <h4>Busiest routes (readers-writers protected stats)</h4>
          <AutoTable rows={server.routes.slice(0, 6)} />
        </div>
      </div>
    </article>
  );
}

export default function OSConcepts({ refreshKey }) {
  return (
    <>
      <SectionIntro
        title="Operating System Concepts"
        subtitle="Each concept is C++ code in the backend. Press Run to execute it with real threads and see what happened."
      />
      <LivePanel refreshKey={refreshKey} />
      <ConceptLab catalogPath="/os/concepts" runPrefix="/os/run" refreshKey={refreshKey} />
      <div className="notice">
        <b>Where the code is:</b> backend/src/os_concepts.hpp (demos, semaphores, ticket lock, scheduling, Banker's),
        backend/src/db_pool.hpp (mutex + condition variable pool) and backend/src/main.cpp (timer thread, live routes).
      </div>
    </>
  );
}
