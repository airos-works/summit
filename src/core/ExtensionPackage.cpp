#include "ExtensionPackage.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace summit {
namespace fs = std::filesystem;
namespace {
// Raw DEFLATE (RFC 1951), after Mark Adler's puff: small and slow, which
// suits the few kilobytes of an icon or a manifest.
class Inflater {
public:
    Inflater(const unsigned char* input, size_t size, size_t limit) : fInput(input), fSize(size), fLimit(limit) { }
    std::string Run()
    {
        bool last = false;
        do {
            last = Bits(1);
            switch (Bits(2)) {
            case 0: Stored(); break;
            case 1: Fixed(); break;
            case 2: Dynamic(); break;
            default: throw std::runtime_error("invalid block type");
            }
        } while (!last);
        return std::move(fOutput);
    }

private:
    static constexpr int kMaximumBits = 15;
    struct Huffman {
        std::array<short, kMaximumBits + 1> count {};
        std::array<short, 288> symbol {};
    };
    int Bits(int need)
    {
        uint32_t value = fBitBuffer;
        while (fBitCount < need) {
            if (fPosition == fSize) throw std::runtime_error("truncated data");
            value |= uint32_t(fInput[fPosition++]) << fBitCount;
            fBitCount += 8;
        }
        fBitBuffer = value >> need;
        fBitCount -= need;
        return int(value & ((1u << need) - 1));
    }
    void Put(unsigned char byte)
    {
        if (fOutput.size() >= fLimit) throw std::runtime_error("too large");
        fOutput.push_back(char(byte));
    }
    void Stored()
    {
        fBitBuffer = 0;
        fBitCount = 0;
        if (fSize - fPosition < 4) throw std::runtime_error("truncated data");
        const unsigned length = fInput[fPosition] | (fInput[fPosition + 1] << 8);
        const unsigned complement = fInput[fPosition + 2] | (fInput[fPosition + 3] << 8);
        fPosition += 4;
        if (length != (~complement & 0xffff) || fSize - fPosition < length) throw std::runtime_error("invalid stored block");
        for (unsigned i = 0; i < length; ++i) Put(fInput[fPosition++]);
    }
    // 0 for a complete code, > 0 incomplete, < 0 over-subscribed.
    static int Construct(Huffman& huffman, const short* lengths, int count)
    {
        huffman.count.fill(0);
        for (int symbol = 0; symbol < count; ++symbol) ++huffman.count[lengths[symbol]];
        if (huffman.count[0] == count) return 0;
        int left = 1;
        for (int length = 1; length <= kMaximumBits; ++length) {
            left <<= 1;
            left -= huffman.count[length];
            if (left < 0) return left;
        }
        std::array<short, kMaximumBits + 1> offsets {};
        for (int length = 1; length < kMaximumBits; ++length) offsets[length + 1] = offsets[length] + huffman.count[length];
        for (int symbol = 0; symbol < count; ++symbol)
            if (lengths[symbol]) huffman.symbol[offsets[lengths[symbol]]++] = short(symbol);
        return left;
    }
    int Decode(const Huffman& huffman)
    {
        int code = 0, first = 0, index = 0;
        for (int length = 1; length <= kMaximumBits; ++length) {
            code |= Bits(1);
            const int count = huffman.count[length];
            if (code - count < first) return huffman.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw std::runtime_error("invalid code");
    }
    void Codes(const Huffman& lengthCode, const Huffman& distanceCode)
    {
        static constexpr short lengthBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
            35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
        static constexpr short lengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
            3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        static constexpr int distanceBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
            257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
        static constexpr short distanceExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
            7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
        for (;;) {
            int symbol = Decode(lengthCode);
            if (symbol < 256) { Put(static_cast<unsigned char>(symbol)); continue; }
            if (symbol == 256) return;
            symbol -= 257;
            if (symbol >= 29) throw std::runtime_error("invalid length");
            const size_t length = lengthBase[symbol] + Bits(lengthExtra[symbol]);
            symbol = Decode(distanceCode);
            if (symbol >= 30) throw std::runtime_error("invalid distance");
            const size_t distance = distanceBase[symbol] + Bits(distanceExtra[symbol]);
            if (distance > fOutput.size()) throw std::runtime_error("distance too far back");
            for (size_t i = 0; i < length; ++i) Put(static_cast<unsigned char>(fOutput[fOutput.size() - distance]));
        }
    }
    void Fixed()
    {
        static const auto codes = [] {
            std::array<short, 288> lengths {};
            int symbol = 0;
            for (; symbol < 144; ++symbol) lengths[symbol] = 8;
            for (; symbol < 256; ++symbol) lengths[symbol] = 9;
            for (; symbol < 280; ++symbol) lengths[symbol] = 7;
            for (; symbol < 288; ++symbol) lengths[symbol] = 8;
            std::pair<Huffman, Huffman> result;
            Construct(result.first, lengths.data(), 288);
            for (symbol = 0; symbol < 30; ++symbol) lengths[symbol] = 5;
            Construct(result.second, lengths.data(), 30);
            return result;
        }();
        Codes(codes.first, codes.second);
    }
    void Dynamic()
    {
        static constexpr short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
        const int lengthCount = Bits(5) + 257, distanceCount = Bits(5) + 1, codeCount = Bits(4) + 4;
        if (lengthCount > 286 || distanceCount > 30) throw std::runtime_error("bad counts");
        std::array<short, 320> lengths {};
        for (int index = 0; index < codeCount; ++index) lengths[order[index]] = short(Bits(3));
        Huffman lengthCode, distanceCode;
        if (Construct(lengthCode, lengths.data(), 19) != 0) throw std::runtime_error("incomplete code lengths");
        for (int index = 0; index < lengthCount + distanceCount;) {
            int symbol = Decode(lengthCode);
            if (symbol < 16) { lengths[index++] = short(symbol); continue; }
            short length = 0;
            if (symbol == 16) {
                if (!index) throw std::runtime_error("repeat with no first length");
                length = lengths[index - 1];
                symbol = 3 + Bits(2);
            } else if (symbol == 17) symbol = 3 + Bits(3);
            else symbol = 11 + Bits(7);
            if (index + symbol > lengthCount + distanceCount) throw std::runtime_error("too many lengths");
            while (symbol--) lengths[index++] = length;
        }
        if (!lengths[256]) throw std::runtime_error("no end-of-block code");
        int error = Construct(lengthCode, lengths.data(), lengthCount);
        if (error && (error < 0 || lengthCount != lengthCode.count[0] + lengthCode.count[1]))
            throw std::runtime_error("incomplete literal/length code");
        error = Construct(distanceCode, lengths.data() + lengthCount, distanceCount);
        if (error && (error < 0 || distanceCount != distanceCode.count[0] + distanceCode.count[1]))
            throw std::runtime_error("incomplete distance code");
        Codes(lengthCode, distanceCode);
    }

    const unsigned char* fInput;
    size_t fSize, fPosition = 0, fLimit;
    uint32_t fBitBuffer = 0;
    int fBitCount = 0;
    std::string fOutput;
};

uint32_t CRC32(const std::string& data)
{
    static const auto table = [] {
        std::array<uint32_t, 256> result {};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) value = value & 1 ? 0xedb88320u ^ (value >> 1) : value >> 1;
            result[i] = value;
        }
        return result;
    }();
    uint32_t crc = 0xffffffffu;
    for (unsigned char byte : data) crc = table[(crc ^ byte) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

uint32_t Little16(const unsigned char* bytes) { return bytes[0] | (bytes[1] << 8); }
uint32_t Little32(const unsigned char* bytes)
{
    return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (uint32_t(bytes[3]) << 24);
}

bool ReadAt(std::ifstream& file, uint64_t offset, size_t size, std::vector<unsigned char>& bytes)
{
    bytes.resize(size);
    file.clear();
    file.seekg(std::streamoff(offset));
    return file && file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size)) && size_t(file.gcount()) == size;
}

