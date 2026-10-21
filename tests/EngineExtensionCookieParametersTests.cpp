#include "config.h"
#include "JSWebExtensionCookieParameters.h"
#include "JSWebExtensionString.h"
#include "Protected.h"
#include "WebExtensionCookieStoreIdentifier.h"
#include "WebExtensionCookieQueryParser.h"
#include <Application.h>
#include <cstdio>
#include <limits>
#include <wtf/MainThread.h>
#include <wtf/text/MakeString.h>

using namespace WebKit;
static unsigned checks, failures;
static void check(bool result, const char* description)
{
    ++checks;
    failures += !result;
    std::printf("%s %s\n", result ? "PASS" : "FAIL", description);
}
static void checkIdentifiers()
{
    for (auto number : { uint64_t { 1 }, uint64_t { 9007199254740991 }, uint64_t { 9007199254740993 }, uint64_t { 9223372036854775806 } }) {
        for (bool ephemeral : { false, true }) {
            PAL::SessionID session(number | (ephemeral ? uint64_t { PAL::SessionID::EphemeralSessionMask } : 0));
            auto text = webExtensionCookieStoreIdentifier(session);
            auto restored = parseWebExtensionCookieStoreIdentifier(text);
            check(restored && *restored == session && restored->isEphemeral() == ephemeral, "store identifiers round-trip all 64 bits without floating-point conversion");
        }
    }
    for (auto invalid : { ""_s, "persistent-"_s, "ephemeral-"_s, "persistent-0"_s,
            "persistent-9223372036854775808"_s, "ephemeral-9223372036854775807"_s,
            "ephemeral-18446744073709551615"_s, "persistent-99999999999999999999999999"_s,
            "persistent--1"_s, "persistent-+1"_s, "persistent- 1"_s, "persistent-1 "_s,
            "persistent-1x"_s, "persistent-1.0"_s, "persistent-1e3"_s, "Persistent-1"_s,
            "1"_s, "ephemeral-1/persistent-1"_s })
        check(!parseWebExtensionCookieStoreIdentifier(invalid), "malformed stores cannot alias another session or privacy class");
    check(!parseWebExtensionCookieStoreIdentifier(String::fromUTF8("persistent-１")), "store IDs accept ASCII digits only");
    auto withNull = String::fromUTF8(std::span("persistent-1\0suffix", 19));
    check(!parseWebExtensionCookieStoreIdentifier(withNull), "store parsing consumes embedded nulls rather than accepting a prefix");
    check(webExtensionCookieStoreIdentifier(PAL::SessionID(0)).isNull()
        && webExtensionCookieStoreIdentifier(PAL::SessionID(std::numeric_limits<uint64_t>::max())).isNull(), "invalid internal session sentinels have no public store ID");
    auto maximum = parseWebExtensionCookieStoreIdentifier("persistent-9223372036854775807"_s);
    check(maximum && maximum->toUInt64() == 9223372036854775807ULL && !maximum->isEphemeral(), "largest persistent session is distinct from the ephemeral mask");
    auto leadingZero = parseWebExtensionCookieStoreIdentifier("persistent-0001"_s);
    check(leadingZero && webExtensionCookieStoreIdentifier(*leadingZero) == "persistent-1"_s, "accepted leading zeros serialize canonically");
    auto zeroEphemeral = parseWebExtensionCookieStoreIdentifier("ephemeral-0"_s);
    check(zeroEphemeral && zeroEphemeral->isValid() && zeroEphemeral->isEphemeral(), "the valid mask-only native ephemeral session remains representable");
}
static void checkQueries()
{
    auto parse = [](const String& text, WebExtensionCookieQueryKind kind = WebExtensionCookieQueryKind::GetAll) {
        auto json = JSON::Value::parseJSON(text);
        return parseWebExtensionCookieQuery(json.get(), kind);
    };
    for (auto invalid : { "null"_s, "[]"_s, "true"_s, "7"_s, "\"text\""_s })
        check(!parse(invalid), "cookie queries require a dictionary");
    auto all = parse("{}"_s);
    check(all && !all->session && !all->url.isValid() && !all->filter.name && !all->filter.secure && !all->filter.session,
        "empty getAll details leave every filter omitted");
    for (auto invalid : { "{}"_s, R"({"name":"n"})"_s, R"({"url":"https://example.test"})"_s })
        check(!parse(invalid, WebExtensionCookieQueryKind::Get), "single-cookie reads require both name and URL");
    auto get = parse(R"({"name":"","url":"https://example.test/a","storeId":"persistent-9007199254740993"})"_s, WebExtensionCookieQueryKind::Get);
    check(get && get->filter.name && get->filter.name->isEmpty() && get->session->toUInt64() == 9007199254740993ULL,
        "unnamed-cookie queries preserve name presence and full-width store IDs");
    auto filtered = parse(R"({"name":"n","domain":".example.test","path":"/a","secure":false,"session":true})"_s);
    check(filtered && filtered->filter.name == "n"_s && filtered->filter.domain == "example.test"_s
        && filtered->filter.path == "/a"_s && filtered->filter.secure == false && filtered->filter.session == true,
        "getAll preserves explicit false filters and normalizes a leading domain dot");
    auto ipv6 = parse(R"({"domain":"[::1]"})"_s);
    check(ipv6 && ipv6->filter.domain == "::1"_s, "IPv6 filter brackets normalize to the native cookie domain form");
    for (auto key : { "name"_s, "url"_s, "storeId"_s, "domain"_s, "path"_s }) {
        for (auto invalid : { "null"_s, "true"_s, "7"_s, "{}"_s, "[]"_s, "\"a\\u0000b\""_s })
            check(!parse(makeString("{\""_s, key, "\":"_s, invalid, '}')), "cookie query strings reject incorrect types and embedded nulls");
    }
    for (auto key : { "secure"_s, "session"_s }) {
        for (auto invalid : { "null"_s, "0"_s, "\"false\""_s, "[]"_s })
            check(!parse(makeString("{\""_s, key, "\":"_s, invalid, '}')), "boolean cookie filters cannot be coerced from other types");
    }
    for (auto invalid : { ""_s, "relative/path"_s, "file:///tmp/cookie"_s, "javascript:1"_s, "https://"_s })
        check(!parse(makeString("{\"url\":\""_s, invalid, "\"}"_s)), "cookie queries reject empty, malformed and non-HTTP URLs");
    check(!parse(R"({"storeId":"persistent-1suffix"})"_s), "query parsing enforces complete store-ID consumption");
    for (auto unknown : { "firstPartyDomain"_s, "typo"_s })
        check(!parse(makeString("{\""_s, unknown, "\":{}}"_s)), "unimplemented or unknown query dimensions cannot silently broaden a read");
    auto ordinary = parse("{}"_s);
    auto wildcard = parse(R"({"partitionKey":{}})"_s);
    auto partition = parse(R"({"partitionKey":{"topLevelSite":"https://sub.example.test:8443/page"}})"_s);
    check(ordinary && ordinary->filter.partitionKey && ordinary->filter.partitionKey->isEmpty(), "cookie queries default to the unpartitioned jar");
    check(wildcard && !wildcard->filter.partitionKey, "an explicit empty partition filter requests all jars");
    check(partition && partition->filter.partitionKey == "https://example.test"_s, "partition filters normalize to scheme and registrable domain");
    for (auto invalid : { "null"_s, "true"_s, R"({"topLevelSite":7})"_s, R"({"topLevelSite":"data:text/html,opaque"})"_s,
        R"({"topLevelSite":"https://example.test","hasCrossSiteAncestor":true})"_s })
        check(!parse(makeString("{\"partitionKey\":"_s, invalid, '}')), "invalid or unsupported partition dimensions fail instead of widening access");
    check(!parse(R"({"name":"n","url":"https://example.test","partitionKey":{}})"_s, WebExtensionCookieQueryKind::Get),
        "a single-cookie query cannot use a wildcard partition");
    check(!parse(R"({"name":"n","url":"https://example.test","secure":true})"_s, WebExtensionCookieQueryKind::Get), "getAll-only filters are rejected for a single-cookie query");
}
static void checkJavaScriptValues()
{
    JSRetainPtr<JSGlobalContextRef> retained(Adopt, JSGlobalContextCreate(nullptr));
    auto context = retained.get();
    auto publish = [&](ASCIILiteral key, JSValueRef value) {
        Protected<JSValueRef> protectedValue(context, value);
        JSObjectSetProperty(context, JSContextGetGlobalObject(context), toJSString(key).get(), value, kJSPropertyAttributeNone, nullptr);
    };
    auto verify = [&](const String& expression, const char* description) {
        JSValueRef exception = nullptr;
        Protected<JSValueRef> value(context, JSEvaluateScript(context, toJSString(expression).get(), nullptr, nullptr, 1, &exception));
        check(!exception && value.get() && JSValueIsBoolean(context, value.get()) && JSValueToBoolean(context, value.get()), description);
    };
    WebExtensionCookieParameters parameters { PAL::SessionID::defaultSessionID(), { } };
    auto& cookie = parameters.cookie;
    cookie.name = ""_s;
    cookie.value = String::fromUTF8("雪 \"quoted\"\nvalue");
    cookie.domain = "example.test"_s;
    cookie.path = "/a"_s;
    cookie.session = true;
    publish("cookie"_s, toWebAPI(context, parameters));
    verify("cookie.name === '' && cookie.domain === 'example.test' && cookie.path === '/a' && cookie.storeId === 'persistent-1'"_s, "cookie identity and empty names remain exact strings");
    verify(String::fromUTF8("cookie.value === '雪 \\\"quoted\\\"\\nvalue'"), "cookie values preserve Unicode, quotes and line breaks");
    verify("cookie.hostOnly === true && cookie.httpOnly === false && cookie.secure === false && cookie.session === true"_s, "explicit false and true cookie flags remain booleans");
    verify("cookie.sameSite === 'no_restriction' && !('expirationDate' in cookie) && Object.keys(cookie).length === 10"_s, "unexpiring cookies omit expiration instead of fabricating a timestamp");
    cookie.expires = 4000000000000.0;
    publish("cookie"_s, toWebAPI(context, parameters));
    verify("cookie.session === true && !('expirationDate' in cookie)"_s, "a session cookie never exposes contradictory internal expiry metadata");
    cookie.expires = std::nullopt;
    for (auto policy : { WebCore::Cookie::SameSitePolicy::None, WebCore::Cookie::SameSitePolicy::Lax, WebCore::Cookie::SameSitePolicy::Strict }) {
        cookie.sameSite = policy;
        publish("cookie"_s, toWebAPI(context, parameters));
        auto expected = policy == WebCore::Cookie::SameSitePolicy::None ? "no_restriction"_s : policy == WebCore::Cookie::SameSitePolicy::Lax ? "lax"_s : "strict"_s;
        verify(makeString("cookie.sameSite === '"_s, expected, '\''), "every native SameSite policy uses the API spelling");
    }
    cookie.domain = ".example.test"_s;
    cookie.httpOnly = true;
    cookie.secure = true;
    cookie.session = false;
    parameters.sessionIdentifier = PAL::SessionID::legacyPrivateSessionID();
    for (double expires : { 0.0, -1500.0, 1700000000123.0 }) {
        cookie.expires = expires;
        publish("cookie"_s, toWebAPI(context, parameters));
        verify(makeString("cookie.expirationDate === "_s, expires / 1000.0), "expiration converts milliseconds to seconds, including zero and past dates");
    }
    verify("cookie.hostOnly === false && cookie.httpOnly === true && cookie.secure === true && cookie.session === false && cookie.storeId === 'ephemeral-1'"_s, "domain cookies and private store identity keep their actual flags");
    for (double expires : { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() }) {
        cookie.expires = expires;
        publish("cookie"_s, toWebAPI(context, parameters));
        verify("!('expirationDate' in cookie)"_s, "invalid internal expiration values do not produce non-finite API dates");
    }
    cookie.expires = 1700000000123.0;
    WebCore::CookieChange change { cookie, false, WebCore::CookieChangeCause::Explicit };
    publish("change"_s, toWebAPICookieChange(context, PAL::SessionID::defaultSessionID(), change));
    verify("change.removed === false && change.cause === 'explicit' && Object.keys(change).length === 3"_s,
        "an addition has exactly cookie, removed and cause fields with correct types");
    verify("change.cookie.value === cookie.value && change.cookie.storeId === 'persistent-1' && change.cookie.httpOnly && change.cookie.secure && change.cookie.expirationDate === 1700000000.123"_s,
        "a change carries the actual cookie metadata and event store identity");
    for (auto cause : { WebCore::CookieChangeCause::Explicit, WebCore::CookieChangeCause::Overwrite,
                       WebCore::CookieChangeCause::Expired, WebCore::CookieChangeCause::ExpiredOverwrite }) {
        change.removed = true;
        change.cause = cause;
        auto expected = cause == WebCore::CookieChangeCause::Explicit ? "explicit"_s : cause == WebCore::CookieChangeCause::Overwrite ? "overwrite"_s
            : cause == WebCore::CookieChangeCause::Expired ? "expired"_s : "expired_overwrite"_s;
        publish("change"_s, toWebAPICookieChange(context, PAL::SessionID::legacyPrivateSessionID(), change));
        verify(makeString("change.removed === true && change.cause === '"_s, expected, "' && change.cookie.storeId === 'ephemeral-1'"_s),
            "every supported removal cause retains its API spelling and actual store");
    }
    change.removed = false;
    for (auto cause : { WebCore::CookieChangeCause::Overwrite, WebCore::CookieChangeCause::Expired, WebCore::CookieChangeCause::ExpiredOverwrite }) {
        change.cause = cause;
        check(JSValueIsNull(context, toWebAPICookieChange(context, PAL::SessionID::defaultSessionID(), change)),
            "a removal-only cause cannot produce a fabricated addition");
    }
    change.cause = static_cast<WebCore::CookieChangeCause>(255);
    check(JSValueIsNull(context, toWebAPICookieChange(context, PAL::SessionID::defaultSessionID(), change)),
        "an invalid backend cause is rejected instead of receiving an invented spelling");
    change.cause = WebCore::CookieChangeCause::Explicit;
    check(JSValueIsNull(context, toWebAPICookieChange(context, PAL::SessionID { uint64_t { 0 } }, change)), "an event with no valid cookie store is rejected");
    change.cookie.partitionKey = "https://top.test"_s;
    publish("partitionChange"_s, toWebAPICookieChange(context, PAL::SessionID::defaultSessionID(), change));
    verify("partitionChange.cookie.partitionKey.topLevelSite === 'https://top.test'"_s, "partitioned events expose their actual top-level site");
    publish("partitionRemoval"_s, toWebAPICookieRemoval(context, URL("https://example.test/"_s), { PAL::SessionID::defaultSessionID(), change.cookie }));
    verify("partitionRemoval.partitionKey.topLevelSite === 'https://top.test'"_s, "removal receipts retain the selected partition");
    change.cookie.partitionKey = emptyString();
    publish("firstChange"_s, toWebAPICookieChange(context, PAL::SessionID::defaultSessionID(), change));
    publish("secondChange"_s, toWebAPICookieChange(context, PAL::SessionID::defaultSessionID(), change));
    verify("firstChange !== secondChange && firstChange.cookie !== secondChange.cookie"_s,
        "each event conversion allocates independent outer and nested cookie objects");
    verify("firstChange.cookie.value = 'listener mutation'; firstChange.removed = true; secondChange.cookie.value !== 'listener mutation' && secondChange.removed === false"_s,
        "mutating one listener's changeInfo cannot alter another listener's object");
    JSGarbageCollect(context);
    verify("secondChange.cookie.domain === '.example.test' && secondChange.cause === 'explicit'"_s,
        "published changeInfo and nested cookie survive real JavaScriptCore collection");
    change.cookie.session = true;
    publish("sessionChange"_s, toWebAPICookieChange(context, PAL::SessionID::defaultSessionID(), change));
    verify("sessionChange.cookie.session === true && !('expirationDate' in sessionChange.cookie)"_s,
        "session-cookie changeInfo omits contradictory expiry metadata");
    Vector<WebExtensionCookieParameters> cookies { parameters, parameters };
    publish("cookies"_s, toWebAPI(context, cookies));
    verify("Array.isArray(cookies) && cookies.length === 2 && cookies[0] !== cookies[1] && (cookies[0].value = 'changed', cookies[1].value !== 'changed')"_s, "each cookie result is an independent object");
    publish("emptyCookies"_s, toWebAPI(context, Vector<WebExtensionCookieParameters> { }));
    verify("Array.isArray(emptyCookies) && emptyCookies.length === 0"_s, "an empty cookie collection stays an array");
    URL removalURL("https://example.test/a?query=1"_s);
    cookie.name = String::fromUTF8("雪-name");
    publish("removed"_s, toWebAPICookieRemoval(context, removalURL, parameters));
    verify("Object.keys(removed).sort().join(',') === 'name,storeId,url' && removed.url === 'https://example.test/a?query=1' && removed.storeId === 'ephemeral-1'"_s,
        "cookie removal returns its URL, name and store ID without exposing a cookie value");
    verify(String::fromUTF8("removed.name === '雪-name'"), "cookie removal preserves Unicode names");
    publish("secondRemoval"_s, toWebAPICookieRemoval(context, removalURL, parameters));
    verify("secondRemoval !== removed && (secondRemoval.name = 'changed', removed.name !== 'changed')"_s, "cookie removal results are independent objects");
    parameters.sessionIdentifier = std::nullopt;
    check(JSValueIsNull(context, toWebAPI(context, parameters)), "a cookie without a session cannot fabricate a valid store");
    check(JSValueIsNull(context, toWebAPICookieRemoval(context, removalURL, parameters)), "a removal without a store cannot fabricate a valid result");
    parameters.sessionIdentifier = PAL::SessionID(0);
    check(JSValueIsNull(context, toWebAPI(context, parameters)), "a cookie with an invalid session cannot fabricate a valid store");
    check(JSValueIsNull(context, toWebAPICookieRemoval(context, removalURL, parameters)), "a removal with an invalid store returns null");
    HashMap<PAL::SessionID, Vector<WebExtensionTabIdentifier>> stores;
    stores.add(PAL::SessionID::defaultSessionID(), Vector { *toWebExtensionTabIdentifier(7), *toWebExtensionTabIdentifier(9) });
    stores.add(PAL::SessionID::legacyPrivateSessionID(), Vector<WebExtensionTabIdentifier> { });
    publish("stores"_s, toWebAPI(context, stores, PAL::SessionID::defaultSessionID()));
    verify("stores.length === 2 && stores.find(s => s.id === 'persistent-1').tabIds.join(',') === '7,9' && stores.find(s => s.id === 'persistent-1').incognito === false && stores.find(s => s.id === 'ephemeral-1').incognito === true"_s, "cookie stores preserve tab order and private-store metadata");
    publish("ownPrivateStores"_s, toWebAPI(context, stores, PAL::SessionID::legacyPrivateSessionID()));
    verify("ownPrivateStores.find(s => s.id === 'ephemeral-1').incognito === false && ownPrivateStores.find(s => s.id === 'ephemeral-1').tabIds.length === 0"_s, "the pinned own-session incognito convention and empty tab list are retained");
    JSGarbageCollect(context);
    verify("stores.find(s => s.id === 'persistent-1').tabIds[1] === 9 && cookies[1].path === '/a'"_s, "published cookie and nested store objects survive garbage collection");
    verify("removed.url === 'https://example.test/a?query=1' && removed.storeId === 'ephemeral-1'"_s, "cookie removal results survive garbage collection");
}
class CookieParametersApplication final : public BApplication {
public:
    bool didRun { false };
    CookieParametersApplication() : BApplication("application/x-vnd.Kunanyi-Summit-cookie-parameters-tests") { }
    void ReadyToRun() override
    {
        WTF::initializeMainThread();
        checkIdentifiers();
        checkQueries();
        checkJavaScriptValues();
        didRun = true;
        PostMessage(B_QUIT_REQUESTED);
    }
};
int main()
{
    CookieParametersApplication application;
    application.Run();
    check(application.didRun, "the native suite ran from BApplication::ReadyToRun");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
