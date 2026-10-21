// Enumerate the actual loaded images of one explicitly selected native team.
#include <OS.h>
#include <image.h>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    if (team <= 0) return 2;
    int32 cookie = 0;
    image_info image;
    unsigned count = 0;
    while (get_next_image_info(team, &cookie, &image) == B_OK) {
        std::puts(image.name);
        ++count;
    }
    return count ? 0 : 1;
}
