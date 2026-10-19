#include "StoreOffer.h"
#include "StoreOfferLogo.h"
#include <nlohmann/json.hpp>
#include <algorithm>

namespace summit {
namespace {
constexpr const char* kStyle = R"css(
:host { all: initial; }
.card { position: fixed; right: 24px; bottom: 24px; z-index: 2147483647; box-sizing: border-box;
  display: flex; align-items: center; gap: 14px; max-width: min(560px, calc(100vw - 32px));
  padding: 10px 12px 10px 10px; border-radius: 999px; background: #fff; color: #1f2328;
  box-shadow: 0 8px 28px rgba(0, 0, 0, .18), 0 0 0 1px rgba(0, 0, 0, .08);
  font: 400 14px/1.35 system-ui, -apple-system, "Noto Sans", "DejaVu Sans", sans-serif;
  text-align: left; animation: summit-offer-in .18s ease-out; }
@keyframes summit-offer-in { from { opacity: 0; transform: translateY(8px); } }
.logo { flex: none; width: 48px; height: 48px; border-radius: 50%; display: flex;
  align-items: center; justify-content: center; overflow: hidden; background: #eef4fa;
  box-shadow: inset 0 0 0 1px rgba(0, 0, 0, .06); }
.logo svg { width: 42px; height: 42px; display: block; }
.text { flex: 1 1 auto; min-width: 0; }
.name, .detail { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.name { font-weight: 600; font-size: 15px; }
.detail { color: #59636e; font-size: 13px; }
.action { flex: none; padding: 8px 16px; border-radius: 999px; background: #1a6bd6; color: #fff;
  font-weight: 600; text-decoration: none; white-space: nowrap; cursor: pointer; }
.action:hover { background: #1559b4; }
.action:focus-visible, .close:focus-visible { outline: 2px solid #1a6bd6; outline-offset: 2px; }
.close { flex: none; width: 30px; height: 30px; border-radius: 50%; display: flex;
  align-items: center; justify-content: center; color: #3b4148; cursor: pointer; }
.close:hover { background: rgba(0, 0, 0, .07); }
.close svg { width: 16px; height: 16px; fill: none; stroke: currentColor; stroke-width: 2; stroke-linecap: round; }
@media (prefers-color-scheme: dark) {
  .card { background: #23272e; color: #e8eaed; box-shadow: 0 8px 28px rgba(0, 0, 0, .5), 0 0 0 1px rgba(255, 255, 255, .1); }
  .logo { background: #2f3640; }
  .detail { color: #a7b0ba; }
  .action { background: #3b82f6; }
  .action:hover { background: #2f6fd8; }
  .close { color: #c9d1d9; }
  .close:hover { background: rgba(255, 255, 255, .1); }
}
@media (max-width: 520px) { .card { left: 12px; right: 12px; bottom: 12px; } }
)css";

// %OFFER% is the card's text and links as JSON, or null.
constexpr const char* kScript = R"js((() => {
const offer = %OFFER%;
for (const old of document.querySelectorAll('[data-summit-extension-offer]')) old.remove();
if (!offer || !document.documentElement) return 'removed';
const build = (node) => {
  const element = document.createElementNS('http://www.w3.org/2000/svg', node[0]);
  for (const [name, value] of Object.entries(node[1])) element.setAttribute(name, value);
  for (const child of node[2] || []) element.appendChild(build(child));
  return element;
};
const make = (tag, className, text) => {
  const element = document.createElement(tag);
  element.className = className;
  if (text) element.textContent = text;
  return element;
};
const host = document.createElement('div');
host.setAttribute('data-summit-extension-offer', '');
host.style.setProperty('all', 'initial', 'important');
const root = host.attachShadow({ mode: 'closed' });
const sheet = new CSSStyleSheet();
sheet.replaceSync(%STYLE%);
root.adoptedStyleSheets = [sheet];
const card = make('div', 'card');
card.setAttribute('role', 'dialog');
card.setAttribute('aria-label', 'Summit');
const logo = make('div', 'logo');
const icon = build(%LOGO%);
icon.setAttribute('aria-hidden', 'true');
logo.appendChild(icon);
const text = make('div', 'text');
text.append(make('div', 'name', offer.name), make('div', 'detail', offer.detail));
const action = make('a', 'action', offer.label);
action.href = offer.action;
const close = make('a', 'close');
close.href = offer.dismiss;
close.title = 'Close';
close.setAttribute('aria-label', 'Close');
close.appendChild(build(['svg', { viewBox: '0 0 16 16', 'aria-hidden': 'true' }, [['path', { d: 'M4 4l8 8M12 4l-8 8' }]]]));
card.append(logo, text, action, close);
root.appendChild(card);
document.documentElement.appendChild(host);
return 'shown';
})())js";

void Replace(std::string& text, std::string_view placeholder, const std::string& value)
{
    const auto at = text.find(placeholder);
    if (at != std::string::npos) text.replace(at, placeholder.size(), value);
}
}

std::string StoreOfferLink(const std::string& action, const std::string& token)
{
    return std::string(kStoreOfferScheme) + ":" + action + "?token=" + token;
}

std::optional<StoreOfferChoice> ParseStoreOfferLink(std::string_view url)
{
    const std::string_view scheme = kStoreOfferScheme;
    if (url.size() <= scheme.size() + 1 || url.substr(0, scheme.size()) != scheme || url[scheme.size()] != ':')
        return std::nullopt;
    url.remove_prefix(scheme.size() + 1);
    const auto query = url.find("?token=");
    if (query == std::string_view::npos) return std::nullopt;
    StoreOfferChoice choice { std::string(url.substr(0, query)), std::string(url.substr(query + 7)) };
    if ((choice.action != "install" && choice.action != "manage" && choice.action != "dismiss") || choice.token.empty()
        || !std::all_of(choice.token.begin(), choice.token.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        return std::nullopt;
    return choice;
}

std::string StoreOfferScript(const StoreOffer* offer)
{
    std::string script = kScript;
    nlohmann::json data = nullptr;
    if (offer) {
        data = {
            {"name", offer->name.empty() ? "This extension" : offer->name},
            {"detail", !offer->installed ? "Available for Summit"
                : offer->disabled ? "Installed in Summit, turned off" : "Installed in Summit"},
            {"label", offer->installed ? "Manage Extension" : "Install Extension"},
            {"action", StoreOfferLink(offer->installed ? "manage" : "install", offer->token)},
            {"dismiss", StoreOfferLink("dismiss", offer->token)},
        };
    }
    // Last placeholder first: the name (from the page) is put in last, so
    // nothing in it can be taken for a placeholder. Invalid UTF-8 in a page
    // title must not break the script.
    Replace(script, "%LOGO%", kStoreOfferLogo);
    Replace(script, "%STYLE%", nlohmann::json(kStyle).dump());
    Replace(script, "%OFFER%", data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    return script;
}

std::string StoreItemNameFromTitle(std::string_view title)
{
    for (std::string_view suffix : { " - Chrome Web Store", " – Get this Extension for", " – Add-ons for Firefox" }) {
        if (const auto at = title.find(suffix); at != std::string_view::npos) {
            title = title.substr(0, at);
            break;
        }
    }
    while (!title.empty() && (title.back() == ' ' || title.back() == '\t')) title.remove_suffix(1);
    while (!title.empty() && (title.front() == ' ' || title.front() == '\t')) title.remove_prefix(1);
    return std::string(title);
}
}
