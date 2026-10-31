// Self-checking tests for the scheduling engine. Build and run with:  make test
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <random>

#include "../src/scheduler.hpp"
#include "../src/storage.hpp"

using namespace lab;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::cerr << "FAIL  line " << __LINE__ << ": " #cond << "\n";        \
        }                                                                        \
    } while (0)

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

static const Session& find(const Result& r, const std::string& id) {
    for (const auto& s : r.sessions) if (s.studentId == id) return s;
    std::cerr << "missing session " << id << "\n";
    std::exit(2);
}

// ---- the worked example from the project plan -----------------------------
static void testPlanExample() {
    std::vector<StudentRequest> reqs = {{"S001", 0, 30, 3}, {"S002", 0, 20, 3}, {"S003", 5, 15, 3}};
    Result r = simulate(reqs, 2, Policy::FCFS);

    CHECK(find(r, "S001").computer == 1 && find(r, "S001").start == 0 && find(r, "S001").end == 30);
    CHECK(find(r, "S002").computer == 2 && find(r, "S002").start == 0 && find(r, "S002").end == 20);
    CHECK(find(r, "S003").start == 20);          // waits until a computer frees up at minute 20
    CHECK(find(r, "S003").wait() == 15);
    CHECK(find(r, "S003").computer == 2);
    CHECK(find(r, "S003").end == 35);
    CHECK(near(r.metrics.avgWait, 5.0));         // (0 + 0 + 15) / 3
    CHECK(r.metrics.maxWait == 15);
    CHECK(r.metrics.makespan == 35);
    CHECK(near(r.metrics.utilization, 65.0 / 70.0));  // 65 busy minutes of 2 x 35
    CHECK(r.metrics.maxQueueLength == 1);
    CHECK(r.metrics.studentsWhoWaited == 1);
}

// ---- policies choose differently when they should --------------------------
static void testPolicyOrdering() {
    // One computer. A runs 0-10. While it runs, B (long, low priority), C (short),
    // D (medium, top priority) all queue up in that arrival order.
    std::vector<StudentRequest> reqs = {
        {"A", 0, 10, 3}, {"B", 1, 20, 5}, {"C", 2, 2, 4}, {"D", 3, 8, 1}};

    Result fcfs = simulate(reqs, 1, Policy::FCFS);
    CHECK(find(fcfs, "B").start == 10 && find(fcfs, "C").start == 30 && find(fcfs, "D").start == 32);

    Result sjf = simulate(reqs, 1, Policy::SJF);
    CHECK(find(sjf, "C").start == 10 && find(sjf, "D").start == 12 && find(sjf, "B").start == 20);

    Result pri = simulate(reqs, 1, Policy::PRIORITY);
    CHECK(find(pri, "D").start == 10 && find(pri, "C").start == 18 && find(pri, "B").start == 20);

    // SJF must never be worse than FCFS on average wait for this workload
    CHECK(sjf.metrics.avgWait <= fcfs.metrics.avgWait);
}

static void testTiesAreStable() {
    // identical keys -> input order decides, for every policy
    std::vector<StudentRequest> reqs = {{"X", 0, 5, 2}, {"Y", 0, 5, 2}, {"Z", 0, 5, 2}};
    for (Policy p : {Policy::FCFS, Policy::SJF, Policy::PRIORITY}) {
        Result r = simulate(reqs, 1, p);
        CHECK(find(r, "X").start == 0 && find(r, "Y").start == 5 && find(r, "Z").start == 10);
    }
}

