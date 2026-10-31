// main.cpp - HTTP API + static file server for the Computer Lab Resource Scheduler.
//
//   ./lab_scheduler [port]        (default port 8080, listens on 127.0.0.1 only)
//
// All scheduling logic lives in scheduler.hpp; this file only translates between
// HTTP/JSON and the engine, and keeps the one shared LabState protected by a mutex
// (the server handles requests on several threads - classic OS concurrency).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "httplib.h"
#include "json.hpp"
#include "scheduler.hpp"
#include "storage.hpp"

using json = nlohmann::json;
using namespace lab;

namespace {

std::mutex g_mutex;        // protects g_state
LabState g_state;          // the current lab setup
std::string g_dataDir = "data";

// ------------------------------------------------------------------ JSON --
json requestToJson(const StudentRequest& r) {
    return {{"id", r.id}, {"arrival", r.arrival}, {"duration", r.duration}, {"priority", r.priority}};
}

json limitsJson() {
    return {{"maxComputers", MAX_COMPUTERS}, {"maxRequests", MAX_REQUESTS},
            {"maxArrival", MAX_ARRIVAL},     {"maxDuration", MAX_DURATION},
            {"minPriority", MIN_STUDENT_PRIORITY}, {"maxPriority", MAX_STUDENT_PRIORITY},
            {"maxIdLength", MAX_ID_LENGTH}};
}

json policiesJson() {
    json arr = json::array();
    const std::pair<Policy, const char*> names[] = {
        {Policy::FCFS, "First-Come, First-Served"},
        {Policy::SJF, "Shortest Job First"},
        {Policy::PRIORITY, "Priority"}};
    for (const auto& [p, name] : names)
        arr.push_back({{"key", policyToString(p)}, {"name", name}, {"description", policyDescription(p)}});
    return arr;
}

json stateToJson(const LabState& s) {
    json reqs = json::array();
    for (const auto& r : s.requests) reqs.push_back(requestToJson(r));
    return {{"computers", s.computers}, {"policy", policyToString(s.policy)}, {"requests", reqs},
            {"limits", limitsJson()},   {"policies", policiesJson()}};
}

json metricsToJson(const Metrics& m) {
    return {{"totalRequests", m.totalRequests}, {"completed", m.completed},
            {"avgWait", m.avgWait},             {"maxWait", m.maxWait},
            {"avgTurnaround", m.avgTurnaround}, {"windowStart", m.windowStart},
            {"windowEnd", m.windowEnd},         {"makespan", m.makespan},
            {"utilization", m.utilization},     {"maxQueueLength", m.maxQueueLength},
            {"studentsWhoWaited", m.studentsWhoWaited}};
}

json resultToJson(const Result& r) {
    json sessions = json::array();
    for (const auto& s : r.sessions)
        sessions.push_back({{"studentId", s.studentId}, {"computer", s.computer},
                            {"arrival", s.arrival},     {"start", s.start},
                            {"end", s.end},             {"duration", s.duration},
                            {"priority", s.priority},   {"wait", s.wait()},
                            {"turnaround", s.turnaround()}});
    json comps = json::array();
    for (const auto& c : r.perComputer)
        comps.push_back({{"id", c.id}, {"busyMinutes", c.busyMinutes},
                         {"sessions", c.sessions}, {"utilization", c.utilization}});
    return {{"policy", policyToString(r.policy)}, {"computers", r.computers},
            {"sessions", sessions},               {"perComputer", comps},
            {"metrics", metricsToJson(r.metrics)}};
}

// ------------------------------------------------------------- HTTP utils --
void sendJson(httplib::Response& res, const json& j, int status = 200) {
    res.status = status;
    res.set_content(j.dump(), "application/json");
}

void sendError(httplib::Response& res, int status, const std::string& message) {
    sendJson(res, {{"error", message}}, status);
}

// Parses an optional JSON object body. On failure writes a 400 and returns false.
bool parseBody(const httplib::Request& req, httplib::Response& res, json& body) {
    if (req.body.empty()) { body = json::object(); return true; }
    body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        sendError(res, 400, "Request body must be a JSON object.");
        return false;
    }
    return true;
}

// Reads an optional integer field. Missing/null keeps `out` unchanged.
bool readInt(const json& body, const char* key, int& out, std::string& err) {
    auto it = body.find(key);
    if (it == body.end() || it->is_null()) return true;
    if (!it->is_number_integer()) { err = std::string("'") + key + "' must be a whole number."; return false; }
    long long v = it->get<long long>();
    if (v < -1000000000LL || v > 1000000000LL) { err = std::string("'") + key + "' is out of range."; return false; }
    out = static_cast<int>(v);
    return true;
}

