// WebHID report descriptor parser and eligibility checks. Build with the
// engine's UIProcess/haiku/usb directory on the include path:
//   c++ -std=c++20 -I .cache/WebKit/Source/WebKit/UIProcess/haiku/usb tests/HIDDescriptorTests.cpp
// The descriptors were read from devices on the Framework laptop (#122).
#include "HIDReportDescriptorHaiku.h"
#include <cstdio>
using namespace WebKit::HIDHaiku;

static unsigned checks;
static unsigned failures;
static void check(bool condition, const char* name)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", name);
    }
}

// Keychron Q6 (3434:0161), interface 1: QMK's raw HID channel that VIA uses.
static const uint8_t keychronRaw[] = {
    0x06, 0x60, 0xff, 0x09, 0x61, 0xa1, 0x01, 0x09, 0x62, 0x15, 0x00, 0x26, 0xff, 0x00, 0x95, 0x20,
    0x75, 0x08, 0x81, 0x02, 0x09, 0x63, 0x15, 0x00, 0x26, 0xff, 0x00, 0x95, 0x20, 0x75, 0x08, 0x91,
    0x02, 0xc0,
};
// Keychron Q6 interface 0: the boot keyboard.
static const uint8_t keychronKeyboard[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x08, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x05, 0x07, 0x19, 0x00,
    0x29, 0xff, 0x15, 0x00, 0x26, 0xff, 0x00, 0x95, 0x06, 0x75, 0x08, 0x81, 0x00, 0x05, 0x08, 0x19,
    0x01, 0x29, 0x05, 0x95, 0x05, 0x75, 0x01, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0xc0,
};
// Keychron Q6 interface 2: mouse, system control, consumer and NKRO keyboard
// collections with report IDs.
static const uint8_t keychronShared[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x08, 0x15, 0x00, 0x25, 0x01, 0x95, 0x08, 0x75, 0x01, 0x81, 0x02, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x95, 0x02, 0x75, 0x08, 0x81, 0x06, 0x09, 0x38, 0x15, 0x81,
    0x25, 0x7f, 0x95, 0x01, 0x75, 0x08, 0x81, 0x06, 0x05, 0x0c, 0x0a, 0x38, 0x02, 0x15, 0x81, 0x25,
    0x7f, 0x95, 0x01, 0x75, 0x08, 0x81, 0x06, 0xc0, 0xc0, 0x05, 0x01, 0x09, 0x80, 0xa1, 0x01, 0x85,
    0x03, 0x19, 0x01, 0x2a, 0xb7, 0x00, 0x15, 0x01, 0x26, 0xb7, 0x00, 0x95, 0x01, 0x75, 0x10, 0x81,
    0x00, 0xc0, 0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x04, 0x19, 0x01, 0x2a, 0xa0, 0x02, 0x15,
    0x01, 0x26, 0xa0, 0x02, 0x95, 0x01, 0x75, 0x10, 0x81, 0x00, 0xc0, 0x05, 0x01, 0x09, 0x06, 0xa1,
    0x01, 0x85, 0x06, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x95, 0x08, 0x75,
    0x01, 0x81, 0x02, 0x05, 0x07, 0x19, 0x00, 0x29, 0xef, 0x15, 0x00, 0x25, 0x01, 0x95, 0xf0, 0x75,
    0x01, 0x81, 0x02, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x95, 0x05, 0x75, 0x01, 0x91, 0x02, 0x95,
    0x01, 0x75, 0x03, 0x91, 0x01, 0xc0,
};
// SMSL USB DAC (152a:85dd): a vendor collection with one feature report.
static const uint8_t smslFeature[] = {
    0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x40,
    0x09, 0x01, 0xb1, 0x01, 0xc0,
};
// Realtek 0bda:1100: signed vendor input and output reports.
static const uint8_t realtekVendor[] = {
    0x06, 0xda, 0xff, 0x09, 0xda, 0xa1, 0x01, 0x09, 0xda, 0x19, 0x01, 0x29, 0x0e, 0x15, 0x81, 0x25,
    0x7f, 0x75, 0x08, 0x95, 0xc0, 0x81, 0x02, 0x05, 0xda, 0x19, 0x01, 0x29, 0x05, 0x75, 0x08, 0x95,
    0xc0, 0x91, 0x02, 0xc0,
};

