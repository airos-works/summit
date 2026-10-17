#if SUMMIT_MODERN_WEBKIT
// Summit started as "Summit --mcp-debug-helper ...": a short-lived debugger
// that the MCP server's stacks and profile tools run (McpSystemTools.cpp). It
// attaches to one team, reads every thread's stack or samples where the
// threads run, detaches and prints JSON. A separate process can debug the
// browser's own team as well as its engine processes, and nothing stays
// attached when it is done. The debugger API and libdebug (symbols, stack
// frames) are Haiku's; libdebug is opened at run time, as on a stock system.
#include <OS.h>
#include <debugger.h>
#include <image.h>
#include <cxxabi.h>
#include <dlfcn.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <type_traits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
using nlohmann::json;

// libdebug's debug_support.h, which is a private header.
struct DebugContext {
    team_id team;
    port_id nubPort;
    port_id replyPort;
};
struct StackFrameInfo {
    void* frame;
    void* parentFrame;
    void* returnAddress;
};
struct SymbolLookup;

struct LibDebug {
    status_t (*initContext)(DebugContext*, team_id, port_id) = nullptr;
    void (*destroyContext)(DebugContext*) = nullptr;
    status_t (*sendMessage)(DebugContext*, int32, const void*, int32, void*, int32) = nullptr;
    ssize_t (*readMemoryPartial)(DebugContext*, const void*, void*, size_t) = nullptr;
    status_t (*getCPUState)(DebugContext*, thread_id, debug_debugger_message*, debug_cpu_state*) = nullptr;
    status_t (*getStackFrame)(DebugContext*, void*, StackFrameInfo*) = nullptr;
    status_t (*createLookup)(DebugContext*, image_id, SymbolLookup**) = nullptr;
    void (*deleteLookup)(SymbolLookup*) = nullptr;
    status_t (*lookupAddress)(SymbolLookup*, const void*, void**, char*, int32, char*, int32, bool*) = nullptr;

    bool Load(std::string& error)
    {
        void* library = dlopen("libdebug.so", RTLD_NOW);
        if (!library) {
            error = std::string("libdebug.so: ") + dlerror();
            return false;
        }
        auto find = [&](auto& pointer, const char* name) {
            pointer = reinterpret_cast<std::remove_reference_t<decltype(pointer)>>(dlsym(library, name));
            if (!pointer) error += std::string(error.empty() ? "" : ", ") + name;
        };
        find(initContext, "init_debug_context");
        find(destroyContext, "destroy_debug_context");
        find(sendMessage, "send_debug_message");
        find(readMemoryPartial, "debug_read_memory_partial");
        find(getCPUState, "debug_get_cpu_state");
        find(getStackFrame, "debug_get_stack_frame");
        find(createLookup, "debug_create_symbol_lookup_context");
        find(deleteLookup, "debug_delete_symbol_lookup_context");
        find(lookupAddress, "debug_lookup_symbol_address");
        if (!error.empty()) error = "libdebug.so lacks " + error;
        return error.empty();
    }
};

LibDebug sDebug;

void ContinueThread(port_id nub, thread_id thread)
{
    debug_nub_continue_thread message {};
    message.thread = thread;
    message.handle_event = B_THREAD_DEBUG_HANDLE_EVENT;
    message.single_step = false;
    write_port(nub, B_DEBUG_MESSAGE_CONTINUE_THREAD, &message, sizeof(message));
}

std::string Demangle(const char* name)
{
    int status = 0;
    char* demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    if (!demangled || status) return name;
    std::string text = demangled;
    std::free(demangled);
    return text;
}

