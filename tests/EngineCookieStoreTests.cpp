#include "config.h"
#include <Application.h>
#include <WebCore/Cookie.h>
#include <WebCore/CookieJarDB.h>
#include <WebCore/CookieUtil.h>
#include <WebCore/SQLiteDatabase.h>
#include <WebCore/SQLiteStatement.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <wtf/MainThread.h>
#include <wtf/WallTime.h>
#include <wtf/text/MakeString.h>

using namespace WebCore;
static unsigned checks, failures;
#if SUMMIT_COOKIE_API_PARSER_TESTS
void runCookieWriteChecks(void (*)(bool, const char*));
#endif
static void check(bool result, const char* description)
{
    ++checks;
    failures += !result;
    std::printf("%s %s\n", result ? "PASS" : "FAIL", description);
}
static Cookie makeCookie()
{
    Cookie cookie;
    cookie.name = "native-cookie"_s;
    cookie.value = "retained-value"_s;
    cookie.domain = "example.test"_s;
    cookie.path = "/area"_s;
    cookie.created = WallTime::now().secondsSinceEpoch().milliseconds();
    cookie.expires = cookie.created + 3600000.0;
    cookie.sameSite = Cookie::SameSitePolicy::Strict;
    cookie.secure = true;
    cookie.httpOnly = true;
    return cookie;
}
static void checkMigration(const String& path, bool conflictingColumn, int version, bool validVersionTwo = false)
{
    {
        SQLiteDatabase seed;
        check(seed.open(path), "the migration fixture opens a real SQLite file");
        check(seed.executeCommand("CREATE TABLE Cookie (name TEXT NOT NULL, value TEXT, domain TEXT NOT NULL, path TEXT NOT NULL, expires INTEGER NOT NULL, size INTEGER NOT NULL, session INTEGER NOT NULL, httponly INTEGER NOT NULL DEFAULT 0, secure INTEGER NOT NULL DEFAULT 0, lastupdated INTEGER NOT NULL DEFAULT CURRENT_TIMESTAMP, UNIQUE(name, domain, path))"_s),
            "the migration fixture uses the previous cookie schema");
        if (conflictingColumn)
            check(seed.executeCommand("ALTER TABLE Cookie ADD COLUMN samesite INTEGER NOT NULL DEFAULT 0"_s), "the failed-migration fixture contains a conflicting second column");
        if (validVersionTwo)
            check(seed.executeCommand("ALTER TABLE Cookie ADD COLUMN created REAL NOT NULL DEFAULT 0"_s)
                && seed.executeCommand("ALTER TABLE Cookie ADD COLUMN samesite INTEGER NOT NULL DEFAULT 0"_s), "the version-two fixture includes SameSite and creation metadata");
        check(seed.executeCommand("INSERT INTO Cookie (name,value,domain,path,expires,size,session) VALUES ('legacy','keep-me','example.test','/',4000000000000,7,0), ('session','discard-at-restart','example.test','/',0,18,1)"_s),
            "legacy persistent and session cookies are seeded");
        check(seed.executeCommandSlow(makeString("PRAGMA user_version="_s, version)), "the fixture records its original schema version");
    }
    {
        CookieJarDB database(path);
        database.open();
        auto restored = database.getAllCookies();
        if (!conflictingColumn && (version <= 1 || validVersionTwo)) {
            check(restored.size() == 1 && restored[0].name == "legacy"_s && restored[0].value == "keep-me"_s,
                "migration retains persistent cookies and removes session cookies at restart");
            if (restored.size() == 1)
                check(restored[0].created == 0 && restored[0].sameSite == Cookie::SameSitePolicy::None && restored[0].partitionKey.isEmpty(),
                    "old metadata stays unknown instead of acquiring a fabricated creation time or policy");
        } else
            check(restored.isEmpty() && !database.setCookie(makeCookie()), "failed or future schemas refuse reads and writes");
    }
    {
        SQLiteDatabase inspect;
        check(inspect.open(path), "the database file still opens after the migration attempt");
        auto current = inspect.prepareStatement("PRAGMA user_version"_s);
        bool succeeded = !conflictingColumn && (version <= 1 || validVersionTwo);
        check(current && current->columnInt(0) == (succeeded ? 3 : version), "only a successful migration advances the schema version");
        auto rows = inspect.prepareStatement("SELECT count(*) FROM Cookie"_s);
        check(rows && rows->columnInt(0) == (succeeded ? 1 : 2), "a failed migration preserves all original cookie rows");
        auto columns = inspect.prepareStatement("SELECT count(*) FROM pragma_table_info('Cookie') WHERE name='created'"_s);
        check(columns && columns->columnInt(0) == (succeeded ? 1 : 0), "a failed second migration step rolls back the first column addition");
    }
}