// A member of a ZIP archive, which may follow a CRX3 header: offsets in the
// central directory are then counted from the archive's own start.
std::optional<std::string> ReadArchiveMember(const fs::path& path, const std::string& name, size_t limit)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    file.seekg(0, std::ios::end);
    const uint64_t fileSize = uint64_t(file.tellg());
    if (fileSize < 22) return std::nullopt;
    // The end of central directory record, with a comment of up to 64 KiB.
    const size_t tailSize = size_t(std::min<uint64_t>(fileSize, 22 + 65535));
    std::vector<unsigned char> tail;
    if (!ReadAt(file, fileSize - tailSize, tailSize, tail)) return std::nullopt;
    size_t end = std::string::npos;
    for (size_t i = tailSize - 22 + 1; i-- > 0;)
        if (Little32(&tail[i]) == 0x06054b50) { end = i; break; }
    if (end == std::string::npos) return std::nullopt;
    const uint64_t endOffset = fileSize - tailSize + end;
    const uint32_t directorySize = Little32(&tail[end + 12]), directoryOffset = Little32(&tail[end + 16]);
    if (directoryOffset == 0xffffffffu || directorySize > endOffset || directoryOffset > endOffset - directorySize)
        return std::nullopt;
    const uint64_t base = endOffset - directorySize - directoryOffset;
    std::vector<unsigned char> directory;
    if (directorySize > 64 * 1024 * 1024 || !ReadAt(file, endOffset - directorySize, directorySize, directory))
        return std::nullopt;
    for (size_t at = 0; at + 46 <= directory.size();) {
        if (Little32(&directory[at]) != 0x02014b50) return std::nullopt;
        const uint32_t method = Little16(&directory[at + 10]), crc = Little32(&directory[at + 16]);
        const uint32_t compressedSize = Little32(&directory[at + 20]), size = Little32(&directory[at + 24]);
        const uint32_t nameLength = Little16(&directory[at + 28]);
        const uint32_t extraLength = Little16(&directory[at + 30]), commentLength = Little16(&directory[at + 32]);
        const uint32_t localOffset = Little32(&directory[at + 42]);
        if (at + 46 + nameLength > directory.size()) return std::nullopt;
        const std::string entryName(reinterpret_cast<const char*>(&directory[at + 46]), nameLength);
        at += 46 + nameLength + extraLength + commentLength;
        if (entryName != name) continue;
        if (size > limit || (method != 0 && method != 8) || compressedSize == 0xffffffffu) return std::nullopt;
        std::vector<unsigned char> local;
        if (!ReadAt(file, base + localOffset, 30, local) || Little32(local.data()) != 0x04034b50) return std::nullopt;
        const uint64_t dataOffset = base + localOffset + 30 + Little16(&local[26]) + Little16(&local[28]);
        std::vector<unsigned char> data;
        if (compressedSize > 128 * 1024 * 1024 || dataOffset + compressedSize > fileSize
            || !ReadAt(file, dataOffset, compressedSize, data))
            return std::nullopt;
        std::string contents;
        try {
            contents = method == 0 ? std::string(data.begin(), data.end())
                : Inflater(data.data(), data.size(), limit).Run();
        } catch (const std::exception&) {
            return std::nullopt;
        }
        if (contents.size() != size || CRC32(contents) != crc) return std::nullopt;
        return contents;
    }
    return std::nullopt;
}

