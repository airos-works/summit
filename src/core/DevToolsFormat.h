#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Formatting for the developer tools: what a response body is, and how it is
// laid out for reading. Nothing here needs Haiku; tests/DevToolsTests.cpp
// covers it on the build host.
//
// Kiri (the editor these tools share their look with) parses JSON with the
// same vendored nlohmann library and shows sources with Scintilla and Lexilla;
// it formats documents by running Prettier, which a browser cannot expect to
// be installed, so the three formatters below are Summit's own.
namespace summit::devtools {

enum class BodyKind { Text, JSON, XML, HTML, JavaScript, CSS, Image, Binary };

// From the Content-Type (parameters and case are ignored), and from the body
// itself when the type says little: APIs often send JSON as text/plain.
BodyKind DetectBodyKind(std::string_view mimeType, std::string_view body);
// "JSON", "HTML"... for labels.
const char* BodyKindName(BodyKind kind);
// The Lexilla lexer that colours the kind, as Kiri chooses them; "null" for none.
const char* BodyKindLexer(BodyKind kind);
// JSON, XML and HTML are the kinds FormatBody() lays out.
bool CanFormat(BodyKind kind);

struct FormatResult {
    std::string text;
    // False when the body was left as it came: not a kind that is formatted,
    // or not valid (JSON). note then says why, for the status line.
    bool formatted = false;
    std::string note;
};
FormatResult FormatBody(BodyKind kind, std::string_view body);

// Re-indents JSON by two spaces without changing a token: numbers keep their
// digits and strings their escapes, which parsing and printing again would
// not guarantee (1.0, 1e3 and integers over 64 bits). An anti-hijacking
// prefix such as )]}' stays on a line of its own. False if the text is not
// JSON; out is then unchanged.
bool FormatJSON(std::string_view text, std::string& out);
// One element per line, two spaces per level. An element that holds only text
// stays on one line; comments, CDATA sections and processing instructions are
// copied as they are. Malformed input is laid out as well as it can be:
// nothing but white space between elements is ever dropped.
std::string FormatXML(std::string_view text);
// As FormatXML, knowing HTML: void elements, end tags that may be left out,
// inline elements that stay in their line of text, script and style contents
// that are re-indented but not otherwise touched, and pre and textarea
// contents that are not touched at all.
std::string FormatHTML(std::string_view text);

// A script laid out for reading (the Sources tab's pretty printing): a
// statement or block per line, four spaces per level, tokens untouched. Each
// line remembers where its first token was in the source, so breakpoints and
// the paused line can move between the two.
struct SourcePosition {
    int line = 0, column = 0;
    bool operator<(const SourcePosition& other) const { return line != other.line ? line < other.line : column < other.column; }
};
struct PrettyScript {
    std::string text;
    // Of each line of text, its first token's place in the source.
    std::vector<SourcePosition> starts;
    // The line of text showing a place in the source.
    int LineOf(SourcePosition position) const;
};
PrettyScript FormatJavaScript(std::string_view source);

// "512 B", "1.4 KB", "2.31 MB".
std::string FormatBytes(uint64_t bytes);
// "120 ms", "1.24 s", "2.1 min"; empty for a negative duration.
std::string FormatDuration(double seconds);
// False if the text is not base64 (white space is skipped).
bool DecodeBase64(std::string_view text, std::string& out);
// Sixteen bytes a line: offset, hexadecimal, printable ASCII. At most limit
// bytes are shown; a last line says how many were not.
std::string HexDump(std::string_view bytes, size_t limit = 64 * 1024);
// Replaces bytes that are not UTF-8 with U+FFFD, so that any body can be
// handed to a text view.
std::string ValidUTF8(std::string_view text);
}