int main()
{
    auto raw = parseReportDescriptor(keychronRaw);
    check(!!raw, "raw HID descriptor parses");
    if (raw) {
        check(raw->collections.size() == 1, "raw HID has one top-level collection");
        auto& collection = raw->collections[0];
        check(collection.usagePage == 0xff60 && collection.usage == 0x61, "VIA usage page and usage");
        check(collection.type == 1, "application collection");
        check(!raw->usesReportIds, "no report IDs");
        check(raw->reportLength(ReportType::Input, 0) == 32u, "32-byte input report");
        check(raw->reportLength(ReportType::Output, 0) == 32u, "32-byte output report");
        check(!raw->reportLength(ReportType::Feature, 0), "no feature report");
        check(collection.inputReports.size() == 1 && collection.outputReports.size() == 1, "one report each way");
        if (collection.inputReports.size() == 1 && collection.inputReports[0].items.size() == 1) {
            auto& item = collection.inputReports[0].items[0];
            check(item.usages.size() == 1 && item.usages[0] == 0xff600062, "input usage is extended with its page");
            check(!item.isArray && item.isAbsolute && !item.isConstant, "data, variable, absolute");
            check(item.reportSize == 8 && item.reportCount == 32, "8 x 32 bits");
            check(item.logicalMinimum == 0 && item.logicalMaximum == 255, "two-byte logical maximum is 255");
        } else
            check(false, "one input item");
        check(eligible(*raw), "vendor raw HID interface is eligible");
    }

    auto keyboard = parseReportDescriptor(keychronKeyboard);
    check(keyboard && keyboard->collections.size() == 1 && keyboard->collections[0].usagePage == 1
        && keyboard->collections[0].usage == 6, "boot keyboard collection");
    check(keyboard && keyboard->reportLength(ReportType::Input, 0) == 8u, "8-byte boot keyboard input");
    check(keyboard && keyboard->reportLength(ReportType::Output, 0) == 1u, "1-byte LED output");
    check(keyboard && !eligible(*keyboard), "keyboards are protected");

    auto shared = parseReportDescriptor(keychronShared);
    check(!!shared, "shared endpoint descriptor parses");
    if (shared) {
        check(shared->usesReportIds, "report IDs are used");
        check(shared->collections.size() == 4, "four top-level collections");
        check(shared->collections[0].children.size() == 1 && shared->collections[0].children[0].usage == 1
            && shared->collections[0].children[0].type == 0, "mouse has a physical pointer child");
        check(shared->collections[0].inputReports.empty() && shared->collections[0].children[0].inputReports.size() == 1,
            "pointer reports belong to the child collection");
        check(shared->reportLength(ReportType::Input, 2) == 5u, "mouse report 2 is 5 bytes");
        check(shared->reportLength(ReportType::Input, 3) == 2u, "system control report 3 is 2 bytes");
        check(shared->reportLength(ReportType::Input, 6) == 31u, "NKRO report 6 is 31 bytes");
        check(shared->maximumReportLength(ReportType::Input) == 31u, "largest input report");
        auto& system = shared->collections[1];
        check(system.usagePage == 1 && system.usage == 0x80, "system control collection");
        if (!system.inputReports.empty() && !system.inputReports[0].items.empty()) {
            auto& item = system.inputReports[0].items[0];
            check(item.isRange && item.usageMinimum == 0x10001 && item.usageMaximum == 0x100b7, "usage range");
            check(item.isArray, "array item");
        } else
            check(false, "system control item");
        if (!shared->collections[0].children.empty() && shared->collections[0].children[0].inputReports.size() == 1) {
            auto& items = shared->collections[0].children[0].inputReports[0].items;
            check(items.size() == 4 && items[1].logicalMinimum == -127 && !items[1].isAbsolute, "signed relative axes");
            check(items.size() == 4 && items[3].usages.size() == 1 && items[3].usages[0] == 0x000c0238, "extended AC Pan usage");
        } else
            check(false, "pointer items");
        check(!eligible(*shared), "an interface with protected collections is not eligible");
    }

    auto feature = parseReportDescriptor(smslFeature);
    check(feature && feature->reportLength(ReportType::Feature, 0) == 64u, "64-byte feature report");
    check(feature && feature->collections.size() == 1 && feature->collections[0].featureReports.size() == 1, "feature report listed");
    check(feature && feature->collections[0].featureReports[0].items[0].isConstant, "constant feature item");
    check(feature && eligible(*feature), "vendor feature interface is eligible");

    auto realtek = parseReportDescriptor(realtekVendor);
    check(realtek && realtek->reportLength(ReportType::Input, 0) == 192u && realtek->reportLength(ReportType::Output, 0) == 192u,
        "192-byte vendor reports");
    check(realtek && realtek->collections[0].inputReports[0].items[0].logicalMinimum == -127, "signed vendor minimum");
    check(realtek && realtek->collections[0].outputReports[0].items[0].isRange
        && realtek->collections[0].outputReports[0].items[0].usageMinimum == 0x00da0001, "output usage range uses the one-byte page set before it");

    // Malformed descriptors.
    const uint8_t truncated[] = { 0x06, 0x60 };
    check(!parseReportDescriptor(truncated), "truncated item");
    const uint8_t mainOutside[] = { 0x75, 0x08, 0x95, 0x01, 0x81, 0x02 };
    check(!parseReportDescriptor(mainOutside), "report item outside a collection");
    const uint8_t zeroId[] = { 0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x00, 0xc0 };
    check(!parseReportDescriptor(zeroId), "report ID zero");
    std::vector<uint8_t> deep;
    for (int i = 0; i < 40; ++i) {
        deep.push_back(0xa1);
        deep.push_back(0x00);
    }
    check(!parseReportDescriptor(deep), "nesting is bounded");
    const uint8_t pop[] = { 0xb4 };
    check(!parseReportDescriptor(pop), "pop without push");
    std::vector<uint8_t> large = { 0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01 };
    for (int i = 0; i < 40; ++i) {
        for (uint8_t b : { 0x75, 0xff, 0x96, 0xff, 0xff, 0x81, 0x02 })
            large.push_back(b);
    }
    large.push_back(0xc0);
    check(!parseReportDescriptor(large), "report length is bounded");
    const uint8_t unclosed[] = { 0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02 };
    auto lenient = parseReportDescriptor(unclosed);
    check(lenient && lenient->reportLength(ReportType::Input, 0) == 4u, "a missing final End Collection is tolerated");
    const uint8_t longItem[] = { 0xfe, 0x02, 0x10, 0xaa, 0xbb, 0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0xc0 };
    check(!!parseReportDescriptor(longItem), "long items are skipped");

    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
