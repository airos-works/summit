#include "config.h"
#include "WebExtensionCookieWriteParser.h"
#include <WebCore/CookieJarDB.h>
#include <wtf/JSONValues.h>
#include <wtf/WallTime.h>
#include <wtf/text/MakeString.h>
using namespace WebKit;
void runCookieWriteChecks(void (*check)(bool, const char*))
{
    auto parse = [](const String& text) { return parseWebExtensionCookieWrite(JSON::Value::parseJSON(text).get()); };
    auto minimal = parse(R"({"url":"https://Example.test/area/page?ignored=1#fragment"})"_s);
    check(minimal && minimal->cookie.domain == "example.test"_s && minimal->cookie.path == "/area"_s
        && minimal->cookie.name.isEmpty() && minimal->cookie.value.isEmpty() && minimal->cookie.session && !minimal->cookie.expires,
        "cookie write defaults preserve host-only scope, directory path and session lifetime");
    auto domain = parse(R"({"url":"https://sub.example.com/","domain":"EXAMPLE.COM","name":"n","value":"v","secure":true,"httpOnly":true,"sameSite":"strict","expirationDate":4000000000.25,"storeId":"ephemeral-9007199254740993"})"_s);
    check(domain && domain->cookie.domain == ".example.com"_s && domain->cookie.secure && domain->cookie.httpOnly
        && domain->cookie.sameSite == WebCore::Cookie::SameSitePolicy::Strict && domain->cookie.expires == 4000000000250.0
        && !domain->cookie.session && domain->session && domain->session->toUInt64() == (PAL::SessionID::EphemeralSessionMask | 9007199254740993ULL),
        "domain-cookie attributes and full-width store identity are preserved");
    auto dotted = parse(R"({"url":"https://sub.example.com/","domain":".example.com"})"_s);
    check(dotted && dotted->cookie.domain == ".example.com"_s, "a leading domain dot is canonicalized once");
    auto idna = parse(R"({"url":"https://xn--bcher-kva.example/","domain":"b\u00fccher.example"})"_s);
    check(idna && idna->cookie.domain == ".xn--bcher-kva.example"_s, "international domain text uses the URL parser's IDNA normalization");
    auto ipv6 = parse(R"({"url":"https://[::1]/","domain":"::1","name":"ip","value":"six"})"_s);
    check(ipv6 && ipv6->cookie.domain == ipv6->url.host() && !ipv6->cookie.domain.startsWith('.'), "IPv6 cookies retain the Curl backend's exact URL-host identity");
    auto ipv4 = parse(R"({"url":"http://127.0.0.1/","domain":"127.0.0.1","secure":true})"_s);
    check(ipv4 && ipv4->cookie.domain == "127.0.0.1"_s, "trustworthy loopback cookies remain host-only");
    for (auto text : { R"({"url":"https://example.com/","domain":"other.com"})"_s,
        R"({"url":"https://badexample.com/","domain":"example.com"})"_s,
        R"({"url":"https://shop.example.com/","domain":"com"})"_s,
        R"({"url":"https://shop.example.co.uk/","domain":"co.uk"})"_s,
        R"({"url":"https://project.github.io/","domain":"github.io"})"_s,
        R"({"url":"http://127.0.0.1/","domain":"0.0.1"})"_s,
        R"({"url":"https://example.com/","domain":"..example.com"})"_s,
        R"({"url":"https://example.com/","domain":"example.com@other.com"})"_s,
        R"({"url":"https://example.com/","domain":"example.com/other"})"_s,
        R"({"url":"https://example.com/","domain":"example.com:443"})"_s })
        check(!parse(text), "unrelated, public-suffix, partial-label and malformed domains are rejected");
    for (auto text : { R"({"url":"http://example.com/","secure":true})"_s,
        R"({"url":"https://example.com/","name":"__Secure-id"})"_s,
        R"({"url":"https://example.com/","name":"__Host-id","secure":true,"domain":"example.com"})"_s,
        R"({"url":"https://example.com/area/page","name":"__Host-id","secure":true})"_s,
        R"({"url":"https://example.com/","name":"__Http-id","secure":true})"_s,
        R"({"url":"https://example.com/","name":"__Host-Http-id","secure":true})"_s,
        R"({"url":"https://example.com/","sameSite":"no_restriction"})"_s })
        check(!parse(text), "secure-cookie and cookie-prefix requirements are validated");
    check(!!parse(R"({"url":"https://example.com/","name":"__Host-Http-id","secure":true,"httpOnly":true,"path":"/"})"_s),
        "a correctly scoped secure HttpOnly host-prefixed cookie is accepted");
    check(!!parse(R"({"url":"https://example.com/","secure":true,"sameSite":"no_restriction"})"_s), "explicit SameSite None with secure is accepted");
    auto partition = parse(R"({"url":"https://example.com/","secure":true,"partitionKey":{"topLevelSite":"https://sub.top.test:8443/path"}})"_s);
    check(partition && partition->cookie.partitionKey == "https://top.test"_s, "partitioned API writes retain the normalized top-level site");
    for (auto text : { "null"_s, "[]"_s, "{}"_s, R"({"url":"file:///tmp/cookies"})"_s,
        R"({"url":"https://example.com/","name":"bad;name"})"_s,
        R"({"url":"https://example.com/","name":"bad=name"})"_s,
        R"({"url":"https://example.com/","value":"bad\r\nvalue"})"_s,
        R"({"url":"https://example.com/","path":"relative"})"_s,
        R"({"url":"https://example.com/","secure":1})"_s,
        R"({"url":"https://example.com/","httpOnly":"true"})"_s,
        R"({"url":"https://example.com/","expirationDate":true})"_s,
        R"({"url":"https://example.com/","expirationDate":"4000000000"})"_s,
        R"({"url":"https://example.com/","expirationDate":1e100})"_s,
        R"({"url":"https://example.com/","storeId":"persistent-0"})"_s,
        R"({"url":"https://example.com/","sameSite":"unspecified"})"_s,
        R"({"url":"https://example.com/","partitionKey":{"topLevelSite":"https://example.com"}})"_s,
        R"({"url":"https://example.com/","firstPartyDomain":"example.com"})"_s,
        R"({"url":"https://example.com/","created":123})"_s })
        check(!parse(text), "malformed and unsupported writes fail without silently dropping scope fields");
    auto zero = parse(R"({"url":"https://example.com/","expirationDate":0})"_s);
    auto negative = parse(R"({"url":"https://example.com/","expirationDate":-1})"_s);
    check(zero && negative && !zero->cookie.session && zero->cookie.expires == 0.0 && negative->cookie.expires == -1000.0,
        "zero and negative expiries mean expired cookies instead of accidental session cookies");
    WebCore::CookieJarDB database(":memory:"_s);
    database.open();
    if (domain) {
        std::optional<WebCore::Cookie> stored;
        check(database.setCookieWithResult(domain->cookie, stored) && stored && stored->domain == ".example.com"_s,
            "a parsed domain cookie reaches the real SQLite writer");
        auto queried = database.searchCookies(domain->url, domain->url, std::nullopt, std::nullopt, std::nullopt);
        check(queried && queried->size() == 1 && (*queried)[0].value == "v"_s, "the written domain cookie is found for its original URL");
    }
    if (ipv6) {
        check(database.setCookie(ipv6->cookie), "a parsed IPv6 cookie is stored");
        auto queried = database.searchCookies(ipv6->url, ipv6->url, std::nullopt, std::nullopt, std::nullopt);
        check(queried && queried->size() == 1 && (*queried)[0].value == "six"_s, "IPv6 API input round-trips through actual URL lookup");
        check(database.setCookie(ipv6->url, ipv6->url, "from-http=six; Domain=[::1]; Path=/"_s, WebCore::CookieJarDB::Source::Network),
            "a bracketed IPv6 Domain attribute is accepted by the actual HTTP writer");
        queried = database.searchCookies(ipv6->url, ipv6->url, std::nullopt, std::nullopt, std::nullopt);
        check(queried && queried->size() == 2 && (*queried)[1].domain == ipv6->url.host(),
            "the HTTP writer keeps IPv6 host-only identity without adding a domain dot");
    }
}
