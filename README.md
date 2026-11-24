# EmergencySOS — React frontend + pure C++ backend + MySQL

```
EmergencySOS/
├── backend/          C++17 REST API (no Node.js) — talks to MySQL
│   ├── src/main.cpp          all /api routes, timer thread, live OS/DBMS routes
│   ├── src/os_concepts.hpp   ALL OS concepts (threads, mutex, semaphores, deadlock, Banker's, scheduling ...)
│   ├── src/dbms_concepts.hpp ALL DBMS concepts (views, triggers, procedures, transactions, locking ...)
│   ├── src/db_pool.hpp       thread-safe connection pool + transactions with deadlock retry
│   ├── src/db_helpers.hpp    shared MySQL helpers (rowsToJson, query, execute)
│   ├── src/config.hpp        reads backend/.env
│   ├── third_party/        cpp-httplib + nlohmann/json (header-only, already included)
│   └── CMakeLists.txt
├── frontend/         React app (Vite + npm)
├── database/         project_schema.sql (tables + sample beds, staff, equipment)
├── os_concepts/      console runner that calls the same backend/src/os_concepts.hpp code
└── cpp_console/      your original C++ console modules (unchanged)
```

How it fits together:

```
Browser (React, port 5173) --/api--> C++ server (port 8080) --> MySQL (port 3306)
```

---

## 1. What you need installed

| Tool | Why | Get it |
|---|---|---|
| MySQL Server 8.x | the database | https://dev.mysql.com/downloads/mysql/ |
| MySQL Connector/C++ 8.0.33+ or 9.x | lets C++ talk to MySQL | https://dev.mysql.com/downloads/connector/cpp/ |
| A C++17 compiler + CMake | builds the backend | Windows: Visual Studio 2022 ("Desktop development with C++" includes CMake). Linux: `sudo apt install build-essential cmake`. macOS: `xcode-select --install` and `brew install cmake` |
| Node.js 18+ | runs the React frontend | https://nodejs.org |

> **Windows note:** the official Connector/C++ download is built for Visual Studio (MSVC).
> It does **not** work with MinGW / Code::Blocks / Dev-C++. Use the "x64 Native Tools Command Prompt for VS 2022".
>
> **Linux:** `sudo apt install libmysqlcppconn-dev` &nbsp;·&nbsp; **macOS:** `brew install mysql-connector-c++`

---

## 2. Create the database (one time)

```bash
mysql -u root -p < database/project_schema.sql
```

---

## 3. Build and start the C++ backend

```bash
cd backend
cp .env.example .env          # Windows: copy .env.example .env
```

Open `backend/.env` and set `DB_PASSWORD` to your MySQL password.

**Linux / macOS**
```bash
cmake -S . -B build
cmake --build build
./build/emergency_sos_server
```

Quick alternative without CMake (Linux):
```bash
g++ -std=c++17 -O2 -Ithird_party -o emergency_sos_server src/main.cpp -lmysqlcppconn -pthread
./emergency_sos_server
```

**Windows (x64 Native Tools Command Prompt for VS 2022)**
```bat
cmake -S . -B build -DMYSQL_CONCPP_DIR="C:/Program Files/MySQL/MySQL Connector C++ 9.1"
cmake --build build --config Release
build\Release\emergency_sos_server.exe
```
(Change the folder to match the Connector/C++ version you installed.)

You should see:
```
EmergencySOS C++ API running at http://localhost:8080
```
Check it in a browser: http://localhost:8080/api/health

Always start the server **from the `backend` folder** so it finds `.env`.

---

## 4. Start the React frontend (second terminal)

```bash
cd frontend
npm install
npm run dev
```

The app opens at **http://localhost:5173**. The sidebar shows a green dot when the C++ backend is connected.

### Optional: one address only
Run `npm run build` in `frontend/`, then restart the C++ server. It will serve the finished app itself at http://localhost:8080.

---

## API (served by C++)

| Method | Path | What it does |
|---|---|---|
| GET  | /api/health | backend + database check |
| GET  | /api/dashboard | counts + top of the queue |
| GET  | /api/queue | active emergency queue (priority, then arrival) |
| POST | /api/queue | `{patient_id, priority 1-4}` |
| POST | /api/queue/:id/status | `{status: "In Treatment" \| "Completed" \| "Cancelled"}` |
| GET  | /api/resources | beds, staff, equipment, active allocations |
| POST | /api/patients | register a patient |
| GET  | /api/patients/:id | find a patient |
| POST | /api/records | add a medical record |
| GET  | /api/records/:patient_id | a patient's record history |
| POST | /api/allocations | `{patient_id, bed_id, staff_id, equipment_id, priority?}` (0 = none). Starts a severity release timer |
| POST | /api/allocations/:id/release | return the resources of one allocation |
| POST | /api/patients/:id/release | release a patient by patient ID: returns all their resources and closes their queue entry |
| GET  | /api/os/live | live OS data from the server: worker threads, requests, connection pool, transactions, timer thread |
| GET  | /api/os/concepts | list of OS concepts + their input fields |
| POST | /api/os/run/:id | runs one OS concept with real C++ threads (body = its parameters) |
| GET  | /api/dbms/concepts | list of DBMS concepts + the database objects created at start-up |
| POST | /api/dbms/run/:id | runs one DBMS concept as real SQL on MySQL (data-changing demos are rolled back) |

## Severity-based auto-release timers

Every allocation gets a release timer based on the patient's severity. When the timer runs out,
a background thread in the C++ server releases the allocation automatically (beds, staff and
equipment go back to Available, and the patient's queue entry is marked Completed once they
hold nothing else).