static void testEdgeCases() {
    Result empty = simulate({}, 3, Policy::FCFS);
    CHECK(empty.sessions.empty() && empty.metrics.makespan == 0 && near(empty.metrics.utilization, 0.0));
    CHECK(empty.perComputer.size() == 3);

    // late first arrival: idle time before the first student is not counted as lab idleness
    Result late = simulate({{"L", 50, 10, 3}}, 2, Policy::FCFS);
    CHECK(find(late, "L").start == 50 && find(late, "L").wait() == 0);
    CHECK(late.metrics.windowStart == 50 && late.metrics.makespan == 10);
    CHECK(near(late.metrics.utilization, 0.5));  // one of two computers busy the whole window

    // more computers than students
    Result many = simulate({{"A", 0, 5, 1}, {"B", 0, 5, 1}}, 10, Policy::FCFS);
    CHECK(near(many.metrics.avgWait, 0.0) && many.metrics.maxQueueLength == 0);

    // a computer freeing at the exact minute a student arrives is usable immediately
    Result touch = simulate({{"A", 0, 10, 1}, {"B", 10, 5, 1}}, 1, Policy::FCFS);
    CHECK(find(touch, "B").start == 10 && find(touch, "B").wait() == 0);

    bool threw = false;
    try { simulate({}, 0, Policy::FCFS); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

// ---- randomized invariants (the real proof of correctness) ----------------
static void checkInvariants(const std::vector<StudentRequest>& reqs, const Result& r) {
    const int n = static_cast<int>(reqs.size());
    CHECK(static_cast<int>(r.sessions.size()) == n);

    std::map<std::string, const StudentRequest*> byId;
    for (const auto& q : reqs) byId[q.id] = &q;

    int horizon = 0;
    for (const auto& s : r.sessions) {
        const StudentRequest* q = byId.at(s.studentId);
        CHECK(s.start >= q->arrival);                    // nobody starts before arriving
        CHECK(s.end == s.start + q->duration);           // session length is honoured
        CHECK(s.computer >= 1 && s.computer <= r.computers);
        horizon = std::max(horizon, s.end);
    }

    // per-minute occupancy: no computer double-booked, and no computer idle while someone waits
    std::vector<std::vector<int>> busy(static_cast<std::size_t>(r.computers),
                                       std::vector<int>(static_cast<std::size_t>(horizon + 1), 0));
    for (const auto& s : r.sessions)
        for (int t = s.start; t < s.end; ++t) busy[static_cast<std::size_t>(s.computer - 1)][static_cast<std::size_t>(t)]++;
    for (auto& row : busy) for (int v : row) CHECK(v <= 1);

    for (const auto& s : r.sessions) {
        for (int t = s.arrival; t < s.start; ++t) {     // s is waiting during minute t
            int busyCount = 0;
            for (int c = 0; c < r.computers; ++c) busyCount += busy[static_cast<std::size_t>(c)][static_cast<std::size_t>(t)];
            CHECK(busyCount == r.computers);             // work-conserving: lab was full
        }
    }

    // metrics agree with the raw sessions
    double waitSum = 0;
    long long busySum = 0;
    for (const auto& s : r.sessions) { waitSum += s.wait(); busySum += s.duration; }
    if (n > 0) CHECK(near(r.metrics.avgWait, waitSum / n));
    CHECK(r.metrics.utilization >= 0.0 && r.metrics.utilization <= 1.0 + 1e-12);
    long long perSum = 0;
    for (const auto& c : r.perComputer) perSum += c.busyMinutes;
    CHECK(perSum == busySum);
}

static void testRandomized() {
    std::mt19937 rng(12345);
    for (int round = 0; round < 300; ++round) {
        int n = static_cast<int>(rng() % 40);
        int computers = 1 + static_cast<int>(rng() % 6);
        std::vector<StudentRequest> reqs;
        for (int i = 0; i < n; ++i)
            reqs.push_back({"S" + std::to_string(i + 1), static_cast<int>(rng() % 120),
                            1 + static_cast<int>(rng() % 45), 1 + static_cast<int>(rng() % 5)});
        for (Policy p : {Policy::FCFS, Policy::SJF, Policy::PRIORITY})
            checkInvariants(reqs, simulate(reqs, computers, p));
    }
}

// ---- validation + file format ---------------------------------------------
static void testValidationAndStorage() {
    std::vector<StudentRequest> existing = {{"S001", 0, 10, 3}};
    CHECK(validateRequest({"S002", 0, 10, 3}, existing).empty());
    CHECK(!validateRequest({"S001", 0, 10, 3}, existing).empty());   // duplicate
    CHECK(!validateRequest({"", 0, 10, 3}, existing).empty());
    CHECK(!validateRequest({"A B", 0, 10, 3}, existing).empty());    // space in id
    CHECK(!validateRequest({"S9", -1, 10, 3}, existing).empty());
    CHECK(!validateRequest({"S9", 0, 0, 3}, existing).empty());
    CHECK(!validateRequest({"S9", 0, MAX_DURATION + 1, 3}, existing).empty());
    CHECK(!validateRequest({"S9", 0, 10, 0}, existing).empty());
    CHECK(!validateRequest({"S9", 0, 10, 6}, existing).empty());
    CHECK(normalizeId("  s001 ") == "S001");

    LabState s;
    s.computers = 3;
    s.policy = Policy::PRIORITY;
    s.requests = {{"S001", 0, 30, 1}, {"S002", 4, 15, 5}};
    LabState back;
    CHECK(parseState(serializeState(s), back).empty());
    CHECK(back.computers == 3 && back.policy == Policy::PRIORITY && back.requests.size() == 2);
    CHECK(back.requests[1].id == "S002" && back.requests[1].arrival == 4 &&
          back.requests[1].duration == 15 && back.requests[1].priority == 5);

    LabState junk;
    CHECK(!parseState("COMPUTERS 2\nS001 0 abc 3\n", junk).empty());          // bad number
    CHECK(!parseState("POLICY FCFS\nS001 0 10 3\n", junk).empty());           // missing COMPUTERS
    CHECK(!parseState("COMPUTERS 2\nS001 0 10 3\nS001 1 5 3\n", junk).empty());  // duplicate id
    CHECK(parseState("# c\r\nCOMPUTERS 2\r\n\r\ns1 0 10 3\r\n", junk).empty() && junk.requests[0].id == "S1");

    CHECK(isValidSaveName("week-1_lab"));
    CHECK(!isValidSaveName("../etc/passwd"));
    CHECK(!isValidSaveName(""));
}

int main() {
    testPlanExample();
    testPolicyOrdering();
    testTiesAreStable();
    testEdgeCases();
    testRandomized();
    testValidationAndStorage();

    std::cout << checks << " checks, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
