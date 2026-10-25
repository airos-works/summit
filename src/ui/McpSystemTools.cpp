#if SUMMIT_MODERN_WEBKIT
// MCP tools for what the operating system knows of Summit's processes: CPU,
// threads and what they wait on, memory areas, stacks, the log, crash
// reports and the environment (McpService.h). None of them needs a looper of
// Summit's, so they answer while the browser hangs.
#include "McpService.h"
#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Path.h>
#include <image.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

extern char** environ;

namespace summit {
namespace {
using mcp::Schema;
using mcp::ToolError;
using mcp::ToolResult;

double Round(double value, double scale = 10)
{
    return std::round(value * scale) / scale;
}

double MiB(uint64 bytes)
{
    return Round(double(bytes) / 1048576.0, 100);
}

const char* StateName(thread_state state)
{
    switch (state) {
        case B_THREAD_RUNNING: return "running";
        case B_THREAD_READY: return "ready";
        case B_THREAD_RECEIVING: return "receiving";
        case B_THREAD_ASLEEP: return "asleep";
        case B_THREAD_SUSPENDED: return "suspended";
        case B_THREAD_WAITING: return "waiting";
        default: return "?";
    }
}

struct ThreadTimes {
    thread_info info;
    bigtime_t cpu;
};

std::map<thread_id, ThreadTimes> SampleThreads(team_id team)
{
    std::map<thread_id, ThreadTimes> threads;
    int32 cookie = 0;
    thread_info info;
    while (get_next_thread_info(team, &cookie, &info) == B_OK)
        threads[info.thread] = {info, info.user_time + info.kernel_time};
    return threads;
}

// The team a tool names: "team" (an id), or the web process of "tab", or this browser.
team_id ResolveTeam(const Json& arguments, bool required = false)
{
    if (mcp::HasArgument(arguments, "team")) {
        const team_id team = team_id(mcp::IntegerArgument(arguments, "team", 0, 1, INT32_MAX));
        team_info info;
        if (get_team_info(team, &info) != B_OK) throw ToolError("there is no team " + std::to_string(team));
        return team;
    }
    if (mcp::HasArgument(arguments, "tab")) {
        McpTabRef ref = McpResolveTab(arguments);
        Json request = {{"action", "tabs"}};
        const Json window = McpAsk(ref.window, request, 3000000, "The window");
        for (const auto& tab : window.value("tabs", Json::array())) {
            const bool match = ref.tab ? tab.value("id", "") == ref.id : tab.value("selected", false);
            if (!match) continue;
            const team_id team = tab.value("webProcess", 0);
            if (team <= 0)
                throw ToolError("the engine has not said which web process shows tab " + tab.value("id", ref.id)
                    + " (an engine without process ids in its state, or the page has not loaded): use processes and team");
            return team;
        }
        throw ToolError("no tab " + ref.id);
    }
    if (required) throw ToolError("name a team (see processes) or a tab");
    return getpid();
}

// Area kinds by name, for memory totals that say what the memory is.
std::string AreaKind(const char* name)
{
    const std::string text = name;
    auto has = [&](const char* part) { return text.find(part) != std::string::npos; };
    // Anonymous memory is named for the library that mapped it (names are
    // cut at 31 characters, so "mmap area" may be "mmap ar").
    if (has(" mmap ar")) {
        if (has("JavaScriptCore")) return "JavaScript heap and malloc (mapped by libJavaScriptCore: mimalloc, JSC)";
        if (has("libWebKit")) return "engine allocations (mapped by libWebKit)";
        if (has("vulkan") || has("radeon") || has("radv") || has("zink") || has("Mesa") || has("mesa") || has("libGL")
            || has("EGL") || has("gallium") || has("drm") || has("nvidia") || has("nouveau") || has("LLVM"))
            return "GPU and driver";
        if (has("libroot")) return "malloc heaps";
        return "other mapped memory";
    }
    if (has("_seg")) return "code and libraries";
    if (has("_stack") || has("stack")) return "thread stacks";
    if (has("mimalloc") || has("mi_") || has("heap") || has("malloc") || has("libroot")) return "malloc heaps";
    if (has("JSC") || has("jsc") || has("JIT") || has("Gigacage") || has("bmalloc") || has("WTF")) return "JavaScript heap and JIT";
    if (has("drm") || has("radeon") || has("amdgpu") || has("nvidia") || has("nouveau") || has("intel") || has("gem")
        || has("vram") || has("vulkan") || has("zink") || has("mesa") || has("GPU") || has("gpu") || has("pvr") || has("v3d"))
        return "GPU and driver";
    if (has("bitmap") || has("Bitmap") || has("frame") || has("Frame") || has("shared memory") || has("SharedMemory")
        || has("WebKit shm") || has("shm"))
        return "bitmaps and shared memory";
    if (has("_seg") || has(".so") || has("_text") || has("_data") || has("ro_data") || has("rw_data")) return "code and libraries";
    if (has("server_memory") || has("app_server") || has("rw_lock") || has("port")) return "system";
    return "other";
}

Json TeamMemory(team_id team, bool areas, size_t top, const std::string& pattern)
{
    std::map<std::string, std::pair<uint64, uint64>> kinds;
    struct Area {
        std::string name;
        uint64 ram, size;
        uint32 protection;
        int copies;
    };
    std::vector<Area> list;
    uint64 resident = 0, reserved = 0;
    size_t count = 0;
    ssize_t cookie = 0;
    area_info info;
    while (get_next_area_info(team, &cookie, &info) == B_OK) {
        ++count;
        resident += info.ram_size;
        reserved += info.size;
        auto& kind = kinds[AreaKind(info.name)];
        kind.first += info.ram_size;
        kind.second += info.size;
        if (areas && (pattern.empty() || mcp::Matches(pattern, info.name)))
            list.push_back({info.name, info.ram_size, info.size, info.protection, int(info.copy_count)});
    }
    Json byKind = Json::array();
    std::vector<std::pair<std::string, std::pair<uint64, uint64>>> sorted(kinds.begin(), kinds.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    for (const auto& [kind, sizes] : sorted)
        byKind.push_back({{"kind", kind}, {"residentMiB", MiB(sizes.first)}, {"reservedMiB", MiB(sizes.second)}});
    Json memory = {{"residentMiB", MiB(resident)}, {"reservedMiB", MiB(reserved)}, {"areas", count}, {"byKind", byKind}};
    if (areas) {
        std::sort(list.begin(), list.end(), [](const Area& a, const Area& b) { return a.ram > b.ram; });
        // Areas of the same name (one per thread stack, per heap segment) go together.
        std::map<std::string, std::tuple<uint64, uint64, int>> grouped;
        for (const auto& area : list) {
            std::string name = area.name;
            // Long numbers name instances ("thread 1234_stack"); short ones
            // are versions ("libicuuc.so.78").
            std::string key;
            for (size_t i = 0; i < name.size();) {
                size_t digits = 0;
                while (i + digits < name.size() && std::isdigit(static_cast<unsigned char>(name[i + digits]))) ++digits;
                if (digits >= 3) { key += '#'; i += digits; }
                else if (digits) { key += name.substr(i, digits); i += digits; }
                else key += name[i++];
            }
            name = key;
            auto& group = grouped[name];
            std::get<0>(group) += area.ram;
            std::get<1>(group) += area.size;
            ++std::get<2>(group);
        }
        std::vector<std::pair<std::string, std::tuple<uint64, uint64, int>>> groups(grouped.begin(), grouped.end());
        std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return std::get<0>(a.second) > std::get<0>(b.second); });
        Json largest = Json::array();
        for (size_t i = 0; i < groups.size() && i < top; ++i)
            largest.push_back({{"name", groups[i].first}, {"areas", std::get<2>(groups[i].second)},
                {"residentMiB", MiB(std::get<0>(groups[i].second))}, {"reservedMiB", MiB(std::get<1>(groups[i].second))}});
        memory["largestAreaGroups"] = largest;
    }
    return memory;
}

Json SystemMemory()
{
    system_info system;
    if (get_system_info(&system) != B_OK) return Json::object();
    return {{"totalMiB", MiB(uint64(system.max_pages) * B_PAGE_SIZE)}, {"usedMiB", MiB(uint64(system.used_pages) * B_PAGE_SIZE)},
        {"cachedMiB", MiB(uint64(system.cached_pages) * B_PAGE_SIZE)}, {"blockCacheMiB", MiB(uint64(system.block_cache_pages) * B_PAGE_SIZE)},
        {"neededMiB", MiB(system.needed_memory)}, {"freeMiB", MiB(system.free_memory)},
        {"swapTotalMiB", MiB(uint64(system.max_swap_pages) * B_PAGE_SIZE)}, {"swapFreeMiB", MiB(uint64(system.free_swap_pages) * B_PAGE_SIZE)},
        {"pageFaults", system.page_faults}, {"usedThreads", system.used_threads}, {"usedTeams", system.used_teams},
        {"usedSemaphores", system.used_sems}, {"usedPorts", system.used_ports}};
}

std::string ReadFile(const std::string& path, size_t limit)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return { };
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    const std::streamoff start = size > std::streamoff(limit) ? size - std::streamoff(limit) : 0;
    in.seekg(start);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text;
}

