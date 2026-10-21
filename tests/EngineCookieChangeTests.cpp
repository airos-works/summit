#include "config.h"
#include <WebCore/CookieChange.h>
#include <WebCore/CookieJarDB.h>
#include <WebCore/SQLiteDatabase.h>
#include <WebCore/SQLiteStatement.h>
#include <cstdlib>
#include <unistd.h>
#include <wtf/SharedTask.h>
#include <wtf/WallTime.h>
#include <wtf/text/MakeString.h>

using namespace WebCore;

static Cookie eventCookie()
{
    Cookie cookie;
    cookie.name = "event"_s;
    cookie.value = "before"_s;
    cookie.domain = "example.test"_s;
    cookie.path = "/area"_s;
    cookie.created = WallTime::now().secondsSinceEpoch().milliseconds();
    cookie.expires = cookie.created + 3600000.0;
    cookie.secure = true;
    cookie.httpOnly = true;
    cookie.sameSite = Cookie::SameSitePolicy::Strict;
    return cookie;
}

void runCookieChangeChecks(void (*check)(bool, const char*))
{
    CookieJarDB memory(":memory:"_s);
    memory.open();
    Vector<Vector<CookieChange>> batches;
    auto observe = [&batches](CookieJarDB& database) {
        database.setCookieChangeHandler(createSharedTask<void(Vector<CookieChange>&&)>([&batches](Vector<CookieChange>&& changes) {
            batches.append(WTF::move(changes));
        }));
    };
    observe(memory);
    check(batches.isEmpty(), "registering a change observer does not invent a cookie mutation");
    auto cookie = eventCookie();
    check(memory.setCookie(cookie), "an observed insert commits to real SQLite");
    check(batches.size() == 1 && batches[0].size() == 1 && !batches[0][0].removed
        && batches[0][0].cause == CookieChangeCause::Explicit, "insertion emits one explicit addition");
    if (batches.size() == 1 && batches[0].size() == 1) {
        auto& stored = batches[0][0].cookie;
        check(stored.isKeyEqual(cookie) && stored.value == cookie.value && stored.created == cookie.created
            && stored.expires == static_cast<double>(static_cast<int64_t>(*cookie.expires))
            && stored.httpOnly && stored.secure && !stored.session && stored.sameSite == cookie.sameSite,
            "the addition carries actual identity, value, creation, normalized expiry, flags and SameSite");
    }
    auto replacement = cookie;
    replacement.value = "after"_s;
    replacement.sameSite = Cookie::SameSitePolicy::Lax;
    replacement.created += 1000;
    batches.clear();
    std::optional<Cookie> receipt;
    check(memory.setCookieWithResult(replacement, receipt), "an observed overwrite returns a committed receipt");
    check(batches.size() == 1 && batches[0].size() == 2, "overwrite delivers both changes in one ordered batch");
    if (batches.size() == 1 && batches[0].size() == 2) {
        auto& before = batches[0][0];
        auto& after = batches[0][1];
        check(before.removed && before.cause == CookieChangeCause::Overwrite && before.cookie.value == cookie.value
            && before.cookie.sameSite == cookie.sameSite, "overwrite removal carries the previous value and metadata");
        check(!after.removed && after.cause == CookieChangeCause::Explicit && after.cookie.value == replacement.value
            && after.cookie.sameSite == replacement.sameSite && after.cookie.created == cookie.created
            && receipt && *receipt == after.cookie, "overwrite addition agrees with the stored receipt and preserved creation time");
    }
    batches.clear();
    check(memory.setCookie(replacement) && batches.size() == 1 && batches[0].size() == 2,
        "replacing an unchanged value still reports a committed overwrite pair");
    auto expired = replacement;
    expired.expires = 0;
    batches.clear();
    check(memory.setCookieWithResult(expired, receipt) && !receipt, "an observed expired replacement returns no stored cookie");
    check(batches.size() == 1 && batches[0].size() == 1 && batches[0][0].removed
        && batches[0][0].cause == CookieChangeCause::ExpiredOverwrite && batches[0][0].cookie.value == replacement.value,
        "an expired replacement reports the removed stored value with expired-overwrite cause");
    batches.clear();
    check(memory.setCookie(expired) && batches.isEmpty(), "an expired write to a missing key emits no change");
    check(memory.deleteCookie(cookie) && batches.isEmpty(), "deleting a missing key emits no change");
    check(memory.deleteExpiredCookies() && batches.isEmpty(), "an empty expiry sweep emits no change");

    check(memory.setCookie(cookie), "explicit deletion fixture is stored");
    batches.clear();
    check(memory.deleteCookie(cookie) && batches.size() == 1 && batches[0].size() == 1
        && batches[0][0].removed && batches[0][0].cause == CookieChangeCause::Explicit
        && batches[0][0].cookie.value == cookie.value, "explicit deletion reports the actual old cookie");
    auto pathCookie = cookie;
    pathCookie.path = "/other"_s;
    auto domainCookie = cookie;
    domainCookie.domain = ".example.test"_s;
    auto ordinaryCookie = cookie;
    ordinaryCookie.name = "ordinary"_s;
    ordinaryCookie.httpOnly = false;
    check(memory.setCookie(cookie) && memory.setCookie(pathCookie) && memory.setCookie(domainCookie) && memory.setCookie(ordinaryCookie),
        "bulk deletion fixture has distinct paths, domain spelling and HttpOnly flags");
    batches.clear();
    check(memory.deleteCookiesForHostname("example.test"_s, IncludeHttpOnlyCookies::No)
        && batches.size() == 1 && batches[0].size() == 1 && batches[0][0].cookie.name == ordinaryCookie.name,
        "hostname deletion excluding HttpOnly reports only the deleted ordinary cookie");
    batches.clear();
    check(memory.deleteCookie(cookie) && batches.size() == 1 && batches[0].size() == 1
        && batches[0][0].cookie.path == cookie.path && memory.getAllCookies().size() == 2,
        "exact deletion and its event preserve unrelated paths and leading-dot domains");
    batches.clear();
    check(memory.deleteAllCookies() && batches.size() == 1 && batches[0].size() == 2 && memory.getAllCookies().isEmpty(),
        "delete-all reports the two remaining actual rows in one batch");
    if (batches.size() == 1 && batches[0].size() == 2)
        check(batches[0][0].removed && batches[0][1].removed
            && batches[0][0].cause == CookieChangeCause::Explicit && batches[0][1].cause == CookieChangeCause::Explicit,
            "all live rows removed by delete-all have explicit causes");
    batches.clear();
    check(memory.deleteAllCookies() && batches.isEmpty(), "empty delete-all does not emit a spurious event");
    auto emptyPath = cookie;
    emptyPath.path = emptyString();
    check(memory.setCookie(emptyPath) && memory.setCookie(cookie), "exact empty-path fixture stores both keys");
    batches.clear();
    check(memory.deleteCookie(emptyPath) && batches.size() == 1 && batches[0].size() == 1
        && batches[0][0].cookie.path.isEmpty() && memory.getAllCookies().size() == 1,
        "an empty exact path is not widened to every path in a domain");
    memory.deleteAllCookies();
    batches.clear();
    URL url("https://example.test/area/page"_s);
    check(memory.setCookie(url, url, "http-event=network; Path=/area; Secure; HttpOnly; SameSite=Lax"_s, CookieJarDB::Source::Network)
        && batches.size() == 1 && batches[0].size() == 1 && batches[0][0].cookie.httpOnly
        && batches[0][0].cookie.sameSite == Cookie::SameSitePolicy::Lax,
        "actual HTTP header parsing and database insertion emit a populated addition");
    batches.clear();
    check(!memory.setCookie(url, url, "http-event=script; Path=/area"_s, CookieJarDB::Source::Script) && batches.isEmpty(),
        "a rejected script overwrite of an HttpOnly cookie emits no event");
    check(memory.setCookie(url, url, "script-event=script; Path=/area"_s, CookieJarDB::Source::Script)
        && batches.size() == 1 && batches[0].size() == 1 && !batches[0][0].cookie.httpOnly,
        "accepted script writes also reach the real cookie observer");
    memory.deleteAllCookies();
    memory.setCookieChangeHandler(nullptr);
    batches.clear();
    check(memory.setCookie(cookie) && batches.isEmpty(), "unregistering the observer stops notification delivery");
    observe(memory);
    batches.clear();
    auto partitioned = cookie;
    partitioned.partitionKey = "https://top.test"_s;
    check(memory.setCookie(partitioned) && memory.deleteCookie(partitioned) && batches.size() == 2
        && batches[0].size() == 1 && batches[1].size() == 1
        && batches[0][0].cookie.partitionKey == partitioned.partitionKey
        && batches[1][0].cookie.partitionKey == partitioned.partitionKey && batches[1][0].removed,
        "partitioned insert and delete events retain the exact partition identity");
    batches.clear();
    memory.setAcceptPolicy(CookieAcceptPolicy::Never);
    check(!memory.setCookie(cookie) && !memory.deleteAllCookies() && !memory.deleteExpiredCookies() && batches.isEmpty(),
        "disabled database operations emit no changes");
    memory.setAcceptPolicy(CookieAcceptPolicy::Always);
    memory.setCookieChangeHandler(nullptr);
    memory.deleteAllCookies();

    // Re-entrancy must happen after all SQL statements and the write receipt.
    unsigned callbacks = 0;
    bool nestedSucceeded = false;
    memory.setCookieChangeHandler(createSharedTask<void(Vector<CookieChange>&&)>([&](Vector<CookieChange>&& changes) {
        ++callbacks;
        if (callbacks != 1)
            return;
        check(changes.size() == 1 && memory.getAllCookies().size() == 1,
            "the observer sees a committed database when the first callback starts");
        std::optional<Cookie> nestedReceipt;
        nestedSucceeded = memory.setCookieWithResult(replacement, nestedReceipt) && nestedReceipt && nestedReceipt->value == replacement.value;
    }));
    check(memory.setCookieWithResult(cookie, receipt) && receipt && receipt->value == cookie.value && nestedSucceeded && callbacks == 2,
        "a re-entrant overwrite succeeds without changing the outer write receipt");
    memory.setCookieChangeHandler(createSharedTask<void(Vector<CookieChange>&&)>([&](Vector<CookieChange>&&) {
        ++callbacks;
        memory.setCookieChangeHandler(nullptr);
    }));
    auto beforeUnregister = callbacks;
    check(memory.setCookie(cookie) && callbacks == beforeUnregister + 1 && memory.setCookie(replacement)
        && callbacks == beforeUnregister + 1, "an observer can safely unregister itself during delivery");
    auto disposable = std::make_unique<CookieJarDB>(":memory:"_s);
    disposable->open();
    disposable->setCookieChangeHandler(createSharedTask<void(Vector<CookieChange>&&)>([&disposable](Vector<CookieChange>&&) { disposable = nullptr; }));
    check(disposable->setCookieWithResult(cookie, receipt) && !disposable && receipt && receipt->value == cookie.value,
        "an observer can destroy the database after commit while its write receipt remains valid");

    char directory[] = "/tmp/summit-cookie-events-XXXXXX";
    auto* created = mkdtemp(directory);
    check(created, "event failure and expiry tests use an isolated real SQLite file");
    if (!created)
        return;
    auto path = makeString(String::fromUTF8(created), "/events.sqlite"_s);
    {
        CookieJarDB database(path);
        database.open();
        SQLiteDatabase external;
        check(external.open(path), "the fixture can inject actual SQL failures and expired stored rows");
        observe(database);
        batches.clear();
        check(database.setCookie(cookie), "failure fixture has a committed cookie");
        batches.clear();
        check(external.executeCommand("CREATE TRIGGER reject_updates BEFORE UPDATE ON Cookie BEGIN SELECT RAISE(ABORT, 'event test'); END"_s)
            && external.executeCommand("CREATE TRIGGER reject_deletes BEFORE DELETE ON Cookie BEGIN SELECT RAISE(ABORT, 'event test'); END"_s),
            "failure fixture installs real update and delete rejection triggers");
        check(!database.setCookieWithResult(replacement, receipt) && !receipt && batches.isEmpty(), "failed SQL overwrites emit neither removal nor addition");
        check(!database.setCookie(expired) && batches.isEmpty(), "failed expired replacements emit no deletion event");
        check(!database.deleteCookie(cookie) && !database.deleteAllCookies() && batches.isEmpty(), "failed explicit SQL deletions emit no events");
        check(external.executeCommand("DROP TRIGGER reject_updates"_s) && external.executeCommand("DROP TRIGGER reject_deletes"_s)
            && external.executeCommand("CREATE TRIGGER ignore_updates BEFORE UPDATE ON Cookie BEGIN SELECT RAISE(IGNORE); END"_s),
            "a trigger can ignore a mutation while SQLite still reports statement success");
        bool ignoredWrite = database.setCookie(replacement);
        auto ignoredRows = database.getAllCookies();
        check(!ignoredWrite && batches.isEmpty() && ignoredRows.size() == 1 && ignoredRows[0].value == cookie.value,
            "an ignored SQL write returns failure and emits no invented overwrite");
        check(external.executeCommand("DROP TRIGGER ignore_updates"_s)
            && external.executeCommand("UPDATE Cookie SET expires=1 WHERE name='event'"_s), "the expiry fixture contains an actually expired stored row");
        check(database.getAllCookies().isEmpty(), "expired stored rows are excluded from normal cookie reads");
        check(external.executeCommand("CREATE TRIGGER reject_expiry BEFORE DELETE ON Cookie BEGIN SELECT RAISE(ABORT, 'event test'); END"_s),
            "expiry removal can fail at the real database layer");
        check(!database.deleteExpiredCookies() && batches.isEmpty(), "a failed expiry sweep emits no event");
        check(external.executeCommand("DROP TRIGGER reject_expiry"_s), "the expiry failure is removed");
        check(database.deleteExpiredCookies() && batches.size() == 1 && batches[0].size() == 1 && batches[0][0].removed
            && batches[0][0].cause == CookieChangeCause::Expired && batches[0][0].cookie.value == cookie.value
            && batches[0][0].cookie.expires == 1.0, "expiry sweep reports the actual expired row with expired cause");
        auto remaining = external.prepareStatement("SELECT count(*) FROM Cookie"_s);
        check(remaining && !remaining->columnInt(0), "successful expiry sweep physically deletes its database row");
        remaining = nullptr;
        batches.clear();
        check(database.deleteExpiredCookies() && batches.isEmpty(), "repeated expiry sweeps never repeat a removal event");
        check(database.setCookie(cookie) && external.executeCommand("UPDATE Cookie SET expires=1 WHERE name='event'"_s),
            "replacement fixture has an expired predecessor");
        batches.clear();
        check(database.setCookieWithResult(replacement, receipt) && receipt && receipt->created == replacement.created,
            "replacing an expired predecessor assigns the new creation time");
        check(batches.size() == 1 && batches[0].size() == 2 && batches[0][0].removed
            && batches[0][0].cause == CookieChangeCause::Expired && !batches[0][1].removed,
            "replacing an expired predecessor reports expiry followed by addition");
        batches.clear();
        check(external.executeCommand("UPDATE Cookie SET expires=1 WHERE name='event'"_s) && database.deleteCookie(cookie)
            && batches.size() == 1 && batches[0].size() == 1 && batches[0][0].cause == CookieChangeCause::Expired,
            "deleting an already expired row reports expiry, not explicit deletion of a live cookie");
        auto session = cookie;
        session.session = true;
        check(database.setCookie(session) && external.executeCommand("UPDATE Cookie SET expires=1 WHERE name='event'"_s),
            "a session-cookie fixture can retain irrelevant persisted expiry data");
        batches.clear();
        check(database.deleteExpiredCookies() && batches.isEmpty() && database.getAllCookies().size() == 1,
            "expiry sweeping never removes a session cookie");
        database.deleteAllCookies();
        check(database.setCookie(cookie) && external.executeCommand("UPDATE Cookie SET expires=1 WHERE name='event'"_s)
            && external.executeCommand("CREATE TRIGGER refuse_replacement BEFORE INSERT ON Cookie BEGIN SELECT RAISE(ABORT, 'event test'); END"_s),
            "an expired predecessor is present before a replacement insertion is rejected");
        batches.clear();
        check(!database.setCookie(replacement) && batches.isEmpty(),
            "failure after deleting an expired predecessor rolls back without publishing either change");
        auto predecessor = external.prepareStatement("SELECT value, expires FROM Cookie WHERE name='event'"_s);
        check(predecessor && predecessor->step() == SQLITE_ROW && predecessor->columnText(0) == cookie.value && predecessor->columnInt64(1) == 1,
            "rollback restores the expired predecessor and its original contents");
        predecessor = nullptr;
        check(external.executeCommand("DROP TRIGGER refuse_replacement"_s) && database.setCookie(cookie),
            "the rollback fixture returns to a live cookie");
        auto second = cookie;
        second.name = "second"_s;
        check(database.setCookie(second) && external.executeCommand("CREATE TRIGGER reject_second_delete BEFORE DELETE ON Cookie WHEN OLD.name='second' BEGIN SELECT RAISE(ABORT, 'event test'); END"_s),
            "bulk rollback fixture rejects deletion only after an earlier row can be removed");
        batches.clear();
        check(!database.deleteAllCookies() && batches.isEmpty() && database.getAllCookies().size() == 2,
            "partial bulk SQL failure restores every row and publishes no removals");
        check(external.executeCommand("DROP TRIGGER reject_second_delete"_s)
            && external.executeCommand("CREATE TRIGGER invalidate_receipt AFTER UPDATE ON Cookie BEGIN UPDATE Cookie SET samesite=99 WHERE name=NEW.name AND domain=NEW.domain AND path=NEW.path; END"_s),
            "receipt failure fixture invalidates the actual row after SQLite accepts an update");
        check(!database.setCookieWithResult(replacement, receipt) && !receipt && batches.isEmpty(),
            "a failed stored-row receipt rolls back the write and emits no changes");
        auto restored = database.getAllCookies();
        check(restored.size() == 2 && restored[0].value == cookie.value && restored[0].sameSite == cookie.sameSite,
            "receipt validation failure preserves the prior value and valid metadata");
        database.setCookieChangeHandler(nullptr);
    }
    for (auto suffix : { ""_s, "-wal"_s, "-shm"_s })
        unlink(makeString(path, suffix).utf8().legacyCStringPointer());
    rmdir(created);
}

