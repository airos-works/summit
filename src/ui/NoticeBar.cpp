#include "NoticeBar.h"
#include <Button.h>
#include <LayoutBuilder.h>
#include <StringView.h>
#include <Window.h>

namespace summit {
NoticeBar::NoticeBar() : BGroupView("notice-bar", B_HORIZONTAL, 6)
{
    SetFlags(Flags() | B_WILL_DRAW);
    // A pale yellow information bar, as in other browsers, that still reads
    // in a dark system theme.
    const rgb_color panel = ui_color(B_PANEL_BACKGROUND_COLOR);
    const bool dark = panel.red + panel.green + panel.blue < 3 * 128;
    SetViewColor(dark ? rgb_color{78, 70, 40, 255} : rgb_color{255, 244, 196, 255});
    fLabel = new BStringView("notice", "");
    fLabel->SetHighColor(dark ? rgb_color{246, 238, 210, 255} : rgb_color{48, 40, 10, 255});
    fLabel->SetTruncation(B_TRUNCATE_END);
    fLabel->SetExplicitMinSize(BSize(60, B_SIZE_UNSET));
    fLabel->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    BLayoutBuilder::Group<>(this).SetInsets(10, 3, 6, 3).Add(fLabel, 1);
    for (int i = 0; i < 3; ++i) {
        auto* button = new BButton("notice-button", "", nullptr);
        button->Hide();
        GroupLayout()->AddView(button);
        fButtons.push_back(button);
    }
}
void NoticeBar::SetNotice(const std::string& text, std::vector<std::pair<std::string, BMessage*>> buttons)
{
    fText = text;
    fLabel->SetText(text.c_str());
    for (size_t i = 0; i < fButtons.size(); ++i) {
        auto* button = fButtons[i];
        if (i < buttons.size()) {
            button->SetLabel(buttons[i].first.c_str());
            button->SetMessage(buttons[i].second);
            if (Window()) button->SetTarget(Window());
            if (button->IsHidden(button)) button->Show();
        } else {
            button->SetMessage(nullptr);
            if (!button->IsHidden(button)) button->Hide();
        }
    }
    for (size_t i = fButtons.size(); i < buttons.size(); ++i) delete buttons[i].second;
    if (IsHidden(this)) Show();
}
void NoticeBar::Clear()
{
    fText.clear();
    if (!IsHidden(this)) Hide();
}
void NoticeBar::Draw(BRect update)
{
    BGroupView::Draw(update);
    SetHighColor(tint_color(ViewColor(), B_DARKEN_2_TINT));
    StrokeLine(Bounds().LeftBottom(), Bounds().RightBottom());
}
}