LabState copyState() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state;
}

// Call with g_mutex held. Keeps the last setup across server restarts.
void persistLocked() {
    if (auto err = saveStateToFile(g_dataDir, "autosave", g_state); !err.empty())
        std::cerr << "warning: autosave failed: " << err << "\n";
}

std::string nextAutoId(const std::vector<StudentRequest>& rs) {
    int maxNum = 0;
    for (const auto& r : rs) {
        if (r.id.size() < 2 || r.id.size() > 7 || r.id[0] != 'S') continue;
        std::string digits = r.id.substr(1);
        if (std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); }))
            maxNum = std::max(maxNum, std::stoi(digits));
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "S%03d", maxNum + 1);
    return buf;
}

// --------------------------------------------------------------- samples --
LabState demoState() {
    LabState s;
    s.computers = 3;
    s.policy = Policy::FCFS;
    s.requests = {{"S001", 0, 45, 3},  {"S002", 0, 30, 2},  {"S003", 5, 60, 4},  {"S004", 10, 20, 1},
                  {"S005", 12, 40, 3}, {"S006", 15, 25, 5}, {"S007", 20, 55, 2}, {"S008", 25, 15, 3},
                  {"S009", 30, 35, 4}, {"S010", 35, 20, 1}, {"S011", 45, 50, 3}, {"S012", 50, 30, 5}};
    return s;
}

// The worked example from the project plan (2 computers, 3 students).
std::vector<StudentRequest> planExample() {
    return {{"S001", 0, 30, 3}, {"S002", 0, 20, 3}, {"S003", 5, 15, 3}};
}

// Random but realistic: arrivals are spread so the lab is busy (~90% load) and queues form.
std::vector<StudentRequest> randomRequests(int count, int computers, unsigned seed) {
    std::mt19937 rng(seed);
    std::discrete_distribution<int> priorityDist({10, 15, 40, 20, 15});  // priorities 1..5
    std::uniform_int_distribution<int> durationSteps(0, 16);             // 10..90 min in steps of 5

    const double meanDuration = 50.0;
    int spread = static_cast<int>(count * meanDuration / computers * 0.9);
    spread = std::clamp(spread, 30, MAX_ARRIVAL);
    std::uniform_int_distribution<int> arrivalDist(0, spread);

    std::vector<int> arrivals(static_cast<std::size_t>(count));
    for (auto& a : arrivals) a = arrivalDist(rng);
    std::sort(arrivals.begin(), arrivals.end());

    std::vector<StudentRequest> out;
    for (int i = 0; i < count; ++i) {
        char id[16];
        std::snprintf(id, sizeof id, "S%03d", i + 1);
        out.push_back({id, arrivals[static_cast<std::size_t>(i)], 10 + 5 * durationSteps(rng),
                       priorityDist(rng) + 1});
    }
    return out;
}

}  // namespace