namespace {
struct ExpiryTimerFixture {
    std::unique_ptr<CookieJarDB> database;
    std::unique_ptr<CookieJarDB> unobserved;
    std::unique_ptr<RunLoop::Timer> timeout;
    Function<void()> completion;
    void (*check)(bool, const char*);
    unsigned unexpectedCallbacks { 0 };
};
std::unique_ptr<ExpiryTimerFixture> expiryTimerFixture;

void finishExpiryTimerChecks(bool receivedExpiry)
{
    auto fixture = std::exchange(expiryTimerFixture, nullptr);
    if (!fixture)
        return;
    fixture->check(receivedExpiry, "the real Haiku run-loop timer delivers cookie expiry without another cookie operation");
    fixture->check(!fixture->unexpectedCallbacks, "unregistering a timed observer prevents later expiry callbacks");
    fixture->timeout = nullptr;
    auto completion = WTF::move(fixture->completion);
    // Also exercises destroying the database and its timer from an expiry event.
    fixture = nullptr;
    completion();
}
}

void runCookieExpiryTimerChecks(void (*check)(bool, const char*), Function<void()>&& completion)
{
    expiryTimerFixture = std::make_unique<ExpiryTimerFixture>();
    auto& fixture = *expiryTimerFixture;
    fixture.check = check;
    fixture.completion = WTF::move(completion);
    fixture.database = std::make_unique<CookieJarDB>(":memory:"_s);
    fixture.unobserved = std::make_unique<CookieJarDB>(":memory:"_s);
    fixture.database->open();
    fixture.unobserved->open();
    auto cookie = eventCookie();
    cookie.expires = WallTime::now().secondsSinceEpoch().milliseconds() + 500.0;
    auto session = cookie;
    session.name = "retained-session"_s;
    session.session = true;
    check(fixture.database->setCookie(cookie) && fixture.database->setCookie(session) && fixture.unobserved->setCookie(cookie),
        "native timer fixtures contain a short-lived persistent cookie and a session cookie");
    fixture.unobserved->setCookieChangeHandler(createSharedTask<void(Vector<CookieChange>&&)>([](Vector<CookieChange>&&) {
        if (expiryTimerFixture)
            ++expiryTimerFixture->unexpectedCallbacks;
    }));
    fixture.unobserved->setCookieChangeHandler(nullptr);
    fixture.database->setCookieChangeHandler(createSharedTask<void(Vector<CookieChange>&&)>([check, cookie](Vector<CookieChange>&& changes) {
        if (!expiryTimerFixture)
            return;
        check(changes.size() == 1 && changes[0].removed && changes[0].cause == CookieChangeCause::Expired
            && changes[0].cookie.isKeyEqual(cookie) && changes[0].cookie.value == cookie.value,
            "scheduled expiry delivers the populated expired cookie and its actual cause");
        auto remaining = expiryTimerFixture->database->getAllCookies();
        check(remaining.size() == 1 && remaining[0].session && remaining[0].name == "retained-session"_s,
            "scheduled expiry deletes the persistent row while retaining the session cookie");
        finishExpiryTimerChecks(true);
    }));
    fixture.timeout = std::make_unique<RunLoop::Timer>(Ref { RunLoop::currentSingleton() }, "Cookie expiry test timeout"_s, [] {
        finishExpiryTimerChecks(false);
    });
    fixture.timeout->startOneShot(10_s);
}