// Names addresses: "function+0x12 (libWebKit.so.1)", remembered per address.
class Symbols {
public:
    Symbols(DebugContext* context, team_id team)
    {
        if (sDebug.createLookup(context, -1, &fLookup) != B_OK) fLookup = nullptr;
        int32 cookie = 0;
        image_info image;
        while (get_next_image_info(team, &cookie, &image) == B_OK)
            fText.push_back({addr_t(image.text), addr_t(image.text) + addr_t(image.text_size)});
    }
    ~Symbols()
    {
        if (fLookup) sDebug.deleteLookup(fLookup);
    }
    bool InText(addr_t address) const
    {
        for (const auto& [start, end] : fText)
            if (address >= start && address < end) return true;
        return false;
    }
    const std::string& Name(addr_t address)
    {
        auto found = fNames.find(address);
        if (found != fNames.end()) return found->second;
        std::string text;
        char symbol[1024] = "", image[B_PATH_NAME_LENGTH] = "";
        void* base = nullptr;
        bool exact = false;
        if (fLookup && sDebug.lookupAddress(fLookup, reinterpret_cast<void*>(address), &base, symbol, sizeof(symbol), image,
                sizeof(image), &exact) == B_OK) {
            const char* slash = std::strrchr(image, '/');
            char offset[32];
            std::snprintf(offset, sizeof(offset), "+0x%lx", static_cast<unsigned long>(address - addr_t(base)));
            text = (*symbol ? Demangle(symbol) : std::string("?")) + (base ? offset : "") + " (" + (slash ? slash + 1 : image) + ")";
        } else {
            char hex[32];
            std::snprintf(hex, sizeof(hex), "0x%lx", static_cast<unsigned long>(address));
            text = hex;
        }
        return fNames[address] = text;
    }
    // The function alone, for counting samples.
    std::string Function(addr_t address)
    {
        std::string name = Name(address);
        const size_t plus = name.rfind('+');
        const size_t paren = name.rfind(" (");
        if (plus != std::string::npos && paren != std::string::npos && plus < paren) name.erase(plus, paren - plus);
        return name;
    }
    std::string Image(addr_t address)
    {
        const std::string& name = Name(address);
        const size_t paren = name.rfind(" (");
        return paren == std::string::npos ? std::string("?") : name.substr(paren + 2, name.size() - paren - 3);
    }

private:
    SymbolLookup* fLookup = nullptr;
    std::vector<std::pair<addr_t, addr_t>> fText;
    std::map<addr_t, std::string> fNames;
};

struct Attachment {
    team_id team = -1;
    port_id port = -1;
    port_id nub = -1;
    DebugContext context {};
    bool contextReady = false;

    std::string Attach(team_id target)
    {
        team = target;
        port = create_port(256, "summit mcp debugger");
        if (port < 0) return "no port";
        nub = install_team_debugger(team, port);
        if (nub < 0)
            return std::string("cannot debug team ") + std::to_string(team) + ": " + std::strerror(nub)
                + (nub == B_NOT_ALLOWED ? " (another debugger is attached)" : "");
        if (sDebug.initContext(&context, team, nub) != B_OK) return "no debug context";
        contextReady = true;
        return { };
    }
    ~Attachment()
    {
        if (contextReady) sDebug.destroyContext(&context);
        if (nub >= 0) remove_team_debugger(team);
        if (port >= 0) delete_port(port);
    }
};