int IconSize(const std::string& key)
{
    if (key.empty() || key.size() > 5 || !std::all_of(key.begin(), key.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return 0;
    return std::stoi(key);
}

// From a manifest's {"16": "a.png", "48": "b.png"} (or one path for any
// size), the paths in the order to try them for size pixels.
void AddIconCandidates(const nlohmann::json& icons, int size, std::vector<std::string>& paths)
{
    auto raster = [](const std::string& path) {
        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".svg") != 0;
    };
    if (icons.is_string()) {
        if (raster(icons.get<std::string>())) paths.push_back(icons.get<std::string>());
        return;
    }
    if (!icons.is_object()) return;
    std::vector<std::pair<int, std::string>> sized;
    for (const auto& [key, value] : icons.items())
        if (const int pixels = IconSize(key); pixels > 0 && value.is_string() && raster(value.get<std::string>()))
            sized.emplace_back(pixels, value.get<std::string>());
    // At least size, smallest first; then the smaller ones, largest first.
    std::sort(sized.begin(), sized.end(), [size](const auto& a, const auto& b) {
        const bool aLarge = a.first >= size, bLarge = b.first >= size;
        if (aLarge != bLarge) return aLarge;
        return aLarge ? a.first < b.first : a.first > b.first;
    });
    for (const auto& item : sized) paths.push_back(item.second);
}
}

std::optional<std::string> ReadExtensionPackageFile(const fs::path& package, std::string name, size_t limit)
{
    while (!name.empty() && name.front() == '/') name.erase(0, 1);
    if (name.compare(0, 2, "./") == 0) name.erase(0, 2);
    if (name.empty() || name.find('\0') != std::string::npos) return std::nullopt;
    for (size_t start = 0; start <= name.size();) {
        const auto next = name.find('/', start);
        const auto part = name.substr(start, next == std::string::npos ? next : next - start);
        if (part.empty() || part == "." || part == "..") return std::nullopt;
        if (next == std::string::npos) break;
        start = next + 1;
    }
    std::error_code error;
    if (fs::is_directory(package, error)) {
        const auto path = package / name;
        const auto status = fs::symlink_status(path, error);
        if (error || !fs::is_regular_file(status)) return std::nullopt;
        const auto size = fs::file_size(path, error);
        if (error || size > limit) return std::nullopt;
        std::ifstream file(path, std::ios::binary);
        std::string contents(size_t(size), '\0');
        if (!file.read(contents.data(), std::streamsize(size))) return std::nullopt;
        return contents;
    }
    if (!fs::is_regular_file(package, error)) return std::nullopt;
    return ReadArchiveMember(package, name, limit);
}

std::string ExtensionIconData(const fs::path& package, int size)
{
    const auto text = ReadExtensionPackageFile(package, "manifest.json", 1024 * 1024);
    if (!text) return {};
    std::string source = *text;
    if (source.compare(0, 3, "\xEF\xBB\xBF") == 0) source.erase(0, 3);
    const auto manifest = nlohmann::json::parse(source, nullptr, false, true);
    if (!manifest.is_object()) return {};
    std::vector<std::string> paths;
    if (manifest.contains("icons")) AddIconCandidates(manifest["icons"], size, paths);
    for (const char* key : { "action", "browser_action", "page_action" }) {
        const auto action = manifest.find(key);
        if (action != manifest.end() && action->is_object() && action->contains("default_icon"))
            AddIconCandidates(action->at("default_icon"), size, paths);
    }
    for (const auto& path : paths) {
        auto data = ReadExtensionPackageFile(package, path, 2 * 1024 * 1024);
        if (data && !data->empty()) return *data;
    }
    return {};
}
}