static void checkPartitionedCookies()
{
    CookieJarDB database(":memory:"_s);
    database.open();
    URL embed("https://widget.test/area/page"_s), first("https://shop.example/"_s), second("https://news.example/"_s);
    URL sibling("https://sub.shop.example:8443/other"_s), otherScheme("http://shop.example/"_s);
    auto set = [&](const URL& top, const String& header, CookieJarDB::Source source = CookieJarDB::Source::Network) {
        return database.setCookie(top, embed, header, source);
    };
    auto values = [&](const URL& top) {
        Vector<String> result;
        if (auto cookies = database.searchCookies(top, embed, std::nullopt, std::nullopt, std::nullopt)) {
            for (auto& cookie : *cookies)
                result.append(cookie.value);
        }
        return result;
    };
    check(set(first, "id=ordinary; Secure; Path=/; SameSite=None"_s)
        && set(first, "id=first; Secure; Path=/; SameSite=None; Partitioned; Max-Age=3600"_s)
        && set(second, "id=second; Secure; Path=/; SameSite=None; pArTiTiOnEd; Max-Age=3600"_s),
        "identical cookie names, domains and paths coexist in different partitions and the ordinary jar");
    check(values(first).size() == 2 && values(first).contains("first"_s) && !values(first).contains("second"_s),
        "the first embedding site sees only its own partition plus ordinary cookies");
    check(values(second).size() == 2 && values(second).contains("second"_s) && !values(second).contains("first"_s),
        "the second embedding site cannot read the first partition");
    check(values(sibling) == values(first), "subdomains, paths and ports share their top-level schemeful site");
    check(values(otherScheme).size() == 1 && values(otherScheme).contains("ordinary"_s), "HTTP and HTTPS top-level sites use distinct partitions");
    check(values(URL { }).size() == 1, "an unknown top-level site never widens partition access");
    auto all = database.getAllCookies();
    check(all.size() == 3 && all[1].partitionKey == "https://shop.example"_s && all[2].partitionKey == "https://news.example"_s,
        "privileged enumeration retains canonical partition keys");
    check(all.size() == 3 && !(all[0] == all[1]) && !(all[1] == all[2]), "cookie equality distinguishes partition identities for inspector enumeration");
    check(set(first, "id=updated; Secure; Path=/; Partitioned"_s) && values(first).contains("updated"_s)
        && values(second).contains("second"_s), "a partitioned replacement affects only its own jar");
    check(set(first, "id=gone; Secure; Path=/; Partitioned; Max-Age=0"_s)
        && values(first).size() == 1 && values(second).size() == 2, "partitioned expiration does not delete another partition or the ordinary cookie");
    check(!set(first, "insecure=bad; Partitioned"_s)
        && !database.setCookie(first, URL("http://widget.test/"_s), "insecure=bad; Secure; Partitioned"_s, CookieJarDB::Source::Network)
        && !set(URL("data:text/html,opaque"_s), "opaque=bad; Secure; Partitioned"_s),
        "Partitioned requires Secure, an HTTPS setter and a usable top-level site");
    check(set(first, "locked=server; Path=/; Secure; HttpOnly; Partitioned"_s)
        && !set(first, "locked=script; Path=/; Secure; Partitioned"_s, CookieJarDB::Source::Script)
        && set(second, "locked=other-partition; Path=/; Secure; Partitioned"_s, CookieJarDB::Source::Script),
        "HttpOnly overwrite protection applies to the exact partition");
    auto rows = database.getAllCookies();
    for (auto& row : rows) {
        if (row.value == "second"_s)
            check(database.deleteCookie(row), "structured deletion accepts a partitioned cookie's exact identity");
    }
    check(!values(second).contains("second"_s) && values(second).contains("ordinary"_s), "exact partition deletion preserves the ordinary same-name cookie");
    auto invalid = makeCookie();
    invalid.partitionKey = "https://sub.shop.example/path"_s;
    check(!database.setCookie(invalid), "structured writes reject noncanonical partition keys");
}
#if SUMMIT_COOKIE_CHANGE_TESTS
void runCookieChangeChecks(void (*)(bool, const char*));
void runCookieExpiryTimerChecks(void (*)(bool, const char*), Function<void()>&&);
#endif
#if SUMMIT_COOKIE_IPC_TESTS
void runCookieChangeIPCChecks(void (*)(bool, const char*));
#endif
static void runChecks()
{
    checkPartitionedCookies();
#if SUMMIT_COOKIE_IPC_TESTS
    runCookieChangeIPCChecks(check);
#endif
#if SUMMIT_COOKIE_CHANGE_TESTS
    runCookieChangeChecks(check);
#endif
#if SUMMIT_COOKIE_API_PARSER_TESTS
    runCookieWriteChecks(check);
#endif
    CookieJarDB memory(":memory:"_s);
    memory.open();
    auto cookie = makeCookie();
    check(memory.setCookie(cookie), "structured cookie write succeeds in an actual in-memory SQLite database");
    auto all = memory.getAllCookies();
    check(all.size() == 1, "cookie enumeration returns the stored cookie");
    if (all.size() == 1) {
        check(all[0].name == cookie.name && all[0].value == cookie.value && all[0].path == cookie.path && all[0].domain == cookie.domain,
            "cookie identity and text round-trip through SQLite");
        check(all[0].sameSite == cookie.sameSite, "SameSite policy survives storage and enumeration");
        check(all[0].created == cookie.created, "creation time survives storage and enumeration");
        check(all[0].httpOnly && all[0].secure && !all[0].session, "cookie flags survive storage and enumeration");
    }
    URL request("https://example.test/area/page"_s);
    auto matching = memory.searchCookies(request, request, std::nullopt, std::nullopt, std::nullopt);
    check(matching && matching->size() == 1, "URL lookup selects the actual stored cookie");
    if (matching && matching->size() == 1) {
        check((*matching)[0].sameSite == cookie.sameSite, "URL lookup retains the SameSite policy");
        check((*matching)[0].created == cookie.created, "URL lookup retains creation time for ordering");
    }
    auto replacement = cookie;
    replacement.value = "changed"_s;
    replacement.created += 1000;
    replacement.sameSite = Cookie::SameSitePolicy::Lax;
    check(memory.setCookie(replacement), "an existing cookie can be replaced");
    auto replaced = memory.getAllCookies();
    check(replaced.size() == 1 && replaced[0].value == replacement.value && replaced[0].sameSite == replacement.sameSite && replaced[0].created == cookie.created,
        "replacement changes content and policy while preserving original creation time");
    auto expired = cookie;
    expired.expires = WallTime::now().secondsSinceEpoch().milliseconds() - 1000.0;
    check(memory.setCookie(expired), "writing an expired replacement succeeds");
    check(memory.getAllCookies().isEmpty(), "an expired replacement deletes its row using wall-clock time");
    std::optional<Cookie> receipt;
    check(memory.setCookieWithResult(cookie, receipt) && receipt && receipt->value == cookie.value,
        "a confirmed write returns the row stored by SQLite");
    replacement.expires = cookie.expires.value() + 0.75;
    check(memory.setCookieWithResult(replacement, receipt) && receipt && receipt->created == cookie.created
        && receipt->value == replacement.value && receipt->sameSite == replacement.sameSite
        && receipt->expires == static_cast<double>(static_cast<int64_t>(*replacement.expires)),
        "a write receipt uses preserved creation time, updated metadata and actual stored expiry");
    check(memory.setCookieWithResult(expired, receipt) && !receipt,
        "confirmed expiry returns no cookie instead of echoing the deleted input");
    auto sessionCookie = cookie;
    sessionCookie.session = true;
    check(memory.setCookieWithResult(sessionCookie, receipt) && receipt && receipt->session && !receipt->expires,
        "a session write receipt omits an expiry that was not stored");
    auto partitioned = cookie;
    partitioned.partitionKey = "https://top.test"_s;
    check(memory.setCookieWithResult(partitioned, receipt) && receipt && receipt->partitionKey == partitioned.partitionKey,
        "a partitioned write receipt retains the actual stored partition key");
    check(memory.deleteCookie(partitioned), "the receipt test removes only its partitioned row");
    memory.setAcceptPolicy(CookieAcceptPolicy::Never);
    receipt = cookie;
    check(!memory.setCookieWithResult(cookie, receipt) && !receipt && !memory.deleteCookie(cookie),
        "a disabled database reports mutation failure");
    memory.setAcceptPolicy(CookieAcceptPolicy::Always);
    check(memory.deleteCookie(sessionCookie), "the receipt test cleans up its stored cookie");
    CookieJarDB unopened(":memory:"_s);
    receipt = cookie;
    check(!unopened.setCookieWithResult(cookie, receipt) && !receipt && !unopened.deleteCookie(cookie),
        "an unopened database reports failure instead of completing a fictitious mutation");
    for (auto domain : { ".example.test"_s, "::1"_s }) {
        auto scoped = cookie;
        scoped.domain = domain;
        check(memory.setCookie(scoped), "a domain or IPv6 cookie can be stored");
        check(memory.deleteCookie(scoped) && memory.getAllCookies().isEmpty(), "exact-key deletion preserves domain-dot and IPv6 identity");
    }
    auto parsed = CookieUtil::parseCookieHeader("from-http=value; SameSite=Lax; Secure; HttpOnly"_s);
    check(parsed && parsed->sameSite == Cookie::SameSitePolicy::Lax && parsed->secure && parsed->httpOnly,
        "HTTP cookie parsing retains SameSite together with other flags");
    char directory[] = "/tmp/summit-cookie-store-XXXXXX";
    char* created = mkdtemp(directory);
    check(created, "the persistence test gets its own temporary directory");
    if (!created)
        return;
    auto path = makeString(String::fromUTF8(created), "/cookies.sqlite"_s);
    {
        CookieJarDB database(path);
        database.open();
        check(database.setCookie(cookie), "a persistent cookie is written to a real database file");
    }
    {
        CookieJarDB database(path);
        database.open();
        auto restored = database.getAllCookies();
        check(restored.size() == 1 && restored[0].value == cookie.value, "a persistent cookie survives database close and reopen");
        if (restored.size() == 1)
            check(restored[0].sameSite == cookie.sameSite && restored[0].created == cookie.created, "persistent cookie metadata survives reopen");
    }
    {
        CookieJarDB database(path);
        database.open();
        check(database.setCookie(expired), "an expired persistent replacement succeeds");
    }
    {
        SQLiteDatabase inspect;
        check(inspect.open(path), "the expired-cookie database remains readable");
        auto rows = inspect.prepareStatement("SELECT count(*) FROM Cookie WHERE name='native-cookie'"_s);
        check(rows && !rows->columnInt(0), "expired replacement physically removes the stored row");
    }
    auto legacy = makeString(String::fromUTF8(created), "/legacy.sqlite"_s);
    auto unversioned = makeString(String::fromUTF8(created), "/unversioned.sqlite"_s);
    auto failed = makeString(String::fromUTF8(created), "/failed.sqlite"_s);
    auto future = makeString(String::fromUTF8(created), "/future.sqlite"_s);
    auto malformed = makeString(String::fromUTF8(created), "/malformed.sqlite"_s);
    auto versionTwo = makeString(String::fromUTF8(created), "/version-two.sqlite"_s);
    auto rejected = makeString(String::fromUTF8(created), "/rejected.sqlite"_s);
    {
        CookieJarDB database(rejected);
        database.open();
        check(database.setCookie(cookie), "the mutation failure fixture stores its original cookie");
        SQLiteDatabase injectFailure;
        check(injectFailure.open(rejected)
            && injectFailure.executeCommand("CREATE TRIGGER refuse_cookie_update BEFORE UPDATE ON Cookie BEGIN SELECT RAISE(ABORT, 'test write rejected'); END"_s)
            && injectFailure.executeCommand("CREATE TRIGGER refuse_cookie_delete BEFORE DELETE ON Cookie BEGIN SELECT RAISE(ABORT, 'test delete rejected'); END"_s),
            "real SQLite triggers reject updates and deletes in the failure fixture");
        receipt = cookie;
        check(!database.setCookieWithResult(replacement, receipt) && !receipt,
            "an actual SQL write failure returns failure and no invented stored cookie");
        check(!database.deleteCookie(cookie), "an actual SQL delete failure is reported");
        receipt = cookie;
        check(!database.setCookieWithResult(expired, receipt) && !receipt,
            "an expired replacement reports a failed deletion");
        auto unchanged = database.getAllCookies();
        check(unchanged.size() == 1 && unchanged[0].value == cookie.value,
            "failed mutations leave the original persisted value intact");
    }
    checkMigration(legacy, false, 1);
    checkMigration(unversioned, false, 0);
    checkMigration(failed, true, 1);
    checkMigration(future, false, 99);
    checkMigration(malformed, false, 2);
    checkMigration(versionTwo, false, 2, true);
    for (auto& databasePath : { path, legacy, unversioned, failed, future, malformed, rejected, versionTwo }) {
        for (auto suffix : { ""_s, "-wal"_s, "-shm"_s })
            unlink(makeString(databasePath, suffix).utf8().legacyCStringPointer());
    }
    rmdir(created);
}
class CookieStoreApplication final : public BApplication {
public:
    bool didRun { false };
    CookieStoreApplication() : BApplication("application/x-vnd.Kunanyi-Summit-cookie-store-tests") { }
    void ReadyToRun() override
    {
        WTF::initializeMainThread();
        runChecks();
#if SUMMIT_COOKIE_CHANGE_TESTS
        runCookieExpiryTimerChecks(check, [this] {
            didRun = true;
            PostMessage(B_QUIT_REQUESTED);
        });
#else
        didRun = true;
        PostMessage(B_QUIT_REQUESTED);
#endif
    }
};
int main()
{
    CookieStoreApplication application;
    application.Run();
    check(application.didRun, "the real native database test ran from BApplication::ReadyToRun");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
