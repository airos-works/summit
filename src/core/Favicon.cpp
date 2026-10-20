#include "Favicon.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>

// Sites that offer only an SVG favicon: Haiku has no SVG translator.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvg/nanosvg.h>
#include <nanosvg/nanosvgrast.h>
#pragma GCC diagnostic pop

namespace summit {
static uint32_t Read16(std::string_view data, size_t offset)
{
    return uint8_t(data[offset]) | uint8_t(data[offset + 1]) << 8;
}
static uint32_t Read32(std::string_view data, size_t offset)
{
    return Read16(data, offset) | Read16(data, offset + 2) << 16;
}

bool IsIco(std::string_view data)
{
    return data.size() >= 6 && Read16(data, 0) == 0 && (Read16(data, 2) == 1 || Read16(data, 2) == 2)
        && Read16(data, 4) > 0 && data.size() >= 6 + 16 * size_t(Read16(data, 4));
}

IcoEntry SelectIcoEntry(std::string_view data, int wanted)
{
    IcoEntry best;
    if (!IsIco(data)) return best;
    int bestScore = 0, bestDepth = -1;
    const uint32_t count = Read16(data, 4);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t entry = 6 + 16 * i;
        const uint32_t size = Read32(data, entry + 8), offset = Read32(data, entry + 12);
        if (!size || offset >= data.size() || size > data.size() - offset) continue;
        auto bytes = data.substr(offset, size);
        IcoEntry candidate;
        if (bytes.size() >= 8 && bytes.substr(0, 8) == std::string_view("\x89PNG\r\n\x1a\n", 8)) {
            candidate.kind = IcoEntryKind::PNG;
        } else if (bytes.size() >= 40 && Read32(bytes, 0) >= 40) {
            candidate.kind = IcoEntryKind::DIB;
        } else continue;
        candidate.bytes = bytes;
        candidate.width = uint8_t(data[entry]) ? uint8_t(data[entry]) : 256;
        candidate.height = uint8_t(data[entry + 1]) ? uint8_t(data[entry + 1]) : 256;
        int depth = int(Read16(data, entry + 6));
        if (candidate.kind == IcoEntryKind::PNG || !depth) depth = 32;
        const int score = std::abs(candidate.width - wanted) + (candidate.width < wanted / 2 ? 256 : 0);
        if (best.kind == IcoEntryKind::None || score < bestScore || (score == bestScore && depth > bestDepth)) {
            best = candidate;
            bestScore = score;
            bestDepth = depth;
        }
    }
    return best;
}

bool IsSvg(std::string_view data)
{
    const auto head = data.substr(0, 1024);
    const auto start = head.find_first_not_of(" \t\r\n\xef\xbb\xbf");
    if (start == std::string_view::npos || head[start] != '<') return false;
    return head.find("<svg") != std::string_view::npos;
}

bool RasterizeSvg(std::string_view data, int size, Image& out)
{
    out = Image();
    // A favicon is small; a large one is not worth the time.
    if (size <= 0 || size > 512 || data.empty() || data.size() > 256 * 1024 || !IsSvg(data)) return false;
    std::string text(data);
    std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> image(nsvgParse(text.data(), "px", 96), nsvgDelete);
    if (!image || !image->shapes || !(image->width > 0) || !(image->height > 0)) return false;
    std::unique_ptr<NSVGrasterizer, decltype(&nsvgDeleteRasterizer)> rasterizer(nsvgCreateRasterizer(), nsvgDeleteRasterizer);
    if (!rasterizer) return false;
    const float scale = size / std::max(image->width, image->height);
    if (!std::isfinite(scale) || scale <= 0) return false;
    out.width = size;
    out.height = size;
    out.rgba.assign(size_t(size) * size * 4, 0);
    const float x = (size - image->width * scale) / 2, y = (size - image->height * scale) / 2;
    nsvgRasterize(rasterizer.get(), image.get(), x, y, scale, out.rgba.data(), size, size, size * 4);
    for (size_t i = 3; i < out.rgba.size(); i += 4)
        if (out.rgba[i]) return true;
    out = Image();
    return false;
}

