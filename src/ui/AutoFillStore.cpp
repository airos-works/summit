#include "AutoFillStore.h"
#include <DataIO.h>
#include <Key.h>
#include <KeyStore.h>
#include <Message.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <random>

namespace summit {
namespace {
constexpr const char* kKeyring = "Summit cards";

std::string Normalized(const std::string& text)
{
    std::string result;
    bool space = false;
    for (unsigned char character : text) {
        if (std::isspace(character)) { space = !result.empty(); continue; }
        if (space) result += ' ';
        space = false;
        result += static_cast<char>(std::tolower(character));
    }
    return result;
}
}

namespace CardStore {
std::vector<AutoFillCard> Cards()
{
    std::vector<AutoFillCard> cards;
    BKeyStore store;
    uint32 cookie = 0;
    BKey key;
    // A keyring that does not exist yet (no card was ever saved) answers at
    // once, without asking the user to allow Summit.
    while (store.GetNextKey(kKeyring, B_KEY_TYPE_GENERIC, B_KEY_PURPOSE_WEB, cookie, key) == B_OK) {
        if (!key.Identifier() || !key.Data()) continue;
        BMemoryIO input(key.Data(), key.DataLength());
        BMessage record;
        if (record.Unflatten(&input) != B_OK) continue;
        AutoFillCard card { key.Identifier(), record.GetString("name", ""), record.GetString("number", ""),
            static_cast<int>(record.GetInt32("month", 0)), static_cast<int>(record.GetInt32("year", 0)) };
        if (CardNumberDigits(card.number).empty()) continue;
        cards.push_back(std::move(card));
        if (cards.size() >= 1000) break;
    }
    return cards;
}

std::string Save(AutoFillCard& card)
{
    card.number = CardNumberDigits(card.number);
    if (card.number.empty()) return "That is not a card number.";
    if (card.id.empty()) card.id = NewAutoFillID();
    BMessage record;
    record.AddString("name", card.name.c_str());
    record.AddString("number", card.number.c_str());
    record.AddInt32("month", card.month);
    record.AddInt32("year", card.year);
    BMallocIO data;
    if (record.Flatten(&data) != B_OK) return "The card could not be stored.";
    BKeyStore store;
    // The keyring is made on first use; it exists from then on.
    store.AddKeyring(kKeyring);
    Remove(card.id);
    BKey key(B_KEY_PURPOSE_WEB, card.id.c_str(), nullptr, static_cast<const uint8*>(data.Buffer()), data.BufferLength());
    const status_t status = store.AddKey(kKeyring, key);
    return status == B_OK ? std::string() : std::string(strerror(status));
}

bool Remove(const std::string& id)
{
    // Found by enumeration, as PasskeyStore::Remove does: keystore_server
    // asks the user once per kind of access, and listing already asked.
    BKeyStore store;
    uint32 cookie = 0;
    BKey key;
    while (store.GetNextKey(kKeyring, B_KEY_TYPE_GENERIC, B_KEY_PURPOSE_WEB, cookie, key) == B_OK) {
        if (key.Identifier() && id == key.Identifier()) return store.RemoveKey(kKeyring, key) == B_OK;
    }
    return false;
}
}

std::string CardNumberDigits(const std::string& text)
{
    std::string digits;
    for (unsigned char character : text) {
        if (std::isdigit(character)) digits += static_cast<char>(character);
        else if (character != ' ' && character != '-') return {};
    }
    if (digits.size() < 12 || digits.size() > 19) return {};
    int sum = 0;
    for (size_t i = 0; i < digits.size(); ++i) {
        int digit = digits[digits.size() - 1 - i] - '0';
        if (i % 2) digit = digit * 2 > 9 ? digit * 2 - 9 : digit * 2;
        sum += digit;
    }
    return sum % 10 ? std::string() : digits;
}

std::string CardBrand(const std::string& number)
{
    auto prefix = [&](size_t length) { return number.size() >= length ? std::stoi(number.substr(0, length)) : -1; };
    if (number.empty() || !std::isdigit(static_cast<unsigned char>(number[0]))) return "Card";
    if (number[0] == '4') return "Visa";
    if ((prefix(2) >= 51 && prefix(2) <= 55) || (prefix(4) >= 2221 && prefix(4) <= 2720)) return "Mastercard";
    if (prefix(2) == 34 || prefix(2) == 37) return "American Express";
    if (prefix(4) == 6011 || prefix(2) == 65 || (prefix(3) >= 644 && prefix(3) <= 649)) return "Discover";
    if (prefix(2) == 35) return "JCB";
    if (prefix(2) == 36 || prefix(2) == 38 || prefix(2) == 30) return "Diners Club";
    if (prefix(2) == 62) return "UnionPay";
    return "Card";
}

std::string MaskedCard(const AutoFillCard& card)
{
    const std::string last = card.number.size() >= 4 ? card.number.substr(card.number.size() - 4) : card.number;
    return CardBrand(card.number) + " •••• " + last;
}

std::string CardExpiry(const AutoFillCard& card)
{
    if (card.month < 1 || card.month > 12 || card.year < 2000) return {};
    char text[16];
    std::snprintf(text, sizeof(text), "%02d/%04d", card.month, card.year);
    return text;
}

std::string NewAutoFillID()
{
    static constexpr char digits[] = "0123456789abcdef";
    std::random_device random;
    std::string id;
    for (int i = 0; i < 16; ++i) id += digits[random() % 16];
    return id;
}

std::string AddressName(const AutoFillAddress& address)
{
    std::string name = address.givenName;
    if (!address.familyName.empty()) name += (name.empty() ? "" : " ") + address.familyName;
    if (!address.organization.empty()) name += (name.empty() ? "" : ", ") + address.organization;
    return name;
}

std::string AddressSummary(const AutoFillAddress& address)
{
    std::string summary;
    auto add = [&](std::string part) {
        for (size_t at; (at = part.find('\n')) != std::string::npos;) part.replace(at, 1, ", ");
        if (!part.empty()) summary += (summary.empty() ? "" : ", ") + part;
    };
    add(address.street);
    add(address.city);
    add(address.region);
    add(address.postalCode);
    add(address.country);
    if (summary.empty()) add(address.email);
    if (summary.empty()) add(address.tel);
    return summary;
}

bool AddressCovers(const AutoFillAddress& saved, const AutoFillAddress& submitted)
{
    for (const auto& [name, field] : AutoFillAddressFields()) {
        const std::string value = Normalized(submitted.*field);
        if (value.empty()) continue;
        // Street lines may be joined with commas on one form, with new lines
        // on another.
        std::string mine = Normalized(saved.*field), theirs = value;
        if (field == &AutoFillAddress::street) {
            std::erase_if(mine, [](char c) { return c == ',' || c == ' '; });
            std::erase_if(theirs, [](char c) { return c == ',' || c == ' '; });
        }
        if (field == &AutoFillAddress::tel) {
            std::erase_if(mine, [](char c) { return !std::isdigit(static_cast<unsigned char>(c)); });
            std::erase_if(theirs, [](char c) { return !std::isdigit(static_cast<unsigned char>(c)); });
        }
        if (mine != theirs) return false;
    }
    return true;
}

bool AddressWorthSaving(const AutoFillAddress& address)
{
    const bool name = !address.givenName.empty() || !address.familyName.empty();
    return (!address.street.empty() && (!address.city.empty() || !address.postalCode.empty()))
        || (name && (!address.email.empty() || !address.tel.empty()));
}
}
