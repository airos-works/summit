#include "core/WebAppManifest.h"
#include <cstdlib>
#include <iostream>

int main()
{
    using namespace summit;
    unsigned checks = 0;
    auto check = [&](bool value, const std::string& label) {
        ++checks;
        if (!value) { std::cerr << "FAIL: " << label << '\n'; std::exit(1); }
    };
    check(ResolveURL("https://a.example/app/page.html", "manifest.json") == "https://a.example/app/manifest.json", "relative");
    check(ResolveURL("https://a.example/app/page.html", "/start?x=1") == "https://a.example/start?x=1", "root relative");
    check(ResolveURL("https://a.example/app/sub/page.html", "../icons/a.png") == "https://a.example/app/icons/a.png", "dot dot");
    check(ResolveURL("https://a.example/app/", "./") == "https://a.example/app/", "dot");
    check(ResolveURL("https://a.example/app/", "//cdn.example/i.png") == "https://cdn.example/i.png", "scheme relative");
    check(ResolveURL("https://a.example/app/", "https://b.example/x") == "https://b.example/x", "absolute");
    check(ResolveURL("https://a.example", "icon.png") == "https://a.example/icon.png", "origin base");

    // A manifest with a name, start address, scope and icons.
    auto plan = PlanWebApp(R"({"url":"https://ha.example/lovelace/0","manifestURL":"https://ha.example/manifest.json",
        "manifest":{"name":"Home Assistant","short_name":"HA","start_url":"/?homescreen=1","scope":"/","display":"standalone",
        "icons":[{"src":"/static/icons/favicon-192x192.png","sizes":"192x192","type":"image/png"},
                 {"src":"/static/icons/favicon-1024x1024.png","sizes":"1024x1024","type":"image/png"},
                 {"src":"/static/icons/mask.svg","sizes":"any","type":"image/svg+xml"}]},
        "icons":[],"title":"Overview – Home Assistant","appName":""})");
    check(plan.name == "Home Assistant", "manifest name");
    check(plan.startURL == "https://ha.example/?homescreen=1", "start url");
    check(plan.scope == "https://ha.example/", "scope");
    check(plan.iconURL == "https://ha.example/static/icons/favicon-192x192.png", "icon of at least 64 px, smallest");
    check(plan.display == "standalone", "display");
    check(InWebAppScope(plan.scope, "https://ha.example/config"), "in scope");
    check(!InWebAppScope(plan.scope, "https://other.example/"), "out of scope");

    // No manifest: the title, the page, its directory and the page's icon.
    plan = PlanWebApp(R"({"url":"https://mail.example/inbox/today","manifest":null,"manifestURL":"",
        "icons":[{"src":"https://mail.example/favicon.ico","sizes":"","type":""}],"title":"Inbox","appName":"Mail"})");
    check(plan.name == "Mail", "application-name");
    check(plan.startURL == "https://mail.example/inbox/today", "page as start");
    check(plan.scope == "https://mail.example/inbox/", "page directory as scope");
    check(plan.iconURL == "https://mail.example/favicon.ico", "page icon");

    // A start address on another site is not accepted.
    plan = PlanWebApp(R"({"url":"https://x.example/","manifestURL":"https://x.example/m.json",
        "manifest":{"short_name":"X","start_url":"https://evil.example/"},"icons":[],"title":""})");
    check(plan.name == "X" && plan.startURL == "https://x.example/", "foreign start ignored");
    check(PlanWebApp(R"({"url":"file:///boot/home/a.html","manifest":null})").name.empty(), "not a web page");
    check(PlanWebApp("not json").name.empty(), "bad json");

    check(LauncherName("A/B: C") == "A-B- C", "launcher name");
    check(LauncherName("   ") == "Web App", "empty launcher name");
    check(LauncherName(std::string(80, 'x')).size() == 64, "long launcher name");
    std::cout << checks << " web app checks passed\n";
}
