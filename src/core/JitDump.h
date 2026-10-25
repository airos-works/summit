#pragma once
// JavaScriptCore's JITDump files (JSC_useJITDump=1 in a process's environment:
// jit-THREAD-PROCESS.dump in JSC_jitDumpDirectory, /tmp by default), read to
// name addresses in JavaScript and the engine's generated code, which no
// image's symbols cover. Format: Linux perf's jitdump specification; only
// JIT_CODE_LOAD records are used.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>

namespace summit {

struct JitCode {
    uint64_t start = 0, size = 0;
    std::string name;
};

class JitCodeMap {
public:
    // Every jit-*-PROCESS.dump in directory.
    void Load(const std::string& directory, int32_t process)
    {
        const std::string suffix = "-" + std::to_string(process) + ".dump";
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
            const auto name = entry.path().filename().string();
            if (name.rfind("jit-", 0) == 0 && name.size() > suffix.size()
                && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
                LoadFile(entry.path().string());
        }
    }

    // Whether the file is a JITDump; code loaded at an address replaces what
    // was there before (the engine reuses freed code memory).
    bool LoadFile(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        uint32_t header[4] = { };
        if (!file.read(reinterpret_cast<char*>(header), sizeof(header)) || header[0] != kMagic || header[2] < 16) return false;
        file.seekg(header[2]);
        while (file) {
            const auto start = file.tellg();
            uint32_t record[4] = { };
            if (!file.read(reinterpret_cast<char*>(record), sizeof(record)) || record[1] < sizeof(record)) break;
            if (record[0] == kCodeLoad && record[1] > sizeof(record) + 40) {
                uint64_t fields[5] = { };
                uint32_t ids[2] = { };
                if (!file.read(reinterpret_cast<char*>(ids), sizeof(ids)) || !file.read(reinterpret_cast<char*>(fields), sizeof(fields))) break;
                // vma, code address, code size, index; then the name.
                JitCode code;
                code.start = fields[1];
                code.size = fields[2];
                std::getline(file, code.name, '\0');
                if (code.size && code.name.size() < 4096) {
                    // Code that overlapped the new code is gone.
                    auto after = fCode.lower_bound(code.start);
                    while (after != fCode.end() && after->first < code.start + code.size) after = fCode.erase(after);
                    if (after != fCode.begin()) {
                        auto before = std::prev(after);
                        if (before->first + before->second.size > code.start) fCode.erase(before);
                    }
                    fCode[code.start] = std::move(code);
                }
            }
            file.clear();
            file.seekg(start + std::streamoff(record[1]));
        }
        return true;
    }

    const JitCode* Find(uint64_t address) const
    {
        auto found = fCode.upper_bound(address);
        if (found == fCode.begin()) return nullptr;
        --found;
        return address < found->second.start + found->second.size ? &found->second : nullptr;
    }
    size_t Count() const { return fCode.size(); }

private:
    static constexpr uint32_t kMagic = 0x4a695444, kCodeLoad = 0;
    std::map<uint64_t, JitCode> fCode;
};

}