std::vector<thread_info> Threads(team_id team, const std::set<thread_id>& only)
{
    std::vector<thread_info> threads;
    int32 cookie = 0;
    thread_info info;
    while (get_next_thread_info(team, &cookie, &info) == B_OK)
        if (only.empty() || only.count(info.thread)) threads.push_back(info);
    return threads;
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

int Stacks(team_id team, const std::set<thread_id>& only, int depth)
{
    Attachment attachment;
    if (std::string error = attachment.Attach(team); !error.empty()) {
        std::printf("%s\n", json{{"error", error}}.dump().c_str());
        return 1;
    }
    // What each thread was doing before it was stopped.
    const auto threads = Threads(team, only);
    std::map<thread_id, thread_info> before;
    for (const auto& info : threads) before[info.thread] = info;
    for (const auto& info : threads) debug_thread(info.thread);
    std::set<thread_id> stopped;
    const bigtime_t deadline = system_time() + 3000000;
    while (stopped.size() < threads.size() && system_time() < deadline) {
        debug_debugger_message_data message;
        int32 code = 0;
        const ssize_t size = read_port_etc(attachment.port, &code, &message, sizeof(message), B_ABSOLUTE_TIMEOUT, deadline);
        if (size < 0) {
            if (size == B_INTERRUPTED) continue;
            break;
        }
        if (code == B_DEBUGGER_MESSAGE_THREAD_DEBUGGED) {
            stopped.insert(message.origin.thread);
            continue;
        }
        // Anything else that stopped a thread goes on as it would have.
        if (message.origin.thread >= 0 && message.origin.nub_port >= 0) ContinueThread(attachment.nub, message.origin.thread);
    }
    Symbols symbols(&attachment.context, team);
    json list = json::array();
    for (const auto& info : threads) {
        const thread_info& was = before[info.thread];
        json entry = {{"thread", info.thread}, {"name", info.name}, {"state", StateName(was.state)}, {"priority", was.priority}};
        if (was.sem >= 0) {
            sem_info sem;
            if (get_sem_info(was.sem, &sem) == B_OK) entry["waitsOn"] = {{"semaphore", sem.sem}, {"name", sem.name}, {"ownerTeam", sem.team}};
        }
        if (!stopped.count(info.thread)) {
            entry["error"] = "did not stop within 3 s (an uninterruptible wait in the kernel?)";
            list.push_back(entry);
            continue;
        }
        debug_debugger_message code;
        debug_cpu_state cpu;
        if (sDebug.getCPUState(&attachment.context, info.thread, &code, &cpu) != B_OK) {
            entry["error"] = "no CPU state";
            list.push_back(entry);
            continue;
        }
#if defined(__x86_64__)
        const addr_t ip = cpu.rip, framePointer = cpu.rbp, stackPointer = cpu.rsp;
#elif defined(__aarch64__)
        const addr_t ip = cpu.elr, framePointer = cpu.x[29], stackPointer = cpu.sp;
#else
        const addr_t ip = 0, framePointer = 0, stackPointer = 0;
#endif
        // The frame pointer chain: exact where the code keeps frame pointers.
        json frames = json::array();
        frames.push_back(symbols.Name(ip));
        addr_t frame = framePointer;
        for (int i = 0; i < depth && frame; ++i) {
            StackFrameInfo frameInfo {};
            if (sDebug.getStackFrame(&attachment.context, reinterpret_cast<void*>(frame), &frameInfo) != B_OK
                || !frameInfo.returnAddress)
                break;
            frames.push_back(symbols.Name(addr_t(frameInfo.returnAddress)));
            if (addr_t(frameInfo.parentFrame) <= frame) break;
            frame = addr_t(frameInfo.parentFrame);
        }
        entry["frames"] = frames;
        // Code addresses on the stack, nearest first: where frame pointers
        // are left out (most optimized C++), these show the callers.
        std::vector<addr_t> words(4096);
        const ssize_t got = sDebug.readMemoryPartial(&attachment.context, reinterpret_cast<void*>(stackPointer), words.data(),
            words.size() * sizeof(addr_t));
        json scanned = json::array();
        if (got > 0) {
            addr_t last = 0;
            for (size_t i = 0; i < size_t(got) / sizeof(addr_t) && scanned.size() < size_t(depth); ++i) {
                if (!symbols.InText(words[i]) || words[i] == last) continue;
                last = words[i];
                scanned.push_back(symbols.Name(words[i]));
            }
        }
        entry["stackScan"] = scanned;
        list.push_back(entry);
    }
    for (thread_id thread : stopped) ContinueThread(attachment.nub, thread);
    std::printf("%s\n", json{{"team", team}, {"threads", list}}.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
    return 0;
}

int Profile(team_id team, double seconds, bigtime_t interval, int depth, const std::set<thread_id>& only)
{
    Attachment attachment;
    if (std::string error = attachment.Attach(team); !error.empty()) {
        std::printf("%s\n", json{{"error", error}}.dump().c_str());
        return 1;
    }
    struct Sampled {
        std::string name;
        area_id area = -1;
        addr_t* samples = nullptr;
        bigtime_t startCPU = 0, endCPU = 0;
        uint64 ticks = 0, dropped = 0;
        bool running = false;
    };
    const size_t areaSize = 1024 * 1024;
    std::map<thread_id, Sampled> threads;
    std::map<std::string, uint64> self, inclusive, images;
    std::map<thread_id, std::map<std::string, uint64>> selfByThread;
    Symbols symbols(&attachment.context, team);
    uint64 totalTicks = 0;
    auto take = [&](thread_id thread, int32 count, int32 stackDepth, bool variable) {
        auto found = threads.find(thread);
        if (found == threads.end() || !found->second.samples) return;
        addr_t* samples = found->second.samples;
        int32 i = 0;
        while (i < count) {
            int32 n = variable ? int32(samples[i++]) : stackDepth;
            if (n < 0 || i + n > count) break;
            if (n > 0) {
                ++found->second.ticks;
                ++totalTicks;
                const std::string top = symbols.Function(samples[i]);
                ++self[top];
                ++selfByThread[thread][top];
                ++images[symbols.Image(samples[i])];
                std::set<std::string> seen;
                for (int32 k = 0; k < n; ++k) {
                    if (!samples[i + k]) continue;
                    const std::string function = symbols.Function(samples[i + k]);
                    // Every thread starts in these; they say nothing.
                    if (function.rfind("commpage_thread_exit", 0) == 0 || function.rfind("pthread_thread_entry", 0) == 0
                        || function.rfind("_start", 0) == 0 || function.rfind("runtime_loader", 0) == 0
                        || function.rfind("WTF::Thread::entryPoint", 0) == 0 || function.rfind("thread_entry", 0) == 0)
                        continue;
                    if (seen.insert(function).second) ++inclusive[function];
                }
            }
            i += n;
        }
    };
    for (const auto& info : Threads(team, only)) {
        Sampled sampled;
        sampled.name = info.name;
        void* address = nullptr;
        sampled.area = create_area("summit mcp samples", &address, B_ANY_ADDRESS, areaSize, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
        if (sampled.area < 0) continue;
        sampled.samples = static_cast<addr_t*>(address);
        sampled.startCPU = info.user_time + info.kernel_time;
        debug_nub_start_profiler message {};
        message.reply_port = attachment.context.replyPort;
        message.thread = info.thread;
        message.interval = interval;
        message.sample_area = sampled.area;
        message.stack_depth = depth;
        message.variable_stack_depth = depth > 1;
        message.profile_kernel = false;
        debug_nub_start_profiler_reply reply {};
        if (sDebug.sendMessage(&attachment.context, B_DEBUG_MESSAGE_START_PROFILER, &message, sizeof(message), &reply, sizeof(reply))
                != B_OK || reply.error != B_OK) {
            delete_area(sampled.area);
            continue;
        }
        sampled.running = true;
        threads[info.thread] = sampled;
    }
    const bigtime_t started = system_time();
    const bigtime_t deadline = started + bigtime_t(seconds * 1e6);
    while (system_time() < deadline) {
        debug_debugger_message_data message;
        int32 code = 0;
        const ssize_t size = read_port_etc(attachment.port, &code, &message, sizeof(message), B_ABSOLUTE_TIMEOUT, deadline);
        if (size < 0) {
            if (size == B_INTERRUPTED) continue;
            break;
        }
        if (code == B_DEBUGGER_MESSAGE_PROFILER_UPDATE) {
            const auto& update = message.profiler_update;
            take(update.origin.thread, update.sample_count, update.stack_depth, update.variable_stack_depth);
            if (auto found = threads.find(update.origin.thread); found != threads.end()) {
                found->second.dropped += update.dropped_ticks;
                if (update.stopped) {
                    found->second.running = false;
                    found->second.endCPU = update.last_cpu_time;
                }
            }
        } else if (code == B_DEBUGGER_MESSAGE_THREAD_DELETED) {
            if (auto found = threads.find(message.origin.thread); found != threads.end()) found->second.running = false;
        }
        if (message.origin.thread >= 0 && message.origin.nub_port >= 0) ContinueThread(attachment.nub, message.origin.thread);
    }
    const double elapsed = double(system_time() - started) / 1e6;
    for (auto& [thread, sampled] : threads) {
        if (sampled.running) {
            debug_nub_stop_profiler message { attachment.context.replyPort, thread };
            debug_profiler_update reply {};
            if (sDebug.sendMessage(&attachment.context, B_DEBUG_MESSAGE_STOP_PROFILER, &message, sizeof(message), &reply, sizeof(reply))
                    == B_OK && reply.origin.thread == thread) {
                take(thread, reply.sample_count, reply.stack_depth, reply.variable_stack_depth);
                sampled.dropped += reply.dropped_ticks;
                sampled.endCPU = reply.last_cpu_time;
            }
        }
        delete_area(sampled.area);
    }
    auto top = [&](const std::map<std::string, uint64>& counts, size_t limit) {
        std::vector<std::pair<std::string, uint64>> sorted(counts.begin(), counts.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        json list = json::array();
        for (size_t i = 0; i < sorted.size() && i < limit; ++i)
            list.push_back({{"function", sorted[i].first}, {"samples", sorted[i].second},
                {"percent", totalTicks ? std::round(1000.0 * sorted[i].second / totalTicks) / 10 : 0.0}});
        return list;
    };
    json perThread = json::array();
    std::vector<std::pair<uint64, thread_id>> busiest;
    for (const auto& [thread, sampled] : threads) busiest.emplace_back(sampled.ticks, thread);
    std::sort(busiest.rbegin(), busiest.rend());
    for (const auto& [ticks, thread] : busiest) {
        if (!ticks) break;
        const Sampled& sampled = threads[thread];
        json entry = {{"thread", thread}, {"name", sampled.name}, {"samples", ticks},
            {"percent", totalTicks ? std::round(1000.0 * ticks / totalTicks) / 10 : 0.0}};
        if (sampled.endCPU > sampled.startCPU) entry["cpuSeconds"] = double(sampled.endCPU - sampled.startCPU) / 1e6;
        if (sampled.dropped) entry["droppedTicks"] = sampled.dropped;
        entry["top"] = top(selfByThread[thread], 6);
        perThread.push_back(entry);
    }
    json result = {{"team", team}, {"seconds", elapsed}, {"intervalMicros", interval}, {"stackDepth", depth},
        {"samples", totalTicks}, {"threadsSampled", threads.size()}, {"self", top(self, 40)}, {"byImage", top(images, 15)},
        {"threads", perThread}};
    if (depth > 1) result["inclusive"] = top(inclusive, 40);
    std::printf("%s\n", result.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
    return 0;
}
}

namespace summit {
// Summit --mcp-debug-helper stacks TEAM DEPTH [THREAD...]
// Summit --mcp-debug-helper profile TEAM SECONDS INTERVAL_US DEPTH [THREAD...]
int RunMcpDebugHelper(int argc, char** argv)
{
    std::string error;
    if (!sDebug.Load(error)) {
        std::printf("%s\n", json{{"error", error}}.dump().c_str());
        return 1;
    }
    if (argc >= 5 && !std::strcmp(argv[2], "stacks")) {
        std::set<thread_id> only;
        for (int i = 5; i < argc; ++i) only.insert(thread_id(std::atoi(argv[i])));
        return Stacks(team_id(std::atoi(argv[3])), only, std::clamp(std::atoi(argv[4]), 1, 256));
    }
    if (argc >= 7 && !std::strcmp(argv[2], "profile")) {
        std::set<thread_id> only;
        for (int i = 7; i < argc; ++i) only.insert(thread_id(std::atoi(argv[i])));
        return Profile(team_id(std::atoi(argv[3])), std::clamp(std::atof(argv[4]), 0.1, 120.0),
            std::clamp<bigtime_t>(std::atoll(argv[5]), 100, 1000000), std::clamp(std::atoi(argv[6]), 1, 64), only);
    }
    std::printf("%s\n", json{{"error", "usage: --mcp-debug-helper stacks TEAM DEPTH [THREAD...] | profile TEAM SECONDS INTERVAL_US DEPTH [THREAD...]"}}.dump().c_str());
    return 2;
}
}
#endif