bool DecodeDIB(std::string_view dib, Image& out)
{
    if (dib.size() < 40) return false;
    const uint32_t header = Read32(dib, 0);
    const int32_t width = int32_t(Read32(dib, 4));
    const int32_t doubledHeight = int32_t(Read32(dib, 8));
    const uint32_t depth = Read16(dib, 14), compression = Read32(dib, 16);
    uint32_t colours = Read32(dib, 32);
    // Icons store the colour image and its AND mask one above the other.
    const int32_t height = doubledHeight / 2;
    if (header > dib.size() || width <= 0 || height <= 0 || width > 1024 || height > 1024) return false;
    if (compression != 0 && !(compression == 3 && depth == 32)) return false;
    if (depth != 1 && depth != 4 && depth != 8 && depth != 24 && depth != 32) return false;
    if (depth <= 8 && (!colours || colours > (1u << depth))) colours = 1u << depth;
    if (depth > 8) colours = 0;
    size_t offset = header + (compression == 3 && header == 40 ? 12 : 0);
    const size_t palette = offset;
    offset += colours * 4;
    const size_t stride = ((size_t(width) * depth + 31) / 32) * 4;
    const size_t maskStride = ((size_t(width) + 31) / 32) * 4;
    if (offset > dib.size() || stride * height > dib.size() - offset) return false;
    const size_t maskOffset = offset + stride * height;
    const bool hasMask = maskOffset <= dib.size() && maskStride * height <= dib.size() - maskOffset;
    out.width = width;
    out.height = height;
    out.rgba.assign(size_t(width) * height * 4, 0);
    bool anyAlpha = false;
    for (int32_t y = 0; y < height; ++y) {
        // Rows are stored bottom-up.
        const auto row = dib.substr(offset + stride * (height - 1 - y), stride);
        for (int32_t x = 0; x < width; ++x) {
            uint8_t* pixel = &out.rgba[(size_t(y) * width + x) * 4];
            if (depth == 32 || depth == 24) {
                const size_t at = size_t(x) * (depth / 8);
                pixel[0] = uint8_t(row[at + 2]);
                pixel[1] = uint8_t(row[at + 1]);
                pixel[2] = uint8_t(row[at]);
                pixel[3] = depth == 32 ? uint8_t(row[at + 3]) : 255;
                anyAlpha = anyAlpha || (depth == 32 && pixel[3]);
            } else {
                const size_t bit = size_t(x) * depth;
                const uint8_t byte = uint8_t(row[bit / 8]);
                const uint32_t index = (byte >> (8 - depth - bit % 8)) & ((1u << depth) - 1);
                if (index >= colours) continue;
                const size_t entry = palette + index * 4;
                pixel[0] = uint8_t(dib[entry + 2]);
                pixel[1] = uint8_t(dib[entry + 1]);
                pixel[2] = uint8_t(dib[entry]);
                pixel[3] = 255;
            }
        }
    }
    // Without an alpha channel the AND mask says which pixels are transparent.
    if (!anyAlpha) {
        for (int32_t y = 0; y < height; ++y) {
            for (int32_t x = 0; x < width; ++x) {
                uint8_t* pixel = &out.rgba[(size_t(y) * width + x) * 4];
                pixel[3] = 255;
                if (!hasMask) continue;
                const uint8_t byte = uint8_t(dib[maskOffset + maskStride * (height - 1 - y) + x / 8]);
                if (byte & (0x80 >> (x % 8))) pixel[3] = 0;
            }
        }
    }
    return true;
}

Image ScaleImage(const Image& image, int size)
{
    Image result;
    if (image.Empty() || size <= 0 || image.rgba.size() < size_t(image.width) * image.height * 4) return result;
    result.width = result.height = size;
    result.rgba.assign(size_t(size) * size * 4, 0);
    const double scale = double(std::max(image.width, image.height)) / size;
    const int fittedWidth = std::max(1, int(std::lround(image.width / scale)));
    const int fittedHeight = std::max(1, int(std::lround(image.height / scale)));
    const int left = (size - fittedWidth) / 2, top = (size - fittedHeight) / 2;
    const double scaleX = double(image.width) / fittedWidth, scaleY = double(image.height) / fittedHeight;
    for (int y = 0; y < fittedHeight; ++y) {
        const double y0 = y * scaleY, y1 = (y + 1) * scaleY;
        for (int x = 0; x < fittedWidth; ++x) {
            const double x0 = x * scaleX, x1 = (x + 1) * scaleX;
            // Premultiplied area average, so transparent pixels do not darken edges.
            double sum[4] = { }, area = 0;
            for (int sy = int(y0); sy < std::min(image.height, int(std::ceil(y1))); ++sy) {
                const double wy = std::min(y1, sy + 1.0) - std::max(y0, double(sy));
                for (int sx = int(x0); sx < std::min(image.width, int(std::ceil(x1))); ++sx) {
                    const double w = wy * (std::min(x1, sx + 1.0) - std::max(x0, double(sx)));
                    if (w <= 0) continue;
                    const uint8_t* pixel = &image.rgba[(size_t(sy) * image.width + sx) * 4];
                    const double alpha = pixel[3] / 255.0;
                    for (int c = 0; c < 3; ++c) sum[c] += pixel[c] * alpha * w;
                    sum[3] += alpha * w;
                    area += w;
                }
            }
            if (area <= 0 || sum[3] <= 0) continue;
            uint8_t* out = &result.rgba[(size_t(top + y) * size + left + x) * 4];
            for (int c = 0; c < 3; ++c) out[c] = uint8_t(std::clamp(std::lround(sum[c] / sum[3]), 0L, 255L));
            out[3] = uint8_t(std::clamp(std::lround(sum[3] / area * 255), 0L, 255L));
        }
    }
    return result;
}

