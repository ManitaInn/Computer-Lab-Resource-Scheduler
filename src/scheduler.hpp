// scheduler.hpp
// ---------------------------------------------------------------------------
// Core engine of the Computer Lab Resource Scheduler.
//
// This file has NO dependency on the web server or on JSON. It is a plain,
// self-contained C++17 simulation of how an OS-style scheduler hands a limited
// pool of resources (lab computers) to competing "processes" (student sessions).
//
//   OS concept            ->  in this project
//   --------------------      --------------------------------------------
//   Process                   one student session (id, arrival, burst time)
//   CPU / resource            one lab computer
//   Ready queue               students waiting for a free computer
//   Process states            NEW -> READY (waiting) -> RUNNING -> TERMINATED
//   Scheduling policy         FCFS, SJF (non-preemptive), PRIORITY
//   Burst time                requested session duration (minutes)
//   Waiting time              start - arrival
//   Turnaround time           end - arrival
//
// The simulation is discrete-event: simulated time jumps from event to event
// (an arrival or a session finishing), so it is fast and exact (integer minutes).
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstddef>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace lab {

// ----------------------------------------------------------------- limits --
// Central place for every validation limit (the API exposes these to the UI).
constexpr int MAX_COMPUTERS = 100;
constexpr int MAX_REQUESTS  = 500;
constexpr int MAX_ARRIVAL   = 10000;  // minutes
constexpr int MAX_DURATION  = 600;    // minutes (10 hours)
constexpr int MIN_STUDENT_PRIORITY = 1;  // 1 = most urgent
constexpr int MAX_STUDENT_PRIORITY = 5;  // 5 = least urgent
constexpr std::size_t MAX_ID_LENGTH = 16;

// ----------------------------------------------------------------- policy --
enum class Policy { FCFS, SJF, PRIORITY };

inline std::string policyToString(Policy p) {
    switch (p) {
        case Policy::FCFS:     return "FCFS";
        case Policy::SJF:      return "SJF";
        case Policy::PRIORITY: return "PRIORITY";
    }
    return "FCFS";
}

inline bool parsePolicy(std::string text, Policy& out) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (text == "FCFS")     { out = Policy::FCFS;     return true; }
    if (text == "SJF")      { out = Policy::SJF;      return true; }
    if (text == "PRIORITY") { out = Policy::PRIORITY; return true; }
    return false;
}

inline std::string policyDescription(Policy p) {
    switch (p) {
        case Policy::FCFS:
            return "First-Come, First-Served: the student who arrived earliest gets the next free computer.";
        case Policy::SJF:
            return "Shortest Job First (non-preemptive): the shortest requested session goes first. "
                   "Minimises average waiting time but long sessions can starve.";
        case Policy::PRIORITY:
            return "Priority: priority 1 is served before priority 5. Ties are broken by arrival order. "
                   "Urgent users win, but low-priority users can starve.";
    }
    return "";
}

// ------------------------------------------------------------- data types --
struct StudentRequest {
    std::string id;
    int arrival  = 0;  // minute at which the student shows up
    int duration = 1;  // requested session length in minutes
    int priority = 3;  // 1 (highest) .. 5 (lowest); only used by Policy::PRIORITY
};

// One scheduled session (the result of assigning a request to a computer).
struct Session {
    std::string studentId;
    int computer = 0;  // 1-based computer number
    int arrival  = 0;
    int start    = 0;
    int end      = 0;
    int duration = 0;
    int priority = 3;

    int wait() const       { return start - arrival; }
    int turnaround() const { return end - arrival; }
};

struct ComputerStats {
    int    id = 0;
    int    busyMinutes = 0;
    int    sessions = 0;
    double utilization = 0.0;  // busyMinutes / observation window, 0..1
};

struct Metrics {
    int    totalRequests = 0;
    int    completed = 0;
    double avgWait = 0.0;
    int    maxWait = 0;
    double avgTurnaround = 0.0;
    int    windowStart = 0;     // first arrival
    int    windowEnd = 0;       // last session end
    int    makespan = 0;        // windowEnd - windowStart
    double utilization = 0.0;   // total busy minutes / (computers * makespan), 0..1
    int    maxQueueLength = 0;  // longest waiting line seen at any moment
    int    studentsWhoWaited = 0;
};

