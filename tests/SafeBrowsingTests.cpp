#include "core/SafeBrowsing.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <unistd.h>

using namespace summit::safebrowsing;

namespace {
int failures = 0;

void Expect(bool condition, const std::string& what)
{
    if (condition) return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

std::string Canonical(const std::string& url)
{
    const auto canonical = Canonicalize(url);
    if (!canonical) return "(none)";
    return canonical->host + canonical->path + (canonical->hasQuery ? "?" + canonical->query : std::string());
}

void ExpectCanonical(const std::string& url, const std::string& expected)
{
    const auto actual = Canonical(url);
    Expect(actual == expected, "canonical " + url + ": " + actual + " != " + expected);
}
}

int main()
{
    // Google Safe Browsing's canonicalisation examples (Update API v4).
    ExpectCanonical("http://host/%25%32%35", "host/%25");
    ExpectCanonical("http://host/%25%32%35%25%32%35", "host/%25%25");
    ExpectCanonical("http://host/%2525252525252525", "host/%25");
    ExpectCanonical("http://host/asdf%25%32%35asd", "host/asdf%25asd");
    ExpectCanonical("http://host/%%%25%32%35asd%%", "host/%25%25%25asd%25%25");
    ExpectCanonical("http://www.google.com/", "www.google.com/");
    ExpectCanonical("http://%31%36%38%2e%31%38%38%2e%39%39%2e%32%36/%2E%73%65%63%75%72%65/%77%77%77%2E%65%62%61%79%2E%63%6F%6D/",
        "168.188.99.26/.secure/www.ebay.com/");
    ExpectCanonical("http://195.127.0.11/uploads/%20%20%20%20/.verify/.eBaysecure=updateuserdataxplimnbqmn-xplmvalidateinfoswqpcmlx=hgplmcx/",
        "195.127.0.11/uploads/%20%20%20%20/.verify/.eBaysecure=updateuserdataxplimnbqmn-xplmvalidateinfoswqpcmlx=hgplmcx/");
    ExpectCanonical("http://host%23.com/%257Ea%2521b%2540c%2523d%2524e%25f%255E00%252611%252A22%252833%252944_55%252B",
        "host%23.com/~a!b@c%23d$e%25f^00&11*22(33)44_55+");
    ExpectCanonical("http://3279880203/blah", "195.127.0.11/blah");
    ExpectCanonical("http://0xc3.0x7f.0.11/blah", "195.127.0.11/blah");
    ExpectCanonical("http://www.google.com/blah/..", "www.google.com/");
    ExpectCanonical("www.google.com/", "www.google.com/");
    ExpectCanonical("www.google.com", "www.google.com/");
    ExpectCanonical("http://www.evil.com/blah#frag", "www.evil.com/blah");
    ExpectCanonical("http://www.GOOgle.com/", "www.google.com/");
    ExpectCanonical("http://www.google.com.../", "www.google.com/");
    ExpectCanonical("http://www.google.com/foo\tbar\rbaz\n2", "www.google.com/foobarbaz2");
    ExpectCanonical("http://www.google.com/q?", "www.google.com/q?");
    ExpectCanonical("http://www.google.com/q?r?", "www.google.com/q?r?");
    ExpectCanonical("http://www.google.com/q?r?s", "www.google.com/q?r?s");
    ExpectCanonical("http://evil.com/foo#bar#baz", "evil.com/foo");
    ExpectCanonical("http://evil.com/foo;", "evil.com/foo;");
    ExpectCanonical("http://evil.com/foo?bar;", "evil.com/foo?bar;");
    ExpectCanonical("http://\x01\x80.com/", "%01%80.com/");
    ExpectCanonical("http://notrailingslash.com", "notrailingslash.com/");
    ExpectCanonical("http://www.gotaport.com:1234/", "www.gotaport.com/");
    ExpectCanonical("  http://www.google.com/  ", "www.google.com/");
    ExpectCanonical("http://%20leadingspace.com/", "%20leadingspace.com/");
    ExpectCanonical("https://www.securesite.com/", "www.securesite.com/");
    ExpectCanonical("http://host.com/ab%23cd", "host.com/ab%23cd");
    ExpectCanonical("http://host.com//twoslashes?more//slashes", "host.com/twoslashes?more//slashes");
    ExpectCanonical("https://user:secret@Example.COM:8443/a/./b/../c", "example.com/a/c");
    ExpectCanonical("example.com:8080/a", "example.com/a");
    ExpectCanonical("ftp://example.com/", "(none)");
    ExpectCanonical("mailto:someone@example.com", "(none)");
    ExpectCanonical("javascript:alert(1)", "(none)");

    // Safe Browsing's lookup example: http://a.b.c/1/2.html?param=1.
    {
        auto expressions = Expressions(*Canonicalize("http://a.b.c/1/2.html?param=1"));
        std::vector<std::string> expected { "a.b.c/1/2.html?param=1", "a.b.c/1/2.html", "a.b.c/", "a.b.c/1/",
            "b.c/1/2.html?param=1", "b.c/1/2.html", "b.c/", "b.c/1/" };
        std::sort(expressions.begin(), expressions.end());
        std::sort(expected.begin(), expected.end());
        Expect(expressions == expected, "expressions of a.b.c/1/2.html?param=1");
        // The last 5 names down to 2, and no more than 4 folders.
        expressions = Expressions(*Canonicalize("http://a.b.c.d.e.f.g/1.html"));
        Expect(expressions.size() == 5 * 2, "a.b.c.d.e.f.g has 5 host forms");
        Expect(std::count(expressions.begin(), expressions.end(), "c.d.e.f.g/") == 1, "c.d.e.f.g/ is looked up");
        Expect(std::count(expressions.begin(), expressions.end(), "b.c.d.e.f.g/") == 0, "b.c.d.e.f.g/ is not looked up");
        expressions = Expressions(*Canonicalize("http://1.2.3.4/1/2/3/4/5/6.html"));
        std::vector<std::string> ip { "1.2.3.4/1/2/3/4/5/6.html", "1.2.3.4/", "1.2.3.4/1/", "1.2.3.4/1/2/", "1.2.3.4/1/2/3/" };
        Expect(expressions == ip, "an IP address has no suffixes, and 4 folders");
    }

    // List lines.
    Expect(LineExpression("evil.example") == "evil.example/", "a host line");
    Expect(LineExpression("  Evil.Example  ") == "evil.example/", "a host line in capitals");
    Expect(LineExpression("0.0.0.0 evil.example # note") == "evil.example/", "a hosts-file line");
    Expect(LineExpression("||evil.example^$all") == "evil.example/", "an Adblock host rule");
    Expect(LineExpression("||evil.example/files/x.exe^$all") == "evil.example/files/x.exe", "an Adblock address rule");
    Expect(LineExpression("||github.com/a/b/raw/main/pdf%20reader.exe^$all") == "github.com/a/b/raw/main/pdf%20reader.exe",
        "an escaped Adblock rule");
    Expect(LineExpression("https://phish.example/login/?a=1") == "phish.example/login/?a=1", "an address line");
    Expect(!LineExpression("# comment"), "a comment");
    Expect(!LineExpression("! Title: list"), "an Adblock comment");
    Expect(!LineExpression("[Adblock Plus 2.0]"), "an Adblock header");
    Expect(!LineExpression("@@||good.example^"), "an exception");
    Expect(!LineExpression("||ads.example^$third-party"), "a rule for other requests");
    Expect(!LineExpression("||*.example^"), "a wildcard rule");
    Expect(!LineExpression("0.0.0.0 localhost"), "localhost");
    Expect(!LineExpression("not a host!"), "junk");

    // Matching, as the lists are used.
    {
        const std::string text = "# Phishing Army\nphish.example\nsub.shared.example\n\nhttps://host.example/bad/\n";
        Database database;
        database.SetList(Threat::Phishing, HashList(ParseList(text)));
        database.SetList(Threat::Malware, HashList(ParseList("! URLhaus\n||mal.example/x/payload.exe^$all\n9.8.7.6\n")));
        Expect(database.Count(Threat::Phishing) == 3, "three phishing entries");
        Expect(database.Check("https://phish.example/") == Threat::Phishing, "a listed host");
        Expect(database.Check("https://www.PHISH.example:443/login?x=1#top") == Threat::Phishing, "a listed host's subdomain and page");
        Expect(database.Check("https://shared.example/") == Threat::None, "the parent of a listed subdomain");
        Expect(database.Check("https://sub.shared.example/a") == Threat::Phishing, "a listed subdomain");
        Expect(database.Check("https://host.example/bad/page.html") == Threat::Phishing, "a page in a listed folder");
        Expect(database.Check("https://host.example/good/") == Threat::None, "a page of the same host outside it");
        Expect(database.Check("http://mal.example/x/payload.exe") == Threat::Malware, "a listed file");
        Expect(database.Check("http://mal.example/x/PAYLOAD.EXE?dl=1") == Threat::Malware, "the file with a query, in capitals");
        Expect(database.Check("http://mal.example/x/other.exe") == Threat::None, "another file there");
        Expect(database.Check("http://9.8.7.6:8080/i.sh") == Threat::Malware, "a listed IP address");
        Expect(database.Check("http://0x9.8.7.6/") == Threat::Malware, "a listed IP address spelt in hex");
        Expect(database.Check("about:blank") == Threat::None, "about:blank");
        Expect(database.Check("file:///boot/home/phish.example") == Threat::None, "a file");
        database.AllowHost("https://www.phish.example/login");
        Expect(database.Check("https://www.phish.example/") == Threat::None, "an allowed host");
        Expect(database.Check("https://phish.example/") == Threat::Phishing, "the allowed host's parent");
        Expect(database.AddTestEntry(Threat::Malware, "http://192.168.1.200:8890/test/malware.html"), "a test entry");
        Expect(database.Check("http://192.168.1.200:8890/test/malware.html") == Threat::Malware, "a test address");
        Expect(database.Check("http://192.168.1.200:8890/test/other.html") == Threat::None, "beside a test address");
        database.SetList(Threat::Malware, HashList());
        Expect(database.Check("http://192.168.1.200:8890/test/malware.html") == Threat::Malware, "test entries outlive list updates");
    }

    // Saved and loaded.
    {
        const auto directory = std::filesystem::temp_directory_path() / ("summit-safe-browsing-" + std::to_string(getpid()));
        std::filesystem::create_directories(directory);
        HashList list(ParseList("a.example\nb.example\nhttps://c.example/x\n"));
        std::string error;
        Expect(list.Save(directory / "phishing.bin", error), "saved: " + error);
        const auto loaded = HashList::Load(directory / "phishing.bin", error);
        Expect(loaded && loaded->Size() == 3 && loaded->Contains(ExpressionHash("b.example/")), "loaded: " + error);
        Expect(!HashList::Load(directory / "missing.bin", error), "a missing list");
        std::filesystem::resize_file(directory / "phishing.bin", 30);
        Expect(!HashList::Load(directory / "phishing.bin", error), "a list cut short");
        std::filesystem::remove_all(directory);
    }
    if (failures) {
        std::cerr << failures << " safe browsing test(s) failed\n";
        return 1;
    }
    std::cout << "safe browsing tests passed\n";
    return 0;
}