| Severity | Default timer | `.env` setting |
|---|---|---|
| 1 Critical | 240 min | `RELEASE_MINUTES_CRITICAL` |
| 2 High | 120 min | `RELEASE_MINUTES_HIGH` |
| 3 Medium | 60 min | `RELEASE_MINUTES_MEDIUM` |
| 4 Low | 30 min | `RELEASE_MINUTES_LOW` |

- Severity comes from the patient's emergency-queue priority. In the Allocate form you can also pick it
  yourself (required if the patient is not in the queue).
- The server checks for expired timers every `AUTO_RELEASE_CHECK_SECONDS` (default 15).
- The **Hospital Resources** page shows a live countdown for each allocation and a **Release patient**
  box to release a patient early by patient ID.
- Tip for demos: set `RELEASE_MINUTES_LOW=1` in `backend/.env` to watch a release happen within a minute.
- Existing databases are upgraded automatically: on start-up the server adds the new columns
  (`priority`, `release_due_at`, `release_type`) to `resource_allocations` if they are missing.
  Allocations made before the upgrade have no timer and are released manually.

## OS concepts — all in `backend/src/os_concepts.hpp`, connected to the **OS Concepts** page

Every concept is C++ code inside the server. Pressing **Run in C++** calls `POST /api/os/run/:id`,
the server runs it with real `std::thread`s and sends back the result and a step-by-step log.
The **Live** panel (`GET /api/os/live`, refreshed every 3 s) shows the concepts the server is
using right now to run the hospital app.

| Concept | Demo id | Used by the hospital app |
|---|---|---|
| Threads / thread pool | live panel | cpp-httplib worker threads serve every request (`ServerMetrics`) |
| Race condition, critical section, atomics | `race_condition` | all live counters are `std::atomic`; pool free list behind a `std::mutex` |
| Counting semaphore, producer-consumer | `producer_consumer` | ambulances → triage bay → doctors; same idea as the connection pool |
| Readers-writers lock (`std::shared_mutex`) | `readers_writers` | route statistics: requests write, `/api/os/live` reads |
| Deadlock prevention (try_lock, lock ordering) | `deadlock` | `/api/allocations` locks patient → bed → staff → equipment |
| Deadlock avoidance — Banker's algorithm | `bankers` | builds Allocation / Available from MySQL (live) or textbook sample |
| Dining philosophers (detect + resource hierarchy) | `dining_philosophers` | same ordering rule as allocations |
| Starvation, bounded waiting (ticket lock) | `starvation` | pool waits time out after 10 s; queue ties broken by arrival |
| CPU scheduling: FCFS, SJF, Priority, RR, Priority + aging | `scheduling` | the live emergency queue is the ready queue |
| Bounded resource pool (mutex + condition variable) | `connection_pool` | `ConnectionPool::acquire()` in `db_pool.hpp` |
| Background timer thread (atomic stop flag) | `timer_thread` | severity auto-release thread in `main.cpp` |

Console version (no MySQL needed, same code):
`cd os_concepts && g++ -std=c++17 -pthread -I../backend/src -I../backend/third_party deadlock_starvation_demo.cpp -o os_demo`

## DBMS concepts — all in `backend/src/dbms_concepts.hpp`, connected to the **DBMS Concepts** page

On start-up the server creates (safely, every time) the `audit_log` table, the views
`v_active_queue` and `v_resource_utilization`, the function `fn_priority_label`, the procedures
`sp_hospital_summary` and `sp_count_by_priority`, six triggers and five indexes. You will see
`DBMS objects ready: 17/17` in the terminal. Pressing **Run in C++** calls `POST /api/dbms/run/:id`,
which runs real SQL and returns the statements, the result tables and a log.
Demos that change data always end with `ROLLBACK`, so your hospital data is never changed.

| Concept | Demo id |
|---|---|
| Keys & constraints (PK, FK, UNIQUE, CHECK, NOT NULL) | `keys_constraints` |
| ER model → relational schema, ON DELETE / ON UPDATE | `relationships` |
| Normalization (1NF, 2NF, 3NF, BCNF) + lossless join | `normalization` |
| Joins: INNER, LEFT, multi-table, SELF | `joins` |
| Aggregation: COUNT / SUM / AVG, GROUP BY, HAVING | `aggregation` |
| Subqueries: nested, correlated, EXISTS, NOT IN | `subqueries` |
| Set operations: UNION ALL, UNION, INTERSECT, EXCEPT | `set_operations` |
| Views | `views` |
| Stored function + procedures (IN / OUT) | `stored_routines` |
| Triggers + audit trail (proved inside a rolled-back transaction) | `triggers` |
| Indexing (B+ tree) + EXPLAIN | `indexing` |
| Transactions, ACID, SAVEPOINT / ROLLBACK | `transactions` |
| Isolation levels, dirty read (two connections) | `isolation` |
| Row-level locking: FOR UPDATE, FOR SHARE, NOWAIT | `locking` |
| Database deadlock detection, victim, recovery (error 1213) | `db_deadlock` |
| Integrity enforcement (each constraint violated on purpose) | `integrity` |

If a trigger or routine cannot be created (shown in red on the DBMS page), the MySQL user needs
the `CREATE ROUTINE` / `TRIGGER` privileges, or run `SET GLOBAL log_bin_trust_function_creators = 1;` once as root.

## Troubleshooting
- **"Could not connect to MySQL"** – MySQL isn't running, the password in `backend/.env` is wrong, or you skipped step 2.
- **Sidebar says "Not reachable"** – the C++ server isn't running, or it's on a port other than 8080.
- **Windows: missing `mysqlcppconn-*.dll`** – copy the DLLs from the Connector/C++ `lib64` folder next to the `.exe`.
- **CMake can't find Connector/C++** – pass `-DMYSQL_CONCPP_DIR="<install folder>"`.

This is a student project, not a certified clinical system. Add login and access control before using real patient data.