struct Result {
    Policy policy = Policy::FCFS;
    int computers = 1;
    std::vector<Session> sessions;          // ordered by start time, then computer
    std::vector<ComputerStats> perComputer; // one entry per computer
    Metrics metrics;
};

// ------------------------------------------------------------- validation --
inline std::string normalizeId(std::string id) {
    // trim spaces, then upper-case so "s001" and "S001" are the same student
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    id.erase(id.begin(), std::find_if(id.begin(), id.end(), notSpace));
    id.erase(std::find_if(id.rbegin(), id.rend(), notSpace).base(), id.end());
    std::transform(id.begin(), id.end(), id.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return id;
}

// Returns an empty string when the request is valid, otherwise a human-readable
// error. `existing` is used to reject duplicate student IDs.
inline std::string validateRequest(const StudentRequest& r,
                                   const std::vector<StudentRequest>& existing) {
    if (r.id.empty())
        return "Student ID is required.";
    if (r.id.size() > MAX_ID_LENGTH)
        return "Student ID must be at most " + std::to_string(MAX_ID_LENGTH) + " characters.";
    for (unsigned char c : r.id) {
        if (!(std::isalnum(c) || c == '-' || c == '_'))
            return "Student ID may only contain letters, digits, '-' and '_'.";
    }
    for (const auto& e : existing) {
        if (e.id == r.id) return "Student ID '" + r.id + "' already exists.";
    }
    if (static_cast<int>(existing.size()) >= MAX_REQUESTS)
        return "Too many requests (maximum " + std::to_string(MAX_REQUESTS) + ").";
    if (r.arrival < 0 || r.arrival > MAX_ARRIVAL)
        return "Arrival time must be between 0 and " + std::to_string(MAX_ARRIVAL) + " minutes.";
    if (r.duration < 1 || r.duration > MAX_DURATION)
        return "Duration must be between 1 and " + std::to_string(MAX_DURATION) + " minutes.";
    if (r.priority < MIN_STUDENT_PRIORITY || r.priority > MAX_STUDENT_PRIORITY)
        return "Priority must be between " + std::to_string(MIN_STUDENT_PRIORITY) + " (highest) and " +
               std::to_string(MAX_STUDENT_PRIORITY) + " (lowest).";
    return "";
}

inline std::string validateComputers(int n) {
    if (n < 1 || n > MAX_COMPUTERS)
        return "Number of computers must be between 1 and " + std::to_string(MAX_COMPUTERS) + ".";
    return "";
}

// --------------------------------------------------------------- simulate --
namespace detail {

// "Is request a more deserving than request b?" for the given policy.
// Ties are resolved by the caller (earlier position in the waiting list wins),
// and the waiting list is always kept in (arrival, input order) order, so every
// policy is fully deterministic.
inline bool moreDeserving(const StudentRequest& a, const StudentRequest& b, Policy p) {
    switch (p) {
        case Policy::FCFS:     return false;  // list order already is arrival order
        case Policy::SJF:      return a.duration < b.duration;
        case Policy::PRIORITY: return a.priority < b.priority;
    }
    return false;
}

struct Machine {
    bool busy = false;
    int  busyUntil = 0;
    int  busyMinutes = 0;
    int  sessions = 0;
};

}  // namespace detail

// Runs the full simulation and returns every session plus the metrics.
// Throws std::invalid_argument for a bad computer count.
inline Result simulate(const std::vector<StudentRequest>& requests, int numComputers, Policy policy) {
    if (auto err = validateComputers(numComputers); !err.empty())
        throw std::invalid_argument(err);

    Result result;
    result.policy = policy;
    result.computers = numComputers;

    const int n = static_cast<int>(requests.size());
    std::vector<detail::Machine> machines(static_cast<std::size_t>(numComputers));

    // Arrival order, stable so that equal arrival times keep their input order.
    std::vector<int> arrivalOrder(static_cast<std::size_t>(n));
    std::iota(arrivalOrder.begin(), arrivalOrder.end(), 0);
    std::stable_sort(arrivalOrder.begin(), arrivalOrder.end(), [&](int a, int b) {
        return requests[static_cast<std::size_t>(a)].arrival < requests[static_cast<std::size_t>(b)].arrival;
    });

    std::vector<int> waiting;  // indices into `requests`, kept in arrival order
    std::size_t nextArrival = 0;
    int scheduled = 0;
    int maxQueue = 0;
    int now = n > 0 ? requests[static_cast<std::size_t>(arrivalOrder[0])].arrival : 0;

    while (scheduled < n) {
        // 1. release computers whose session has finished
        for (auto& m : machines)
            if (m.busy && m.busyUntil <= now) m.busy = false;

        // 2. admit everybody who has arrived by now into the waiting queue
        while (nextArrival < arrivalOrder.size() &&
               requests[static_cast<std::size_t>(arrivalOrder[nextArrival])].arrival <= now) {
            waiting.push_back(arrivalOrder[nextArrival++]);
        }

        // 3. while a computer is free and someone is waiting, dispatch
        while (!waiting.empty()) {
            int freeIdx = -1;
            for (int i = 0; i < numComputers; ++i) {
                if (!machines[static_cast<std::size_t>(i)].busy) { freeIdx = i; break; }
            }
            if (freeIdx < 0) break;  // lab is full

            std::size_t pick = 0;
            for (std::size_t i = 1; i < waiting.size(); ++i) {
                if (detail::moreDeserving(requests[static_cast<std::size_t>(waiting[i])],
                                          requests[static_cast<std::size_t>(waiting[pick])], policy))
                    pick = i;
            }
            const StudentRequest& r = requests[static_cast<std::size_t>(waiting[pick])];

            Session s;
            s.studentId = r.id;
            s.computer = freeIdx + 1;
            s.arrival = r.arrival;
            s.start = now;
            s.end = now + r.duration;
            s.duration = r.duration;
            s.priority = r.priority;
            result.sessions.push_back(s);

            auto& m = machines[static_cast<std::size_t>(freeIdx)];
            m.busy = true;
            m.busyUntil = s.end;
            m.busyMinutes += r.duration;
            m.sessions += 1;

            waiting.erase(waiting.begin() + static_cast<std::ptrdiff_t>(pick));
            ++scheduled;
        }

        // The line that remains after dispatching is the real queue length.
        maxQueue = std::max(maxQueue, static_cast<int>(waiting.size()));
        if (scheduled == n) break;

        // 4. jump to the next event: next arrival or next computer becoming free
        int next = INT_MAX;
        if (nextArrival < arrivalOrder.size())
            next = std::min(next, requests[static_cast<std::size_t>(arrivalOrder[nextArrival])].arrival);
        for (const auto& m : machines)
            if (m.busy) next = std::min(next, m.busyUntil);

        if (next == INT_MAX || next <= now)
            throw std::logic_error("scheduler failed to advance time");  // cannot happen
        now = next;
    }

    // ---------------------------------------------------------- metrics --
    std::stable_sort(result.sessions.begin(), result.sessions.end(), [](const Session& a, const Session& b) {
        if (a.start != b.start) return a.start < b.start;
        return a.computer < b.computer;
    });

    Metrics& mt = result.metrics;
    mt.totalRequests = n;
    mt.completed = static_cast<int>(result.sessions.size());
    mt.maxQueueLength = maxQueue;

    if (n > 0) {
        long long waitSum = 0, turnSum = 0, busySum = 0;
        int firstArrival = INT_MAX, lastEnd = 0;
        for (const auto& s : result.sessions) {
            waitSum += s.wait();
            turnSum += s.turnaround();
            busySum += s.duration;
            mt.maxWait = std::max(mt.maxWait, s.wait());
            if (s.wait() > 0) ++mt.studentsWhoWaited;
            firstArrival = std::min(firstArrival, s.arrival);
            lastEnd = std::max(lastEnd, s.end);
        }
        mt.avgWait = static_cast<double>(waitSum) / n;
        mt.avgTurnaround = static_cast<double>(turnSum) / n;
        mt.windowStart = firstArrival;
        mt.windowEnd = lastEnd;
        mt.makespan = lastEnd - firstArrival;
        if (mt.makespan > 0)
            mt.utilization = static_cast<double>(busySum) / (static_cast<double>(numComputers) * mt.makespan);
    }

    for (int i = 0; i < numComputers; ++i) {
        const auto& m = machines[static_cast<std::size_t>(i)];
        ComputerStats cs;
        cs.id = i + 1;
        cs.busyMinutes = m.busyMinutes;
        cs.sessions = m.sessions;
        cs.utilization = mt.makespan > 0 ? static_cast<double>(m.busyMinutes) / mt.makespan : 0.0;
        result.perComputer.push_back(cs);
    }
    return result;
}

}  // namespace lab
