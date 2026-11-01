#pragma once
#include <GroupView.h>
#include <Messenger.h>
#include <memory>
#include <string>
#include <vector>

class BButton;
class BCheckBox;
class BColumnListView;
class BStringView;

namespace summit {
class SharedProfile;
// Preferences › AutoFill (#149): whether forms are filled and new entries
// offered for saving, and the saved addresses (in the profile, changed here
// directly) and payment cards (in the key store, read and written on threads
// of their own: AutoFillStore.h), which can be added, edited and removed.
// Card numbers are shown masked, and are never shown in full here. Below
// them, whether what is typed in forms is remembered (the form history,
// FormHistory.h), and a button that clears it.
class AutoFillPreferences : public BGroupView {
public:
    explicit AutoFillPreferences(std::shared_ptr<SharedProfile> profile);
    void AttachedToWindow() override;
    void DetachedFromWindow() override;
    // Forms sent in browser windows since add to the form history: its
    // count is read again when Preferences comes to the front.
    void WindowActivated(bool active) override;
    void MessageReceived(BMessage* message) override;

private:
    void ShowAddresses();
    void ShowCards();
    void ShowFormHistoryCount();
    // Opens the editor for an address or card (empty id: a new one).
    void EditAddress(const std::string& id);
    void EditCard(const std::string& id);
    std::shared_ptr<SharedProfile> fProfile;
    BCheckBox* fEnabled;
    BColumnListView* fAddresses;
    BColumnListView* fCards;
    BButton* fEditAddress;
    BButton* fRemoveAddress;
    BButton* fEditCard;
    BButton* fRemoveCard;
    BStringView* fCardStatus;
    BCheckBox* fFormHistory;
    BButton* fClearFormHistory;
    BStringView* fFormHistoryStatus;
    // The lists' rows in order: the addresses' identifiers, and the cards
    // as listed (without their numbers).
    std::vector<std::string> fAddressIDs;
    struct CardRow {
        std::string id, masked, name;
        int month = 0, year = 0;
    };
    std::vector<CardRow> fCardRows;
};
}
