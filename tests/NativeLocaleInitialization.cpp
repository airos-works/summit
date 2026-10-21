// Native Haiku diagnostic for the first-time libroot ICU initialization race.
// Build: g++ -std=c++17 -O2 tests/NativeLocaleInitialization.cpp -o NativeLocaleInitialization
// Run each mode in a fresh process. The default exposes the OS race; --warm
// completes locale initialization before readers, as the Haiku engine now does.
// A failed scan is an error even when a subsequent scan succeeds.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>
#include <vector>

int main(int argc, char** argv)
{
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--warm"))) {
        std::fprintf(stderr, "Usage: %s [--warm]\n", argv[0]);
        return 2;
    }
    const bool warm = argc == 2;
    if (warm)
        tzset();

    std::atomic<unsigned> failures { 0 }, scans { 0 }, ready { 0 };
    std::atomic<bool> start { false }, done { false };
    std::vector<std::thread> readers;
    for (unsigned t = 0; t < 3; ++t) {
        readers.emplace_back([&] {
            ++ready;
            while (!start.load())
                std::this_thread::yield();
            do {
                int major = -1, minor = -1;
                int count = std::sscanf("3.1 Mesa 25.3.6", "%i.%i", &major, &minor);
                if (count != 2 || major != 3 || minor != 1)
                    ++failures;
                ++scans;
            } while (!done.load());
        });
    }
    while (ready.load() != 3)
        std::this_thread::yield();
    start = true;
    while (scans.load() < 1000)
        std::this_thread::yield();
    time_t now = time(nullptr);
    tm converted {};
    const bool convertedTime = localtime_r(&now, &converted);
    done = true;
    for (auto& reader : readers)
        reader.join();

    std::printf("warm=%d scans=%u failures=%u time=%d\n",
        warm, scans.load(), failures.load(), convertedTime);
    return failures || !convertedTime ? 1 : 0;
}
