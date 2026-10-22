#include "core/BookmarkQueries.h"
#include "core/BookmarkIO.h"
#include "core/HistoryVisits.h"
#include "core/Profile.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <unistd.h>
using namespace summit;
using nlohmann::json;
static int checks=0,failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr<<__LINE__<<": " #x "\n"; } } while (0)
static bool Database(const std::string& library,const std::filesystem::path& path,const std::string& sql)
{
    void* handle=dlopen(library.c_str(),RTLD_NOW);if (!handle) return false;
    auto open=reinterpret_cast<int (*)(const char*,void**)>(dlsym(handle,"sqlite3_open"));
    auto exec=reinterpret_cast<int (*)(void*,const char*,void*,void*,char**)>(dlsym(handle,"sqlite3_exec"));
    auto close=reinterpret_cast<int (*)(void*)>(dlsym(handle,"sqlite3_close"));
    void* db=nullptr;const bool okay=open&&exec&&close&&open(path.c_str(),&db)==0&&exec(db,sql.c_str(),nullptr,nullptr,nullptr)==0;
    if (db) close(db);
    dlclose(handle);return okay;
}
static std::string Quote(const std::string& text)
{
    std::string result="'";for (char c:text) {result+=c;if (c=='\'') result+=c;}return result+"'";
}
static std::vector<std::pair<std::string,int64_t>> Rows(const BookmarkQueryResult& r)
{
    std::vector<std::pair<std::string,int64_t>> rows;
    for (size_t i=0;i<r.items.size();++i) rows.emplace_back(r.items[i].url,r.visitTimes.size()==r.items.size()?r.visitTimes[i]:0);
    return rows;
}
int main(int argc,char** argv)
{
    if (argc!=2) return 2;
    setenv("TZ","UTC",1);tzset();
    json fixture;std::ifstream(argv[1])>>fixture;
    Profile profile;std::map<std::string,size_t> urls;
    for (const auto& row:fixture["stored"]) {
        if (row["hidden"]!=0) continue;
        const auto url=row["url"].get<std::string>();
        if (!urls.count(url)) {urls[url]=profile.history.size();profile.history.push_back({url,row["title"],0,false,row["visit_count"]});}
        auto& page=profile.history[urls[url]];page.visitTimes.push_back(row["visit_date"]);
        page.visitTransitions.emplace_back(row["visit_date"],row["visit_type"]);page.visitCountKnown=true;
    }
    for (auto& page:profile.history) SetHistoryVisitTimes(page,page.visitTimes);
    for (const auto& page:fixture["seed"]) if (page["bookmarked"]==true)
        CHECK(profile.bookmarks.Add({page["url"],page["title"]},kBookmarksBarFolder)>0);
    auto query=[&](const std::string& url){return QueryBookmarks(url,profile.bookmarks,profile.history);};
    int compared=0,invalid=0,grouped=0,legacy=0;
    for (const auto& test:fixture["queries"]) {
        const auto url=test["uri"].get<std::string>();const auto actual=query(url);
        if (url.find("onlyBookmarked")!=std::string::npos || url.find("&OR")!=std::string::npos) {++legacy;continue;} // Firefox 158 has one query and removed onlyBookmarked; legacy cases below.
        if (url.find("transition=-")!=std::string::npos || url.find("transition=429496729")!=std::string::npos
            || url.find("transition=&")!=std::string::npos || url.find("transition=not-")!=std::string::npos) {
            ++invalid;CHECK(!actual.error.empty()&&actual.items.empty());continue;
        }
        if (url.rfind("place:type=",0)==0) {++grouped;CHECK(!actual.error.empty()&&actual.items.empty());continue;}
        ++compared;CHECK(actual.error.empty());
        std::vector<std::pair<std::string,int64_t>> expected;
        for (const auto& row:test["rows"]) expected.emplace_back(row["url"],url.find("type=1")!=std::string::npos?row["time"].get<int64_t>():0);
        if (Rows(actual)!=expected) {std::cerr<<"Mismatch "<<url<<"\nActual "<<json(Rows(actual))<<"\nExpected "<<json(expected)<<'\n';CHECK(false);} else CHECK(true);
    }
    CHECK(compared==69 && invalid==15 && grouped==6 && legacy==9);
    CHECK(query("place:transition=7&onlyBookmarked=1").items.size()==4);
    CHECK(query("place:transition=7&parent=toolbar_____").items.size()==4);
    CHECK(query("place:transition=7&domain=transitions.example&terms=both").items.size()==2);
    CHECK(query("place:transition=7&queryType=1&maxVisits=0").items.size()==2);
    CHECK(query("place:transition=1&OR&transition=7").items.size()==7);
    CHECK(query("place:transition=1&OR&transition=7&queryType=1").items.size()==6);
    CHECK(query("place:transition=1&OR&transition=7&type=1").items.size()==11);
    CHECK(query("place:transition=1&OR").items.size()==4);
    const auto zero=query("place:maxVisits=0");CHECK(zero.items.size()==5);
    const auto url="https://transitions.example/repeated-download";
    auto& downloads=profile.history[urls[url]];CHECK(HistoryVisitCount(downloads)==0);
    PageRecord old{url,"Unknown old visit"};CHECK(HistoryVisitCount(old)==1);
    CHECK(QueryBookmarks("place:transition=1",{}, {old}).items.empty());
    // Imports fill missing types even when neither URL nor timestamps change.
    Profile merged;auto unknown=downloads;unknown.visitTransitions.clear();
    bool changed=false;CHECK(merged.ImportHistory({unknown},&changed)==1 && changed);
    CHECK(merged.ImportHistory({downloads},&changed)==0 && changed);
    CHECK(merged.history.front().visitTransitions==downloads.visitTransitions && merged.history.front().visitCountKnown);
    CHECK(merged.ImportHistory({downloads},&changed)==0 && !changed);
    auto collision=downloads;collision.visitTransitions.push_back({collision.visitTimes.front(),1});
    CHECK(merged.ImportHistory({collision},&changed)==0 && changed);
    CHECK(QueryBookmarks("place:transition=1&transition=7",{},merged.history).items.size()==1);
    CHECK(merged.ImportHistory({collision},&changed)==0 && !changed);
    // A new visit with unknown provenance retains old types but creates no link type.
    Profile live;CHECK(live.ImportHistory({downloads})==1);
    CHECK(live.Visit({url,"Visited now"},fixture["base"].get<int64_t>()/1000000+100));
    CHECK(live.history.front().visitTransitions==downloads.visitTransitions);
    CHECK(QueryBookmarks("place:transition=1",{},live.history).items.empty());
    CHECK(QueryBookmarks("place:transition=7",{},live.history).items.size()==1);
    CHECK(HistoryVisitCount(live.history.front())==1);
    // Deleting recent non-counting visits does not subtract an unrelated counted visit.
    Profile deletion;PageRecord visits{url,"Mixed",0,false,1};visits.visitCountKnown=true;
    visits.visitTransitions={{100000000,1},{200000000,7},{300000000,9}};
    SetHistoryVisitTimes(visits,{100000000,200000000,300000000});deletion.history={visits};
    CHECK(deletion.ForgetVisitsSince(200)==2);
    CHECK(deletion.history.size()==1 && HistoryVisitCount(deletion.history.front())==1);
    CHECK((deletion.history.front().visitTransitions==std::vector<std::pair<int64_t,uint32_t>>({{100000000,1}})));
    CHECK(QueryBookmarks("place:transition=7",{},deletion.history).items.empty());
    CHECK(deletion.ForgetVisitsSince(100)==1 && deletion.history.empty());
    visits.visits=0;visits.visitTransitions={{100000000,7},{200000000,7}};
    SetHistoryVisitTimes(visits,{100000000,200000000});deletion.history={visits};
    CHECK(deletion.ForgetVisitsSince(200)==1 && HistoryVisitCount(deletion.history.front())==0);
    CHECK(deletion.history.front().visitTransitions.size()==1);
    // Retention and invalid metadata cannot leave types detached from timestamps.
    PageRecord bounded{url,"Bounded"};std::vector<int64_t> times;
    for (int64_t i=1;i<=250;++i) {times.push_back(1000000*i);bounded.visitTransitions.push_back({1000000*i,uint32_t(i==1?7:1)});}
    bounded.visitTransitions.push_back({999999999,7});bounded.visitTransitions.push_back({250000000,0});bounded.visitTransitions.push_back({250000000,10});
    SetHistoryVisitTimes(bounded,times);CHECK(bounded.visitTimes.size()==kRecentHistoryVisits);
    CHECK(bounded.visitTransitions.size()==kRecentHistoryVisits);
    CHECK(QueryBookmarks("place:transition=7",{}, {bounded}).items.empty());
    // Save/restart preserves both known zero counts and typed timestamps.
    const auto dir=std::filesystem::temp_directory_path()/("summit-transition-tests-"+std::to_string(getpid()));
    std::filesystem::create_directories(dir);std::string error;
    CHECK(profile.Save(dir/"profile.json",error));const auto restored=Profile::Load(dir/"profile.json",error);CHECK(error.empty());
    CHECK(Rows(QueryBookmarks("place:transition=7&type=1&sort=5",restored.bookmarks,restored.history))==Rows(query("place:transition=7&type=1&sort=5")));
    CHECK(Rows(QueryBookmarks("place:transition=7&maxVisits=0&sort=5",restored.bookmarks,restored.history))==Rows(query("place:transition=7&maxVisits=0&sort=5")));
    auto saved=json::parse(std::ifstream(dir/"profile.json"));
    for (const auto& bad:{json::array({json::array({1,-1})}),json::array({json::array({1,4294967296ULL})}),json::array({json::array({1})}),json::array({json::array({"time",7})})}) {
        auto corrupt=saved;corrupt["history"][0]["visitTransitions"]=bad;std::ofstream(dir/"invalid.json")<<corrupt;
        Profile::Load(dir/"invalid.json",error);CHECK(!error.empty());
    }
    auto corrupt=saved;corrupt["history"][0]["visitTransitions"]=std::vector<std::pair<int64_t,uint32_t>>(kMaxHistoryVisitTimes*9+1,{1,7});
    std::ofstream(dir/"oversized.json")<<corrupt;Profile::Load(dir/"oversized.json",error);CHECK(!error.empty());
    corrupt=saved;corrupt["history"][0]["visitCountKnown"]="yes";
    std::ofstream(dir/"bad-count.json")<<corrupt;Profile::Load(dir/"bad-count.json",error);CHECK(!error.empty());
    corrupt=saved;corrupt["history"][0]["visitTransitions"]=json::array({json::array({1,7})});
    std::ofstream(dir/"detached.json")<<corrupt;auto detached=Profile::Load(dir/"detached.json",error);
    CHECK(error.empty() && detached.history.front().visitTransitions.empty());
    Profile legacyProfile;legacyProfile.history={old};CHECK(legacyProfile.Save(dir/"old.json",error));
    auto oldProfile=Profile::Load(dir/"old.json",error);CHECK(error.empty());
    CHECK(!oldProfile.history.front().visitCountKnown && HistoryVisitCount(oldProfile.history.front())==1);
    // Exercise actual Firefox SQLite imports, including its zero-count history.
    std::string library="libsqlite3.so.0";
    if (const char* selected=std::getenv("SUMMIT_TEST_SQLITE")) library=selected;
    std::string sql="CREATE TABLE moz_places(id INTEGER PRIMARY KEY,url TEXT,title TEXT,hidden INTEGER,visit_count INTEGER,last_visit_date INTEGER); CREATE TABLE moz_historyvisits(id INTEGER PRIMARY KEY,place_id INTEGER,visit_date INTEGER,visit_type INTEGER);";
    std::map<std::string,int> ids;int next=0,visitID=0;
    for (const auto& row:fixture["stored"]) {
        const auto address=row["url"].get<std::string>();
        if (!ids.count(address)) {ids[address]=++next;sql+="INSERT INTO moz_places VALUES("+std::to_string(next)+","+Quote(address)+","+Quote(row["title"])+","+row["hidden"].dump()+","+row["visit_count"].dump()+","+row["last_visit_date"].dump()+");";}
        sql+="INSERT INTO moz_historyvisits VALUES("+std::to_string(++visitID)+","+std::to_string(ids[address])+","+row["visit_date"].dump()+","+row["visit_type"].dump()+");";
    }
    CHECK(Database(library,dir/"places.sqlite",sql));std::vector<PageRecord> imported;
    CHECK(ReadFirefoxPlaces(dir/"places.sqlite",library,nullptr,&imported,error));CHECK(imported.size()==profile.history.size());
    CHECK(Rows(QueryBookmarks("place:transition=7&type=1&sort=5",profile.bookmarks,imported))==Rows(query("place:transition=7&type=1&sort=5")));
    CHECK(QueryBookmarks("place:maxVisits=0",{},imported).items.size()==5);
    CHECK(Database(library,dir/"legacy.sqlite","CREATE TABLE moz_places(id INTEGER,url TEXT,title TEXT,hidden INTEGER,visit_count INTEGER,last_visit_date INTEGER); CREATE TABLE moz_historyvisits(id INTEGER,place_id INTEGER,visit_date INTEGER); INSERT INTO moz_places VALUES(1,'https://legacy.test/','Legacy',0,0,1000000); INSERT INTO moz_historyvisits VALUES(1,1,1000000);"));
    CHECK(ReadFirefoxPlaces(dir/"legacy.sqlite",library,nullptr,&imported,error));
    CHECK(imported.size()==1 && imported[0].visitTransitions.empty() && HistoryVisitCount(imported[0])==0);
    CHECK(QueryBookmarks("place:transition=7",{},imported).items.empty());
    // The type belongs to the selected daily MAX timestamp, not an arbitrary
    // other visit on that day. Recent samples and daily samples share a bound.
    std::string daily="CREATE TABLE moz_places(id INTEGER,url TEXT,title TEXT,hidden INTEGER,visit_count INTEGER,last_visit_date INTEGER); CREATE TABLE moz_historyvisits(id INTEGER,place_id INTEGER,visit_date INTEGER,visit_type INTEGER);";
    const int64_t base=fixture["base"];
    for (int page=1;page<=2;++page) {
        daily+="INSERT INTO moz_places VALUES("+std::to_string(page)+",'https://daily.test/"+std::to_string(page)+"','Daily',0,132,"+std::to_string(base+130000000)+");";
        for (int i=1;i<=130;++i) daily+="INSERT INTO moz_historyvisits VALUES("+std::to_string(page*1000+i)+","+std::to_string(page)+","+std::to_string(base+i*1000000)+",1);";
        for (int i=0;i<2;++i) daily+="INSERT INTO moz_historyvisits VALUES("+std::to_string(page*1000+200+i)+","+std::to_string(page)+","+std::to_string(base-86400000000+i*1000000)+","+std::to_string((page==1)==(i==1)?7:1)+");";
    }
    CHECK(Database(library,dir/"daily.sqlite",daily));CHECK(ReadFirefoxPlaces(dir/"daily.sqlite",library,nullptr,&imported,error));
    CHECK(imported.size()==2 && imported[0].visitTimes.size()==129 && imported[1].visitTimes.size()==129);
    const auto dailyDownloads=QueryBookmarks("place:transition=7",{},imported);
    CHECK(dailyDownloads.items.size()==1 && dailyDownloads.items[0].url=="https://daily.test/1");
    std::filesystem::remove_all(dir);
    std::cout<<checks<<" transition query checks, "<<failures<<" failures\n";return failures?1:0;
}