// ------------------------------------------------------------------ main --
int main(int argc, char** argv) {
    int port = 8080;
    if (const char* portEnv = std::getenv("PORT"); portEnv && *portEnv) {
        char* end = nullptr;
        const long parsedPort = std::strtol(portEnv, &end, 10);
        if (*end != '\0' || parsedPort < 1 || parsedPort > 65535) {
            std::cerr << "PORT must be a number between 1 and 65535.\n";
            return 1;
        }
        port = static_cast<int>(parsedPort);
    }
    if (argc > 1) {
        port = std::atoi(argv[1]);
        if (port < 1 || port > 65535) { std::cerr << "usage: " << argv[0] << " [port]\n"; return 1; }
    }
    const char* configuredHost = std::getenv("LAB_HOST");
    const std::string host = configuredHost && *configuredHost ? configuredHost : "127.0.0.1";

    // find the folder with index.html (works when started from the project root or from build/)
    std::string webDir = "web";
    if (!std::filesystem::exists("web/index.html") && std::filesystem::exists("../web/index.html")) {
        webDir = "../web";
        g_dataDir = "../data";
    }
    if (!std::filesystem::exists(webDir + "/index.html")) {
        std::cerr << "Cannot find web/index.html. Start the server from the project folder.\n";
        return 1;
    }

    // restore the last session, or start with the demo
    if (auto err = loadStateFromFile(g_dataDir, "autosave", g_state); !err.empty()) g_state = demoState();

    httplib::Server svr;
    svr.set_payload_max_length(1024 * 1024);

    svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
        sendError(res, 500, "Internal server error.");
    });
    svr.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
        if (req.path.rfind("/api/", 0) == 0 && res.body.empty())
            sendError(res, res.status, "Not found or not allowed.");
    });

    // ---- state -----------------------------------------------------------
    svr.Get("/api/state", [](const httplib::Request&, httplib::Response& res) {
        sendJson(res, stateToJson(copyState()));
    });

    svr.Put("/api/config", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;

        std::lock_guard<std::mutex> lock(g_mutex);
        int computers = g_state.computers;
        std::string err;
        if (!readInt(body, "computers", computers, err)) return sendError(res, 400, err);
        if (auto e = validateComputers(computers); !e.empty()) return sendError(res, 400, e);

        Policy policy = g_state.policy;
        if (body.contains("policy")) {
            if (!body["policy"].is_string() || !parsePolicy(body["policy"].get<std::string>(), policy))
                return sendError(res, 400, "Policy must be FCFS, SJF or PRIORITY.");
        }
        g_state.computers = computers;
        g_state.policy = policy;
        persistLocked();
        sendJson(res, stateToJson(g_state));
    });

    // ---- requests --------------------------------------------------------
    svr.Post("/api/requests", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;

        StudentRequest r;
        std::string err;
        if (!readInt(body, "arrival", r.arrival, err) || !readInt(body, "duration", r.duration, err) ||
            !readInt(body, "priority", r.priority, err))
            return sendError(res, 400, err);
        if (body.contains("id") && !body["id"].is_null()) {
            if (!body["id"].is_string()) return sendError(res, 400, "'id' must be text.");
            r.id = normalizeId(body["id"].get<std::string>());
        }
        if (!body.contains("duration")) return sendError(res, 400, "Duration is required.");

        std::lock_guard<std::mutex> lock(g_mutex);
        if (r.id.empty()) r.id = nextAutoId(g_state.requests);
        if (auto e = validateRequest(r, g_state.requests); !e.empty()) return sendError(res, 400, e);
        g_state.requests.push_back(r);
        persistLocked();
        sendJson(res, stateToJson(g_state), 201);
    });

    svr.Delete("/api/requests/:id", [](const httplib::Request& req, httplib::Response& res) {
        std::string id = normalizeId(req.path_params.at("id"));
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& v = g_state.requests;
        auto it = std::find_if(v.begin(), v.end(), [&](const StudentRequest& r) { return r.id == id; });
        if (it == v.end()) return sendError(res, 404, "No request with ID '" + id + "'.");
        v.erase(it);
        persistLocked();
        sendJson(res, stateToJson(g_state));
    });

    svr.Delete("/api/requests", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_state.requests.clear();
        persistLocked();
        sendJson(res, stateToJson(g_state));
    });

    // kind: "demo" | "example" | "random" (+ optional count, seed)
    svr.Post("/api/sample", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;
        std::string kind = body.value("kind", std::string("random"));

        std::lock_guard<std::mutex> lock(g_mutex);
        if (kind == "demo") {
            Policy keep = g_state.policy;
            g_state = demoState();
            g_state.policy = keep;
        } else if (kind == "example") {
            g_state.computers = 2;
            g_state.requests = planExample();
        } else if (kind == "random") {
            int count = 24;
            int seed = static_cast<int>(std::random_device{}() & 0x7fffffff);
            std::string err;
            if (!readInt(body, "count", count, err) || !readInt(body, "seed", seed, err))
                return sendError(res, 400, err);
            if (count < 1 || count > 100) return sendError(res, 400, "Sample size must be between 1 and 100.");
            g_state.requests = randomRequests(count, g_state.computers, static_cast<unsigned>(seed));
        } else {
            return sendError(res, 400, "Sample kind must be demo, example or random.");
        }
        persistLocked();
        sendJson(res, stateToJson(g_state));
    });

    // ---- simulation ------------------------------------------------------
    // Optional ?policy=SJF&computers=5 lets the UI "what-if" without changing the saved setup.
    svr.Get("/api/simulate", [](const httplib::Request& req, httplib::Response& res) {
        LabState s = copyState();
        if (req.has_param("policy") && !parsePolicy(req.get_param_value("policy"), s.policy))
            return sendError(res, 400, "Policy must be FCFS, SJF or PRIORITY.");
        if (req.has_param("computers")) {
            try { s.computers = std::stoi(req.get_param_value("computers")); }
            catch (...) { return sendError(res, 400, "computers must be a number."); }
        }
        if (auto e = validateComputers(s.computers); !e.empty()) return sendError(res, 400, e);
        sendJson(res, resultToJson(simulate(s.requests, s.computers, s.policy)));
    });

    // All three policies on the same requests, side by side.
    svr.Get("/api/compare", [](const httplib::Request&, httplib::Response& res) {
        LabState s = copyState();
        json arr = json::array();
        for (Policy p : {Policy::FCFS, Policy::SJF, Policy::PRIORITY}) {
            Result r = simulate(s.requests, s.computers, p);
            arr.push_back({{"policy", policyToString(p)}, {"metrics", metricsToJson(r.metrics)}});
        }
        sendJson(res, {{"computers", s.computers}, {"results", arr}});
    });

    // How do wait time and utilization change with 1..max computers?
    svr.Get("/api/sweep", [](const httplib::Request& req, httplib::Response& res) {
        LabState s = copyState();
        if (req.has_param("policy") && !parsePolicy(req.get_param_value("policy"), s.policy))
            return sendError(res, 400, "Policy must be FCFS, SJF or PRIORITY.");
        int maxN = 12;
        if (req.has_param("max")) {
            try { maxN = std::stoi(req.get_param_value("max")); }
            catch (...) { return sendError(res, 400, "max must be a number."); }
        }
        maxN = std::clamp(maxN, 1, 30);
        json pts = json::array();
        for (int n = 1; n <= maxN; ++n) {
            Result r = simulate(s.requests, n, s.policy);
            pts.push_back({{"computers", n},
                           {"avgWait", r.metrics.avgWait},
                           {"maxWait", r.metrics.maxWait},
                           {"utilization", r.metrics.utilization},
                           {"makespan", r.metrics.makespan}});
        }
        sendJson(res, {{"policy", policyToString(s.policy)}, {"points", pts}});
    });

    // ---- text-file persistence -------------------------------------------
    svr.Get("/api/files", [](const httplib::Request&, httplib::Response& res) {
        sendJson(res, {{"files", listSavedFiles(g_dataDir)}});
    });

    svr.Post("/api/save", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;
        std::string name = body.value("name", std::string());
        if (name == "autosave") return sendError(res, 400, "'autosave' is reserved; choose another name.");
        std::string err = saveStateToFile(g_dataDir, name, copyState());
        if (!err.empty()) return sendError(res, 400, err);
        sendJson(res, {{"saved", name}, {"files", listSavedFiles(g_dataDir)}});
    });

    svr.Post("/api/load", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;
        LabState loaded;
        std::string err = loadStateFromFile(g_dataDir, body.value("name", std::string()), loaded);
        if (!err.empty()) return sendError(res, 400, err);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_state = std::move(loaded);
        persistLocked();
        sendJson(res, stateToJson(g_state));
    });

    // Download the current setup as a .txt file / import one from the browser.
    svr.Get("/api/export", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Content-Disposition", "attachment; filename=\"lab-schedule.txt\"");
        res.set_content(serializeState(copyState()), "text/plain; charset=utf-8");
    });

    svr.Post("/api/import", [](const httplib::Request& req, httplib::Response& res) {
        LabState loaded;
        if (auto err = parseState(req.body, loaded); !err.empty()) return sendError(res, 400, err);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_state = std::move(loaded);
        persistLocked();
        sendJson(res, stateToJson(g_state));
    });

    // ---- static frontend -------------------------------------------------
    if (!svr.set_mount_point("/", webDir)) {
        std::cerr << "Could not serve folder '" << webDir << "'.\n";
        return 1;
    }
    svr.set_file_request_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
    });

    std::cout << "Computer Lab Resource Scheduler\n";
    if (host == "127.0.0.1")
        std::cout << "  open  http://127.0.0.1:" << port << "  in your browser\n";
    else
        std::cout << "  listening on " << host << ":" << port << "\n";
    std::cout << "  press Ctrl+C to stop\n";
    if (!svr.listen(host, port)) {
        std::cerr << "Could not listen on port " << port << " (is it already in use? try: ./lab_scheduler 8081)\n";
        return 1;
    }
    return 0;
}