static uint32_t Crc32(std::string_view data)
{
    static const auto table = [] {
        std::array<uint32_t, 256> values { };
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            values[n] = c;
        }
        return values;
    }();
    uint32_t crc = 0xffffffffu;
    for (unsigned char byte : data) crc = table[(crc ^ byte) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}
static void Append32(std::string& out, uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8) out += char((value >> shift) & 0xff);
}
static void AppendChunk(std::string& out, const char* type, std::string_view data)
{
    Append32(out, uint32_t(data.size()));
    std::string body(type, 4);
    body.append(data);
    out += body;
    Append32(out, Crc32(body));
}

std::string EncodePNG(const Image& image)
{
    if (image.Empty() || image.rgba.size() < size_t(image.width) * image.height * 4) return { };
    std::string raw;
    raw.reserve(size_t(image.width * 4 + 1) * image.height);
    for (int y = 0; y < image.height; ++y) {
        raw += '\0';
        raw.append(reinterpret_cast<const char*>(&image.rgba[size_t(y) * image.width * 4]), size_t(image.width) * 4);
    }
    std::string zlib("\x78\x01", 2);
    for (size_t offset = 0; offset < raw.size(); offset += 65535) {
        const size_t length = std::min<size_t>(65535, raw.size() - offset);
        zlib += char(offset + length >= raw.size() ? 1 : 0);
        zlib += char(length & 0xff);
        zlib += char(length >> 8);
        zlib += char(~length & 0xff);
        zlib += char((~length >> 8) & 0xff);
        zlib.append(raw, offset, length);
    }
    uint32_t a = 1, b = 0;
    for (unsigned char byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    Append32(zlib, b << 16 | a);
    std::string png("\x89PNG\r\n\x1a\n", 8);
    std::string header;
    Append32(header, uint32_t(image.width));
    Append32(header, uint32_t(image.height));
    header += std::string("\x08\x06\x00\x00\x00", 5);
    AppendChunk(png, "IHDR", header);
    AppendChunk(png, "IDAT", zlib);
    AppendChunk(png, "IEND", { });
    return png;
}

std::string Base64(std::string_view data)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t chunk = uint8_t(data[i]) << 16;
        if (i + 1 < data.size()) chunk |= uint8_t(data[i + 1]) << 8;
        if (i + 2 < data.size()) chunk |= uint8_t(data[i + 2]);
        out += alphabet[(chunk >> 18) & 63];
        out += alphabet[(chunk >> 12) & 63];
        out += i + 1 < data.size() ? alphabet[(chunk >> 6) & 63] : '=';
        out += i + 2 < data.size() ? alphabet[chunk & 63] : '=';
    }
    return out;
}

std::string FaviconKey(std::string_view url)
{
    size_t start;
    if (url.rfind("https://", 0) == 0) start = 8;
    else if (url.rfind("http://", 0) == 0) start = 7;
    else return { };
    auto authority = url.substr(start, url.find_first_of("/?#", start) - start);
    if (auto at = authority.rfind('@'); at != std::string_view::npos) authority.remove_prefix(at + 1);
    std::string_view host = authority;
    if (!host.empty() && host.front() == '[') host = host.substr(0, host.find(']') + 1);
    else host = host.substr(0, host.find(':'));
    std::string key;
    for (unsigned char c : host) {
        if (c >= 'A' && c <= 'Z') key += char(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-') key += char(c);
        else key += '_';
    }
    if (key.size() > 253 || key.find_first_not_of("._") == std::string::npos) return { };
    return key;
}
}
