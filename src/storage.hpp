// storage.hpp
// ---------------------------------------------------------------------------
// Plain-text save/load of the lab setup (number of computers, policy, requests).
//
// File format (human-readable, easy to edit in Notepad):
//
//     # Computer Lab Resource Scheduler
//     COMPUTERS 4
//     POLICY FCFS
//     # id  arrival  duration  priority
//     S001  0        30        2
//     S002  0        20        3
//
// Lines starting with '#' and blank lines are ignored.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "scheduler.hpp"

namespace lab {

struct LabState {
    int computers = 4;
    Policy policy = Policy::FCFS;
    std::vector<StudentRequest> requests;
};

inline std::string serializeState(const LabState& s) {
    std::ostringstream out;
    out << "# Computer Lab Resource Scheduler\n";
    out << "COMPUTERS " << s.computers << "\n";
    out << "POLICY " << policyToString(s.policy) << "\n";
    out << "# id  arrival(min)  duration(min)  priority(1=highest)\n";
    for (const auto& r : s.requests)
        out << r.id << " " << r.arrival << " " << r.duration << " " << r.priority << "\n";
    return out.str();
}

// Parses text into `out`. Returns an empty string on success, else an error
// message that includes the offending line number. `out` is only modified on success.
inline std::string parseState(const std::string& text, LabState& out) {
    LabState parsed;
    bool sawComputers = false;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;

    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();  // Windows line endings
        auto first = std::find_if(line.begin(), line.end(), [](unsigned char c) { return !std::isspace(c); });
        if (first == line.end() || *first == '#') continue;

        std::istringstream ls(line);
        std::string head;
        ls >> head;
        std::string upper = head;
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        auto fail = [&](const std::string& msg) {
            return "Line " + std::to_string(lineNo) + ": " + msg;
        };

        if (upper == "COMPUTERS") {
            int n;
            std::string extra;
            if (!(ls >> n) || (ls >> extra)) return fail("expected: COMPUTERS <number>");
            if (auto err = validateComputers(n); !err.empty()) return fail(err);
            parsed.computers = n;
            sawComputers = true;
        } else if (upper == "POLICY") {
            std::string name, extra;
            if (!(ls >> name) || (ls >> extra) || !parsePolicy(name, parsed.policy))
                return fail("expected: POLICY FCFS | SJF | PRIORITY");
        } else {
            StudentRequest r;
            r.id = normalizeId(head);
            std::string extra;
            if (!(ls >> r.arrival >> r.duration >> r.priority) || (ls >> extra))
                return fail("expected: <id> <arrival> <duration> <priority>");
            if (auto err = validateRequest(r, parsed.requests); !err.empty()) return fail(err);
            parsed.requests.push_back(r);
        }
    }
    if (!sawComputers) return "Missing 'COMPUTERS <number>' line.";
    out = std::move(parsed);
    return "";
}

// Only letters, digits, '-' and '_' (1..40 chars) are allowed in a save name, which
// makes path traversal ("../x") impossible.
inline bool isValidSaveName(const std::string& name) {
    if (name.empty() || name.size() > 40) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_';
    });
}

inline std::string savePath(const std::string& dir, const std::string& name) {
    return (std::filesystem::path(dir) / (name + ".txt")).string();
}

inline std::string saveStateToFile(const std::string& dir, const std::string& name, const LabState& s) {
    if (!isValidSaveName(name))
        return "Save name may only contain letters, digits, '-' and '_' (max 40 characters).";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream f(savePath(dir, name), std::ios::trunc);
    if (!f) return "Could not write the file on the server.";
    f << serializeState(s);
    return f ? "" : "Could not write the file on the server.";
}

inline std::string loadStateFromFile(const std::string& dir, const std::string& name, LabState& out) {
    if (!isValidSaveName(name)) return "Invalid save name.";
    std::ifstream f(savePath(dir, name));
    if (!f) return "No saved file called '" + name + "'.";
    std::ostringstream buf;
    buf << f.rdbuf();
    return parseState(buf.str(), out);
}

inline std::vector<std::string> listSavedFiles(const std::string& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return names;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".txt") continue;
        std::string stem = e.path().stem().string();
        if (stem == "autosave" || !isValidSaveName(stem)) continue;
        names.push_back(stem);
    }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace lab