// This Summit's executable, which runs the debugger helper (McpDebugHelper.cpp).
std::string HelperPath()
{
    int32 cookie = 0;
    image_info image;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK)
        if (image.type == B_APP_IMAGE) return image.name;
    throw ToolError("cannot find Summit's executable");
}

// Runs a program and returns what it wrote (stdout and stderr), waiting at most timeout.
std::string Run(const std::vector<std::string>& command, bigtime_t timeout, int& exitStatus)
{
    int fds[2];
    if (pipe(fds) != 0) throw ToolError("no pipe");
    std::vector<char*> argv;
    for (const auto& part : command) argv.push_back(const_cast<char*>(part.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    pid_t child = -1;
    const int spawned = posix_spawn(&child, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (spawned != 0) {
        close(fds[0]);
        throw ToolError("could not start " + command[0] + ": " + std::strerror(spawned));
    }
    std::string output;
    const bigtime_t deadline = system_time() + timeout;
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    bool exited = false;
    char buffer[8192];
    while (system_time() < deadline) {
        const ssize_t got = read(fds[0], buffer, sizeof(buffer));
        if (got > 0) { output.append(buffer, size_t(got)); continue; }
        int status = 0;
        const pid_t done = waitpid(child, &status, WNOHANG);
        // The engine's process reaper may have collected it first.
        if (done == child || (done < 0 && errno == ECHILD)) {
            exitStatus = done == child && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            exited = true;
            ssize_t rest;
            while ((rest = read(fds[0], buffer, sizeof(buffer))) > 0) output.append(buffer, size_t(rest));
            break;
        }
        snooze(20000);
    }
    if (!exited) {
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        exitStatus = -1;
        output += "\n(timed out; stopped)";
    }
    close(fds[0]);
    return output;
}
}

void RegisterMcpSystemTools(mcp::Server& server, const std::shared_ptr<McpContext>&)
{
    server.Add({"processes",
        "Summit's processes (this browser, its web, network and other engine processes) measured over sample_ms "
        "(default 1000): CPU as a share of one core, threads, resident and reserved memory, age, the busiest "
        "threads; plus the system's memory. Tabs name their web process in the tabs tool.",
        Schema().Integer("sample_ms", "Measuring time (100-10000, default 1000)"),
        [](const Json& arguments) {
            const bigtime_t sample = mcp::IntegerArgument(arguments, "sample_ms", 1000, 100, 10000) * 1000;
            const auto teams = McpSummitTeams();
            std::map<team_id, std::map<thread_id, ThreadTimes>> before;
            for (team_id team : teams) before[team] = SampleThreads(team);
            const bigtime_t started = system_time();
            snooze(sample);
            const double elapsed = double(system_time() - started);
            Json list = Json::array();
            double total = 0;
            for (team_id team : teams) {
                team_info info;
                if (get_team_info(team, &info) != B_OK) continue;
                const auto after = SampleThreads(team);
                double cores = 0;
                std::vector<std::pair<double, const thread_info*>> busy;
                for (const auto& [id, now] : after) {
                    auto then = before[team].find(id);
                    const double used = double(now.cpu - (then != before[team].end() ? then->second.cpu : 0)) / elapsed;
                    cores += used;
                    busy.emplace_back(used, &now.info);
                }
                total += cores;
                std::sort(busy.begin(), busy.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                Json threads = Json::array();
                for (size_t i = 0; i < busy.size() && i < 4 && busy[i].first >= 0.01; ++i)
                    threads.push_back({{"thread", busy[i].second->thread}, {"name", busy[i].second->name}, {"cores", Round(busy[i].first, 100)}});
                const Json memory = TeamMemory(team, false, 0, { });
                list.push_back({{"team", team}, {"role", McpTeamRole(team)}, {"args", info.args}, {"cores", Round(cores, 100)},
                    {"threads", info.thread_count}, {"areas", memory["areas"]}, {"images", info.image_count},
                    {"residentMiB", memory["residentMiB"]}, {"reservedMiB", memory["reservedMiB"]},
                    {"ageSeconds", Round(double(system_time() - info.start_time) / 1e6)}, {"busiestThreads", threads}});
            }
            return ToolResult::Data({{"sampleMs", sample / 1000}, {"totalCores", Round(total, 100)}, {"processes", list},
                {"system", SystemMemory()}});
        }});

    server.Add({"threads",
        "The threads of a team (default this browser; a tab names its web process): state, priority, CPU used "
        "during sample_ms as a share of a core, total CPU, and for waiting threads the semaphore and its owner. "
        "Threads waiting on the same semaphore are counted together: a main thread that waits while others hold "
        "what it needs is how a hang looks. Filter names with a glob.",
        Schema().Integer("team", "Team id (see processes)").String("tab", "A tab, for its web process")
            .Integer("sample_ms", "Measuring time (0-10000, default 500)").String("filter", "Glob on thread names")
            .Integer("limit", "Most threads listed (default 80), busiest first"),
        [](const Json& arguments) {
            const team_id team = ResolveTeam(arguments);
            const bigtime_t sample = mcp::IntegerArgument(arguments, "sample_ms", 500, 0, 10000) * 1000;
            const std::string filter = mcp::StringArgument(arguments, "filter");
            const size_t limit = size_t(mcp::IntegerArgument(arguments, "limit", 80, 1, 2000));
            const auto before = SampleThreads(team);
            const bigtime_t started = system_time();
            if (sample) snooze(sample);
            const double elapsed = std::max(1.0, double(system_time() - started));
            const auto after = SampleThreads(team);
            struct Row {
                double cores;
                Json entry;
            };
            std::vector<Row> rows;
            std::map<std::string, int> waits;
            std::map<std::string, int> states;
            for (const auto& [id, now] : after) {
                const thread_info& info = now.info;
                if (!filter.empty() && !mcp::Matches(filter, info.name)) continue;
                auto then = before.find(id);
                const double cores = sample ? double(now.cpu - (then != before.end() ? then->second.cpu : now.cpu)) / elapsed : 0;
                Json entry = {{"thread", id}, {"name", info.name}, {"state", StateName(info.state)}, {"priority", info.priority},
                    {"cpuSeconds", Round(double(now.cpu) / 1e6, 1000)}, {"userSeconds", Round(double(info.user_time) / 1e6, 1000)},
                    {"kernelSeconds", Round(double(info.kernel_time) / 1e6, 1000)}};
                if (sample) entry["cores"] = Round(cores, 1000);
                ++states[StateName(info.state)];
                if (info.sem < 0 && info.state == B_THREAD_WAITING)
                    entry["waitsOn"] = "a mutex, condition variable or other wait without a semaphore (see stacks)";
                if (info.sem >= 0 && info.state != B_THREAD_RUNNING && info.state != B_THREAD_READY) {
                    sem_info sem;
                    if (get_sem_info(info.sem, &sem) == B_OK) {
                        entry["waitsOn"] = {{"semaphore", sem.sem}, {"name", sem.name}, {"count", sem.count},
                            {"ownerTeam", sem.team}, {"latestHolder", sem.latest_holder}};
                        ++waits[std::string(sem.name) + " (" + std::to_string(sem.sem) + ")"];
                    } else entry["waitsOn"] = {{"semaphore", info.sem}};
                }
                rows.push_back({cores, entry});
            }
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
                return a.cores != b.cores ? a.cores > b.cores : a.entry["cpuSeconds"].get<double>() > b.entry["cpuSeconds"].get<double>();
            });
            Json threads = Json::array();
            for (size_t i = 0; i < rows.size() && i < limit; ++i) threads.push_back(rows[i].entry);
            Json shared = Json::array();
            for (const auto& [name, count] : waits) if (count > 1) shared.push_back({{"semaphore", name}, {"threads", count}});
            team_info info;
            get_team_info(team, &info);
            return ToolResult::Data({{"team", team}, {"role", McpTeamRole(team)}, {"threadCount", info.thread_count},
                {"states", states}, {"sampleMs", sample / 1000}, {"threads", threads}, {"sharedWaits", shared}});
        }});

    server.Add({"memory",
        "Memory of a team (default this browser; tab: its web process; all=true: every Summit process) from its "
        "areas: resident and reserved totals by kind (malloc heaps, JavaScript heap and JIT, GPU and driver, "
        "bitmaps and shared memory, stacks, code), the largest area groups, and the system's memory. The engine's "
        "own accounting (JS heap objects, caches) is in engine_report topic memory.",
        Schema().Integer("team", "Team id").String("tab", "A tab, for its web process").Boolean("all", "Every Summit process")
            .Integer("top", "Area groups listed (default 25)").String("filter", "Glob on area names"),
        [](const Json& arguments) {
            const size_t top = size_t(mcp::IntegerArgument(arguments, "top", 25, 1, 500));
            const std::string filter = mcp::StringArgument(arguments, "filter");
            Json result = {{"system", SystemMemory()}};
            if (mcp::BooleanArgument(arguments, "all", false)) {
                Json teams = Json::array();
                for (team_id team : McpSummitTeams()) {
                    Json memory = TeamMemory(team, true, std::min<size_t>(top, 8), filter);
                    memory["team"] = team;
                    memory["role"] = McpTeamRole(team);
                    teams.push_back(memory);
                }
                result["processes"] = teams;
            } else {
                const team_id team = ResolveTeam(arguments);
                result["team"] = team;
                result["role"] = McpTeamRole(team);
                result["memory"] = TeamMemory(team, true, top, filter);
            }
            return ToolResult::Data(result);
        }});

    server.Add({"cpu",
        "The whole machine over sample_ms (default 1000): each CPU's load and clock, and the teams using the "
        "most CPU (Summit's and others: a busy app_server or media_addon_server shows here).",
        Schema().Integer("sample_ms", "Measuring time (100-10000, default 1000)").Integer("top", "Teams listed (default 12)"),
        [](const Json& arguments) {
            const bigtime_t sample = mcp::IntegerArgument(arguments, "sample_ms", 1000, 100, 10000) * 1000;
            const size_t top = size_t(mcp::IntegerArgument(arguments, "top", 12, 1, 200));
            system_info system;
            get_system_info(&system);
            std::vector<cpu_info> cpusBefore(system.cpu_count), cpusAfter(system.cpu_count);
            get_cpu_info(0, system.cpu_count, cpusBefore.data());
            std::map<team_id, std::pair<std::string, bigtime_t>> teamsBefore;
            auto teamTimes = [](std::map<team_id, std::pair<std::string, bigtime_t>>& out) {
                int32 cookie = 0;
                team_info info;
                while (get_next_team_info(&cookie, &info) == B_OK) {
                    team_usage_info usage;
                    if (get_team_usage_info(info.team, B_TEAM_USAGE_SELF, &usage) == B_OK)
                        out[info.team] = {info.args, usage.user_time + usage.kernel_time};
                }
            };
            teamTimes(teamsBefore);
            const bigtime_t started = system_time();
            snooze(sample);
            const double elapsed = double(system_time() - started);
            get_cpu_info(0, system.cpu_count, cpusAfter.data());
            std::map<team_id, std::pair<std::string, bigtime_t>> teamsAfter;
            teamTimes(teamsAfter);
            Json cpus = Json::array();
            double load = 0;
            for (uint32 i = 0; i < system.cpu_count; ++i) {
                const double used = double(cpusAfter[i].active_time - cpusBefore[i].active_time) / elapsed;
                load += used;
                cpus.push_back({{"cpu", i}, {"load", Round(used, 100)}, {"enabled", cpusAfter[i].enabled},
                    {"mhz", cpusAfter[i].current_frequency / 1000000}});
            }
            std::vector<std::pair<double, Json>> teams;
            const auto summit = McpSummitTeams();
            for (const auto& [team, now] : teamsAfter) {
                auto then = teamsBefore.find(team);
                if (then == teamsBefore.end()) continue;
                const double cores = double(now.second - then->second.second) / elapsed;
                // The kernel team's time includes the idle threads.
                if (cores < 0.005 || team == 1) continue;
                const bool ours = std::find(summit.begin(), summit.end(), team) != summit.end();
                teams.emplace_back(cores, Json{{"team", team}, {"args", now.first}, {"cores", Round(cores, 100)}, {"summit", ours}});
            }
            std::sort(teams.begin(), teams.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            Json busiest = Json::array();
            for (size_t i = 0; i < teams.size() && i < top; ++i) busiest.push_back(teams[i].second);
            return ToolResult::Data({{"sampleMs", sample / 1000}, {"cpuCount", system.cpu_count}, {"totalLoad", Round(load, 100)},
                {"cpus", cpus}, {"busiestTeams", busiest}});
        }});

    server.Add({"stacks",
        "Stack traces of the threads of a team (a tab's web process, a team id, or this browser), read by a short "
        "debugger process that stops each thread for an instant and lets it go: the frame-pointer chain (exact "
        "where the code keeps frame pointers) and the code addresses found on the stack (the callers where it "
        "does not), named from the libraries' exported symbols (a large +offset means a function that is not "
        "exported). With the threads tool's waits, the way to see what a hung process is doing.",
        Schema().Integer("team", "Team id (default this browser)").String("tab", "A tab, for its web process")
            .String("filter", "Glob on thread names (default all; \"WebProcess\" or \"Summit\" for the main thread)")
            .Integer("depth", "Frames per thread (default 24)").Boolean("scan", "Also list code addresses found on the stack (default true)")
            .Integer("max_bytes", "Most text returned (default 120000)"),
        [](const Json& arguments) {
            const team_id team = ResolveTeam(arguments);
            const std::string filter = mcp::StringArgument(arguments, "filter");
            std::vector<std::string> command = {HelperPath(), "--mcp-debug-helper", "stacks", std::to_string(team),
                std::to_string(mcp::IntegerArgument(arguments, "depth", 24, 1, 256))};
            if (!filter.empty()) {
                int32 cookie = 0;
                thread_info info;
                bool any = false;
                while (get_next_thread_info(team, &cookie, &info) == B_OK)
                    if (mcp::Matches(filter, info.name)) {
                        command.push_back(std::to_string(info.thread));
                        any = true;
                    }
                if (!any) throw ToolError("no thread of team " + std::to_string(team) + " matches " + filter);
            }
            const std::string role = McpTeamRole(team);
            int status = 0;
            const std::string output = Run(command, 30000000, status);
            const Json result = Json::parse(output.substr(0, output.find_last_of('}') + 1), nullptr, false);
            if (!result.is_object()) throw ToolError("the debugger helper failed (exit " + std::to_string(status) + "): " + mcp::Truncate(output, 2000));
            if (result.contains("error")) throw ToolError(result["error"].get<std::string>());
            const bool scan = mcp::BooleanArgument(arguments, "scan", true);
            std::string text = "Stacks of team " + std::to_string(team) + " (" + role + "):\n";
            for (const auto& thread : result.value("threads", Json::array())) {
                text += "\nthread " + std::to_string(thread.value("thread", 0)) + " \"" + thread.value("name", "") + "\" "
                    + thread.value("state", "");
                if (thread.contains("waitsOn")) text += ", waits on semaphore \"" + thread["waitsOn"].value("name", "") + "\"";
                text += "\n";
                if (thread.contains("error")) { text += "  " + thread["error"].get<std::string>() + "\n"; continue; }
                for (const auto& frame : thread.value("frames", Json::array())) text += "  " + frame.get<std::string>() + "\n";
                if (scan && !thread.value("stackScan", Json::array()).empty()) {
                    text += "  stack scan:\n";
                    for (const auto& frame : thread["stackScan"]) text += "    " + frame.get<std::string>() + "\n";
                }
            }
            return ToolResult::Text(mcp::Truncate(text, size_t(mcp::IntegerArgument(arguments, "max_bytes", 120000, 1000, 16 << 20))));
        }});

    server.Add({"profile",
        "Samples where a team's threads run for some seconds (default 3) with the kernel's profiler, from a "
        "short debugger process, and says where the CPU goes: functions by samples where they were running "
        "(self), functions anywhere on the sampled stacks (inclusive, with depth > 1), libraries, and each busy "
        "thread with its top functions. Works on this browser's own team too. Use with processes/threads to "
        "find which team and thread is busy first; names come from exported symbols. JavaScript and other JIT code "
        "is named \"name (JIT)\" when the process writes JITDump files: set_environment JSC_useJITDump=1 and "
        "JSC_jitDumpDirectory=/tmp, then restart_process the tab's web process.",
        Schema().Integer("team", "Team id (default this browser)").String("tab", "A tab, for its web process")
            .Number("seconds", "Sampling time (0.1-60, default 3)").Integer("interval_us", "Sampling interval (default 1000)")
            .Integer("depth", "Return addresses per sample (1-64, default 12)").String("filter", "Glob on thread names (default all)"),
        [](const Json& arguments) {
            const team_id team = ResolveTeam(arguments);
            const double seconds = std::clamp(mcp::NumberArgument(arguments, "seconds", 3), 0.1, 60.0);
            std::vector<std::string> command = {HelperPath(), "--mcp-debug-helper", "profile", std::to_string(team),
                std::to_string(seconds), std::to_string(mcp::IntegerArgument(arguments, "interval_us", 1000, 100, 1000000)),
                std::to_string(mcp::IntegerArgument(arguments, "depth", 12, 1, 64))};
            const std::string filter = mcp::StringArgument(arguments, "filter");
            if (!filter.empty()) {
                int32 cookie = 0;
                thread_info info;
                while (get_next_thread_info(team, &cookie, &info) == B_OK)
                    if (mcp::Matches(filter, info.name)) command.push_back(std::to_string(info.thread));
                if (command.size() == 7) throw ToolError("no thread of team " + std::to_string(team) + " matches " + filter);
            }
            int status = 0;
            const std::string output = Run(command, bigtime_t(seconds * 1e6) + 60000000, status);
            Json result = Json::parse(output.substr(0, output.find_last_of('}') + 1), nullptr, false);
            if (!result.is_object()) throw ToolError("the debugger helper failed (exit " + std::to_string(status) + "): " + mcp::Truncate(output, 2000));
            if (result.contains("error")) throw ToolError(result["error"].get<std::string>());
            result["role"] = McpTeamRole(team);
            return ToolResult::Data(result);
        }});

    server.Add({"logs",
        "Lines Summit and its engine processes wrote to stderr since the MCP server started (traces, warnings, "
        "assertion messages, SUMMIT_* trace output), newest last. since: a sequence number from an earlier call "
        "(returned as last) to get only what is new; filter: a glob or text to look for.",
        Schema().Integer("since", "Only lines after this sequence number").String("filter", "Text or glob to match")
            .Integer("limit", "Most lines returned (default 300)"),
        [](const Json& arguments) {
            uint64 last = 0;
            const auto lines = mcplog::Read(uint64(mcp::IntegerArgument(arguments, "since", 0, 0)), mcp::StringArgument(arguments, "filter"),
                size_t(mcp::IntegerArgument(arguments, "limit", 300, 1, 20000)), last);
            std::string text;
            const bigtime_t now = system_time();
            for (const auto& line : lines) {
                char prefix[48];
                std::snprintf(prefix, sizeof(prefix), "[%llu -%.1fs] ", static_cast<unsigned long long>(line.sequence),
                    double(now - line.when) / 1e6);
                text += prefix + line.text + "\n";
            }
            if (!mcplog::Capturing()) text = "The log is not being captured.\n";
            else if (lines.empty()) text = "No matching lines.\n";
            return ToolResult::Text(mcp::Truncate(text, 400000) + "last=" + std::to_string(last) + " (pass as since for newer lines)");
        }});

    server.Add({"syslog",
        "The end of the system log (/var/log/syslog): kernel and driver messages (GPU, USB, audio), debug_server "
        "crash notes, app_server. filter: text or glob per line.",
        Schema().String("filter", "Text or glob to match").Integer("lines", "Most lines (default 200)"),
        [](const Json& arguments) {
            const std::string text = ReadFile("/var/log/syslog", 4 * 1024 * 1024);
            std::string pattern = mcp::StringArgument(arguments, "filter");
            if (!pattern.empty() && pattern.find('*') == std::string::npos) pattern = "*" + pattern + "*";
            std::vector<std::string> lines;
            std::istringstream in(text);
            std::string line;
            while (std::getline(in, line)) if (pattern.empty() || mcp::Matches(pattern, line)) lines.push_back(line);
            const size_t count = size_t(mcp::IntegerArgument(arguments, "lines", 200, 1, 20000));
            std::string out;
            for (size_t i = lines.size() > count ? lines.size() - count : 0; i < lines.size(); ++i) out += lines[i] + "\n";
            return ToolResult::Text(out.empty() ? "No matching lines." : mcp::Truncate(out, 400000));
        }});

    server.Add({"crash_reports",
        "Crash reports of Summit's processes (Summit, WebProcess, NetworkProcess) that debug_server saved on the "
        "Desktop, newest first; with name, the report itself.",
        Schema().String("name", "A report's file name to read").Integer("max_bytes", "Most of a report returned (default 200000)"),
        [](const Json& arguments) {
            BPath desktop;
            find_directory(B_DESKTOP_DIRECTORY, &desktop);
            const std::string name = mcp::StringArgument(arguments, "name");
            if (!name.empty()) {
                if (name.find('/') != std::string::npos) throw ToolError("a file name, not a path");
                const std::string report = ReadFile(std::string(desktop.Path()) + "/" + name, 32 * 1024 * 1024);
                if (report.empty()) throw ToolError("no report " + name + " on the Desktop");
                return ToolResult::Text(mcp::Truncate(report, size_t(mcp::IntegerArgument(arguments, "max_bytes", 200000, 1000, 16 << 20))));
            }
            BDirectory folder(desktop.Path());
            BEntry entry;
            std::vector<std::pair<time_t, Json>> reports;
            while (folder.GetNextEntry(&entry) == B_OK) {
                char file[B_FILE_NAME_LENGTH];
                entry.GetName(file);
                const std::string text = file;
                if (text.size() < 7 || text.substr(text.size() - 7) != ".report") continue;
                if (text.find("Summit") == std::string::npos && text.find("WebProcess") == std::string::npos
                    && text.find("NetworkProcess") == std::string::npos && text.find("summit") == std::string::npos) continue;
                time_t modified = 0;
                entry.GetModificationTime(&modified);
                off_t size = 0;
                entry.GetSize(&size);
                reports.emplace_back(modified, Json{{"name", text}, {"modified", modified}, {"bytes", size}});
            }
            std::sort(reports.begin(), reports.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            Json list = Json::array();
            for (const auto& [time, report] : reports) list.push_back(report);
            return ToolResult::Data({{"folder", desktop.Path()}, {"reports", list}});
        }});

    server.Add({"environment",
        "Summit's environment variables for tracing, graphics and the engine (SUMMIT_*, WEBKIT_*, JSC_*, Mesa, "
        "zink, EGL…), or those matching a glob. Engine processes started from now on get what set_environment "
        "changed.",
        Schema().String("filter", "Glob on variable names (default: the trace and graphics ones)"),
        [](const Json& arguments) {
            Json variables = Json::object();
            const std::string pattern = mcp::StringArgument(arguments, "filter");
            static const char* const prefixes[] = { "SUMMIT_", "WEBKIT_", "JSC_", "MESA_", "GALLIUM_", "ZINK_", "LIBGL_", "__EGL_", "EGL_", "VK_" };
            for (char** entry = environ; entry && *entry; ++entry) {
                const std::string text = *entry;
                const size_t equals = text.find('=');
                if (equals == std::string::npos) continue;
                const std::string name = text.substr(0, equals);
                bool wanted = !pattern.empty() && mcp::Matches(pattern, name);
                if (pattern.empty()) for (const char* prefix : prefixes) wanted = wanted || name.rfind(prefix, 0) == 0;
                const bool secret = name.find("TOKEN") != std::string::npos || name.find("SECRET") != std::string::npos
                    || name.find("PASSWORD") != std::string::npos;
                if (wanted) variables[name] = secret ? std::string("(hidden)") : text.substr(equals + 1);
            }
            return ToolResult::Data({{"variables", variables}, {"note",
                "Most SUMMIT_* switches are read once when a process starts: set them with set_environment, then "
                "restart_process (a tab's web process) or open the page in a new tab."}});
        }});

    server.Add({"set_environment",
        "Sets (or with an empty value removes) an environment variable in Summit, inherited by engine processes "
        "started afterwards: SUMMIT_* traces (SUMMIT_COMPOSITOR_TIMING_TRACE=1, SUMMIT_MSE_TRACE=1, "
        "SUMMIT_MEDIA_PLAYBACK_TRACE=1, SUMMIT_NET_TRACE=1, SUMMIT_DAMAGE_TRACE=1…), Mesa or JSC options. Then "
        "restart_process the tab and read logs.",
        Schema().String("name", "Variable name", true).String("value", "Value; empty removes it"),
        [](const Json& arguments) {
            const std::string name = mcp::StringArgument(arguments, "name");
            if (name.empty() || name.find('=') != std::string::npos) throw ToolError("not a variable name");
            const std::string value = mcp::StringArgument(arguments, "value");
            const char* previous = std::getenv(name.c_str());
            const std::string before = previous ? previous : "";
            if (value.empty()) unsetenv(name.c_str());
            else setenv(name.c_str(), value.c_str(), 1);
            mcplog::Note("Summit MCP: environment " + name + "=" + value);
            return ToolResult::Data({{"name", name}, {"value", value}, {"previous", before},
                {"appliesTo", "engine processes started from now on (restart_process a tab's web process)"}});
        }, false});

    server.Add({"restart_process",
        "Ends a web process (a tab's, or a team id of Summit's) so the engine starts it again: its tabs show the "
        "page process as gone and reload. Picks up set_environment changes; also the way out of a hung page.",
        Schema().String("tab", "A tab, for its web process").Integer("team", "A web process team id"),
        [](const Json& arguments) {
            const team_id team = ResolveTeam(arguments, true);
            const auto teams = McpSummitTeams();
            if (team == getpid() || std::find(teams.begin(), teams.end(), team) == teams.end())
                throw ToolError("team " + std::to_string(team) + " is not one of this Summit's engine processes");
            const std::string role = McpTeamRole(team);
            // The tabs it shows, to load again in a new process.
            std::vector<std::pair<BMessenger, int64>> shown;
            Json reloaded = Json::array();
            for (const auto& window : McpWindows()) {
                try {
                    const Json state = McpAsk(window.messenger, {{"action", "tabs"}}, 2000000, "A window");
                    for (const auto& tab : state.value("tabs", Json::array())) {
                        if (tab.value("webProcess", 0) != team) continue;
                        const std::string id = tab.value("id", "");
                        shown.emplace_back(window.messenger, std::stoll(id.substr(id.find('.') + 1)));
                        reloaded.push_back(id);
                    }
                } catch (const std::exception&) {
                }
            }
            const status_t status = kill_team(team);
            if (status != B_OK) throw ToolError(std::string("could not end it: ") + std::strerror(status));
            mcplog::Note("Summit MCP: ended team " + std::to_string(team) + " (" + role + ")");
            // The views hear of the exit first; a reload before that would go
            // to the dying process.
            team_info gone;
            for (int i = 0; i < 50 && get_team_info(team, &gone) == B_OK; ++i) snooze(20000);
            snooze(300000);
            for (const auto& [window, tab] : shown) {
                try {
                    McpAsk(window, {{"action", "reload"}, {"tab", tab}}, 2000000, "A window");
                } catch (const std::exception&) {
                }
            }
            return ToolResult::Data({{"ended", team}, {"role", role}, {"reloadedTabs", reloaded},
                {"note", role == "web" ? "Its tabs load again in a new web process (see tabs for the new team)."
                    : "The engine starts a new one when it is needed."}});
        }, true, true});
}
}
#endif
