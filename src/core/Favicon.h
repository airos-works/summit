#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace summit {
// Straight (not premultiplied) 8-bit RGBA, top row first.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    bool Empty() const { return width <= 0 || height <= 0; }
};

enum class IcoEntryKind { None, PNG, DIB };
struct IcoEntry {
    IcoEntryKind kind = IcoEntryKind::None;
    std::string_view bytes;
    int width = 0, height = 0;
};

bool IsIco(std::string_view data);
// The entry closest to wanted pixels (preferring larger over smaller, then
// deeper colour). PNG entries still need an image decoder.
IcoEntry SelectIcoEntry(std::string_view data, int wanted = 32);
// Decodes an uncompressed 1/4/8/24/32-bit DIB from an .ico, with its mask.
bool DecodeDIB(std::string_view dib, Image& out);
// Whether the data is an SVG image (an <svg> element near the start).
bool IsSvg(std::string_view data);
// Draws an SVG image into a size x size square (keeping its aspect ratio,
// centred); false for one that cannot be read or draws nothing.
bool RasterizeSvg(std::string_view data, int size, Image& out);
// Fits the image into a size x size square (keeping its aspect ratio,
// centred) by area averaging.
Image ScaleImage(const Image& image, int size);
// An uncompressed (stored-deflate) RGBA PNG; small icons need nothing better.
std::string EncodePNG(const Image& image);
std::string Base64(std::string_view data);
// The lowercase host of an http(s) URL, the key icons are cached under, or
// empty. The result only contains [a-z0-9.-_] so it is also a file name.
std::string FaviconKey(std::string_view url);
}
