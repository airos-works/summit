#include "ProfileLock.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace summit {
ProfileLock::Result ProfileLock::Take(const std::filesystem::path& directory, int64_t team, int64_t& holder, std::string& error)
{
    Release();
    holder = 0;
    error.clear();
    // Not inherited by the engine's processes, which would keep it after
    // Summit ended.
    const int descriptor = ::open((directory / "lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        error = std::strerror(errno);
        return Result::Unsupported;
    }
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        const int failure = errno;
        if (failure == EWOULDBLOCK || failure == EAGAIN) {
            char text[32] = { };
            if (::pread(descriptor, text, sizeof text - 1, 0) > 0) holder = std::strtoll(text, nullptr, 10);
            ::close(descriptor);
            return Result::Held;
        }
        ::close(descriptor);
        error = std::strerror(failure);
        return Result::Unsupported;
    }
    const std::string text = std::to_string(team) + "\n";
    if (::ftruncate(descriptor, 0) != 0 || ::pwrite(descriptor, text.data(), text.size(), 0) != static_cast<ssize_t>(text.size())) {
        // Locked all the same; only a second copy's hand-over needs the team.
        error = std::strerror(errno);
    }
    fDescriptor = descriptor;
    return Result::Taken;
}

void ProfileLock::Release()
{
    if (fDescriptor < 0) return;
    // Closing drops the lock; the file stays, naming nobody.
    (void)!::ftruncate(fDescriptor, 0);
    ::close(fDescriptor);
    fDescriptor = -1;
}
}
