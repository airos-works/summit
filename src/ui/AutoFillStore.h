#pragma once
#include "core/Profile.h"
#include <cstdint>
#include <string>
#include <vector>

namespace summit {
// AutoFill (#149): contact cards and addresses (AutoFillAddress, in the
// profile) and payment cards, which are kept in the Haiku key store in the
// keyring "Summit cards", apart from the passwords: a generic key per card
// with its identifier as the key's, and the name, number and expiry in a
// flattened BMessage. The security code (CVC) is never stored. Like
// PasswordStore, every CardStore call may wait for the user (to allow Summit
// or unlock the keyring): never call them on a window's thread.
struct AutoFillCard {
    std::string id; // letters and digits
    std::string name, number; // number: digits only
    int month = 0, year = 0;  // 1-12 and four digits; 0 when not known
    bool operator==(const AutoFillCard&) const = default;
};
namespace CardStore {
std::vector<AutoFillCard> Cards();
// Adds the card, or replaces the one with its identifier (one is made when
// it has none). An error message, or empty.
std::string Save(AutoFillCard& card);
bool Remove(const std::string& id);
}

// Digits only; empty unless they make a card number (12-19 digits that pass
// the Luhn check).
std::string CardNumberDigits(const std::string& text);
// "Visa", "Mastercard"... from the number's first digits, or "Card".
std::string CardBrand(const std::string& number);
// "Visa •••• 4242": the number as the browser shows it.
std::string MaskedCard(const AutoFillCard& card);
// "04/2028", or empty.
std::string CardExpiry(const AutoFillCard& card);
// A new identifier for an address or card.
std::string NewAutoFillID();
// The address as one line, and its name ("Jane Doe, Example Ltd").
std::string AddressSummary(const AutoFillAddress& address);
std::string AddressName(const AutoFillAddress& address);
// Whether saved has every field submitted has (compared without case and
// extra spaces): a form filled from it, or with less, adds nothing new.
bool AddressCovers(const AutoFillAddress& saved, const AutoFillAddress& submitted);
// Whether the address has anything worth keeping: a street with a city or
// postal code, or a name with an e-mail address or telephone number.
bool AddressWorthSaving(const AutoFillAddress& address);
}
