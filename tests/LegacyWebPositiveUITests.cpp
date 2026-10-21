// Native UI driver for the installed WebPositive; no engine callbacks are replaced.
#include <Application.h>
#include <Bitmap.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>
#include <Screen.h>
#include <View.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using JSON = nlohmann::json;

static BMessage
Ask(BMessenger target, BMessage message)
{
    BMessage reply;
    target.SendMessage(&message, &reply, 1000000, 1000000);
    return reply;
}

static BMessage
Property(BMessenger target, const char* name, uint32 what = B_GET_PROPERTY)
{
    BMessage message(what);
    message.AddSpecifier(name);
    return Ask(target, message);
}

static BMessenger
Window(BMessenger app, int32 index)
{
    BMessage message(B_GET_PROPERTY);
    message.AddSpecifier("Window", index);
    BMessenger result;
    Ask(app, message).FindMessenger("result", &result);
    return result;
}

static BMessenger
EngineView(BMessenger parent, unsigned depth = 0)
{
    if (depth > 16)
        return {};
    BMessage named(B_GET_PROPERTY);
    named.AddSpecifier("Messenger");
    named.AddSpecifier("View", "engine view");
    BMessenger direct;
    Ask(parent, named).FindMessenger("result", &direct);
    if (direct.IsValid() && !Property(direct, "Hidden").GetBool("result", true))
        return direct;
    int32 count = Property(parent, "View", B_COUNT_PROPERTIES).GetInt32("result", 0);
    for (int32 i = 0; i < count; ++i) {
        BMessage request(B_GET_PROPERTY);
        request.AddSpecifier("Messenger");
        request.AddSpecifier("View", i);
        BMessenger child;
        Ask(parent, request).FindMessenger("result", &child);
        if (!child.IsValid() || Property(child, "Hidden").GetBool("result", true))
            continue;
        auto found = EngineView(child, depth + 1);
        if (found.IsValid())
            return found;
    }
    return {};
}

static int
CaptureScreen(const char* path)
{
    BScreen screen;
    BRect frame = screen.Frame();
    BBitmap bitmap(frame, B_RGBA32);
    if (bitmap.InitCheck() != B_OK || screen.ReadBitmap(&bitmap, false, &frame) != B_OK)
        return 7;
    int width = frame.IntegerWidth() + 1;
    int height = frame.IntegerHeight() + 1;
    std::ofstream file(path, std::ios::binary);
    file << "P6\n" << width << " " << height << "\n255\n";
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto* pixel = static_cast<uint8*>(bitmap.Bits()) + y * bitmap.BytesPerRow() + x * 4;
            const char rgb[] = {char(pixel[2]), char(pixel[1]), char(pixel[0])};
            file.write(rgb, 3);
        }
    }
    return file.good() ? 0 : 8;
}

int
main(int argc, char** argv)
{
    if (argc != 3 && argc != 4)
        return 2;
    BApplication self("application/x-vnd.Summit-WebPositiveUIProbe");
    status_t status;
    BMessenger app(nullptr, atoi(argv[1]), &status);
    if (status != B_OK)
        return 3;
    std::string action = argv[2];
    if (action == "capture" && argc == 4)
        return CaptureScreen(argv[3]);
    if (action == "quit")
        return app.SendMessage(B_QUIT_REQUESTED) == B_OK ? 0 : 4;
    if (action != "inspect" && action != "click")
        return 2;

    JSON result = JSON::array();
    bool clicked = false;
    int32 count = Property(app, "Window", B_COUNT_PROPERTIES).GetInt32("result", 0);
    for (int32 i = 0; i < count; ++i) {
        auto window = Window(app, i);
        if (!window.IsValid())
            continue;
        std::string title = Property(window, "Title").GetString("result", "");
        bool isParent = title.find("Legacy popup test") != std::string::npos;
        bool isChild = title.find("Legacy child") != std::string::npos;
        auto view = isParent || isChild ? EngineView(window) : BMessenger();
        BRect frame;
        Property(window, "Frame").FindRect("result", &frame);
        JSON item = {{"index", i}, {"title", title}, {"engineView", view.IsValid()},
            {"frame", {frame.left, frame.top, frame.right, frame.bottom}}};
        if (view.IsValid()) {
            BRect viewFrame;
            Property(view, "Frame").FindRect("result", &viewFrame);
            item["viewFrame"] = {viewFrame.left, viewFrame.top, viewFrame.right, viewFrame.bottom};
        }
        if (action == "click" && !clicked && view.IsValid() && isParent) {
            for (uint32 what : {B_MOUSE_DOWN, B_MOUSE_UP}) {
                BMessage message(what);
                message.AddPoint("where", BPoint(108, 38));
                message.AddPoint("be:view_where", BPoint(108, 38));
                message.AddInt32("buttons", what == B_MOUSE_DOWN ? B_PRIMARY_MOUSE_BUTTON : 0);
                message.AddInt32("clicks", 1);
                message.AddInt32("modifiers", 0);
                message.AddInt64("when", system_time());
                if (view.SendMessage(&message) != B_OK)
                    return 5;
            }
            clicked = true;
        }
        if (action == "inspect" && isChild && frame.IsValid()) {
            BBitmap bitmap(frame, B_RGBA32);
            BScreen screen;
            if (bitmap.InitCheck() == B_OK && screen.ReadBitmap(&bitmap, false, &frame) == B_OK) {
                auto* pixel = static_cast<uint8*>(bitmap.Bits())
                    + (frame.IntegerHeight() / 2) * bitmap.BytesPerRow()
                    + (frame.IntegerWidth() / 2) * 4;
                item["centerRGB"] = {pixel[2], pixel[1], pixel[0]};
            }
        }
        result.push_back(item);
    }
    std::printf("%s\n", result.dump().c_str());
    return action == "click" && !clicked ? 6 : 0;
}
