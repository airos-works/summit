#pragma once
#include <GroupView.h>
#include <Message.h>
#include <string>
#include <utility>
#include <vector>

class BButton;
class BStringView;

namespace summit {
// A one-line bar above the page: a message and up to three buttons, whose
// messages go to the window. The window shows the selected tab's most
// important notice in it, or hides it.
class NoticeBar : public BGroupView {
public:
    NoticeBar();
    // Buttons: labels and the messages they send (taken over).
    void SetNotice(const std::string& text, std::vector<std::pair<std::string, BMessage*>> buttons);
    void Clear();
    const std::string& Text() const { return fText; }
    void Draw(BRect update) override;
private:
    BStringView* fLabel;
    std::vector<BButton*> fButtons;
    std::string fText;
};
}
