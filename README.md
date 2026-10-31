# Lab Scheduler

A Computer Lab Resource Scheduler written in C++17, with a browser interface.
Students request time on a limited number of lab computers; the C++ engine decides who gets which computer and when, using FCFS, Shortest Job First or Priority scheduling, and reports waiting time and utilization.

```
browser (web/)  <--JSON over HTTP-->  C++ server (src/main.cpp)  -->  engine (src/scheduler.hpp)
                                                                  -->  text files (src/storage.hpp, data/)
```

The browser only draws the schedule. Every scheduling decision and every number comes from the C++ code.

## Run it

You need a C++17 compiler (g++ or clang++) and `make`. Both libraries are already included in `third_party/`.

```
make            # builds ./lab_scheduler (takes ~30 s the first time)
make test       # runs the engine tests
./lab_scheduler # then open http://127.0.0.1:8080
```

A different port: `./lab_scheduler 8081`.

**Windows (MinGW-w64 / MSYS2):**

```
g++ -std=c++17 -O2 -pthread src/main.cpp -Ithird_party -o lab_scheduler.exe -lws2_32 -lwsock32
lab_scheduler.exe
```

Start the server from the project folder (the one containing `web/`).

## How the scheduler works

Discrete-event simulation in whole minutes (`simulate()` in `src/scheduler.hpp`). Time jumps between events: an arrival, or a session ending.

1. Release computers whose session has ended.
2. Move everyone who has arrived into the waiting queue.
3. While a computer is free and someone is waiting, pick the next student by policy and give them the lowest-numbered free computer.
4. Jump to the next arrival or the next finishing session.

| Policy | Next student is... | Trade-off |
| --- | --- | --- |
| FCFS | the one who arrived first | fair, but one long session delays everyone behind it |
| SJF (non-preemptive) | the one with the shortest session | lowest average wait, long sessions can starve |
| Priority | the one with priority 1 before 5 | urgent users first, low priority can starve |

Ties always fall back to arrival order, so results are repeatable.

Metrics: wait = start − arrival; turnaround = end − arrival; utilization = total session minutes ÷ (computers × minutes from the first arrival to the last finish).

## How it maps to OS chapters learn in class

| Chapter | In this project |
| --- | --- |
| 1. Computer System Overview | The computers are the limited hardware resource |
| 2. OS Overview | The scheduler allocates a resource, like the OS does |
| 3. Processes | Each student session is a process: not arrived (new), waiting (ready), using a computer (running), finished (terminated). The Simulation tab shows these four counts live |
| 4. Threads | The server handles requests on several threads (cpp-httplib thread pool) |
| 5. Concurrency | `g_mutex` in `main.cpp` protects the shared lab state, so two simultaneous requests cannot corrupt it |

## What is in the interface

- **Simulation**: press Play to watch computers fill up and the waiting line move; drag the slider or click the timeline to jump to any minute.
- **Schedule**: every session with arrival, start, finish and wait.
- **Compare policies**: FCFS, SJF and Priority on the same requests, best value per row marked.
- **How many computers?**: average wait and utilization for 1 to 12+ computers.
- **Files**: Save and Load use plain `.txt` files in `data/`; Export and Import move the same format through the browser. The last setup is restored automatically on restart (`data/autosave.txt`).

Text file format:

```
COMPUTERS 2
POLICY FCFS
# id arrival duration priority
S001 0 30 3
S002 0 20 3
S003 5 15 3
```

## Tests

`make test` checks the worked example (S003 waits 15 min, average wait 5.0, utilization 92.9%), that each policy picks differently when it should, input validation, the file format, and runs 900 random schedules checking these rules:

- nobody starts before arriving
- no computer is ever double-booked
- no computer sits idle while someone waits
- the reported metrics match the raw sessions

## API

| Method and path | Purpose |
| --- | --- |
| `GET /api/state` | computers, policy, requests, limits |
| `PUT /api/config` | `{computers, policy}` |
| `POST /api/requests` | `{id?, arrival, duration, priority}` |
| `DELETE /api/requests/:id`, `DELETE /api/requests` | remove one / clear all |
| `POST /api/sample` | `{kind: "example" \| "demo" \| "random", count?, seed?}` |
| `GET /api/simulate` | full schedule and metrics (`?policy=&computers=` for what-if) |
| `GET /api/compare` | metrics for all three policies |
| `GET /api/sweep` | metrics for 1..N computers |
| `GET /api/files`, `POST /api/save`, `POST /api/load` | named text files in `data/` |
| `GET /api/export`, `POST /api/import` | download / upload the text format |

## Files

```
src/scheduler.hpp   engine: policies, simulation, metrics, validation
src/storage.hpp     text-file save / load
src/main.cpp        HTTP server and JSON API
web/                index.html, style.css, app.js
tests/              test_scheduler.cpp
third_party/        cpp-httplib, nlohmann/json (single headers, MIT licence)
data/               saved setups
```
