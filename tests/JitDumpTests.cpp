// The JITDump reader the MCP stacks and profile tools name JIT code with.
#include "core/JitDump.h"
#include <cstdio>
#include <unistd.h>
#include <iostream>
#include <vector>

static int checks = 0, failures = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; std::cerr << __FILE__ << ":" << __LINE__ << ": " #condition "\n"; } } while (0)

// A file as JavaScriptCore writes it: header, then records.
static void Write(const std::string& path, const std::vector<std::pair<uint64_t, std::string>>& loads, uint64_t size, bool debugInfo)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    const uint32_t header[6] = {0x4a695444, 1, 40, 0x3E, 0, 1234};
    const uint64_t more[2] = {7, 0};
    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    file.write(reinterpret_cast<const char*>(more), sizeof(more));
    uint64_t index = 0;
    for (const auto& [address, name] : loads) {
        if (debugInfo) {
            // A JIT_CODE_DEBUG_INFO record to skip.
            const uint32_t record[4] = {2, 16 + 16, 0, 0};
            const uint64_t body[2] = {address, 0};
            file.write(reinterpret_cast<const char*>(record), sizeof(record));
            file.write(reinterpret_cast<const char*>(body), sizeof(body));
        }
        const std::vector<char> code(size, '\xCC');
        const uint32_t total = static_cast<uint32_t>(16 + 8 + 40 + name.size() + 1 + code.size());
        const uint32_t record[4] = {0, total, 0, 0};
        const uint32_t ids[2] = {1234, 1};
        const uint64_t fields[5] = {address, address, size, index++};
        file.write(reinterpret_cast<const char*>(record), sizeof(record));
        file.write(reinterpret_cast<const char*>(ids), sizeof(ids));
        file.write(reinterpret_cast<const char*>(fields), 40);
        file.write(name.c_str(), static_cast<std::streamsize>(name.size() + 1));
        file.write(code.data(), static_cast<std::streamsize>(code.size()));
    }
}

int main()
{
    const auto directory = std::filesystem::temp_directory_path() / ("summit-jitdump-" + std::to_string(::getpid()));
    std::filesystem::create_directories(directory);
    Write((directory / "jit-11-1234.dump").string(), {{0x1000, "Baseline: render#AbCd"}, {0x2000, "DFG: tick"}}, 0x100, true);
    Write((directory / "jit-12-1234.dump").string(), {{0x3000, "Thunk: arityFixup"}}, 0x40, false);
    // Another process's.
    Write((directory / "jit-13-999.dump").string(), {{0x4000, "other"}}, 0x40, false);
    {
        std::ofstream junk(directory / "jit-14-1234.dump", std::ios::binary);
        junk << "not a dump";
    }
    summit::JitCodeMap map;
    map.Load(directory.string(), 1234);
    CHECK(map.Count() == 3);
    CHECK(map.Find(0x1000) && map.Find(0x1000)->name == "Baseline: render#AbCd");
    CHECK(map.Find(0x10ff) && !map.Find(0x1100));
    CHECK(map.Find(0x2080) && map.Find(0x2080)->name == "DFG: tick");
    CHECK(map.Find(0x3010) && map.Find(0x3010)->name == "Thunk: arityFixup");
    CHECK(!map.Find(0x4000) && !map.Find(0x500));
    // Code loaded later over freed code replaces it.
    Write((directory / "jit-15-1234.dump").string(), {{0x2080, "FTL: tick"}}, 0x20, false);
    map.LoadFile((directory / "jit-15-1234.dump").string());
    CHECK(map.Find(0x2090) && map.Find(0x2090)->name == "FTL: tick" && !map.Find(0x2000));
    CHECK(!map.LoadFile((directory / "jit-14-1234.dump").string()));
    std::filesystem::remove_all(directory);
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
