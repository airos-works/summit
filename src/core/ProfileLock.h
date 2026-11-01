#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace summit {
// The lock a running Summit holds on its profile folder (#93), so that a
// second copy (another build or package, or since browser profiles (#157)
// made Summit B_MULTIPLE_LAUNCH, the same one) does not open the same
// profile: it hands its start to the first. It is an flock() of the
// folder's "lock" file, which the system drops when the process ends; the
// file names the team that holds it.
class ProfileLock {
public:
    enum class Result {
        Taken,
        // Another process has it: holder is the team the file names (0 when
        // it names none).
        Held,
        // The folder's file system cannot lock (error says why).
        Unsupported,
    };
    ProfileLock() = default;
    ProfileLock(const ProfileLock&) = delete;
    ProfileLock& operator=(const ProfileLock&) = delete;
    ~ProfileLock() { Release(); }
    Result Take(const std::filesystem::path& directory, int64_t team, int64_t& holder, std::string& error);
    void Release();
    bool Holds() const { return fDescriptor >= 0; }
private:
    int fDescriptor = -1;
};
}
