#pragma once
#include <Message.h>
#include <Messenger.h>
#include <SupportDefs.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class BWebKitContext;

namespace summit {
// Firefox's containers (contextualIdentities): tabs whose cookies and site
// storage are their own, apart from the profile's and each other's. The
// engine keeps the list (BWebKitContext::GetContainers; extensions such as
// Multi-Account Containers change it); this keeps an engine context for each
// container, which only the application thread can make, so that windows can
// open tabs in one from their own threads.
struct ContainerInfo {
    uint32 id = 0;
    std::string name, color, icon;
    uint32 rgb = 0;
};
class Containers {
public:
    static Containers& Shared();
    // Application thread: the profile's list again, with a context for each
    // container (made once); nullptr forgets them all (quitting).
    void Sync(const std::shared_ptr<BWebKitContext>& profile);
    // Any thread.
    std::shared_ptr<BWebKitContext> Context(uint32 id) const;
    std::vector<ContainerInfo> List() const;
    std::optional<ContainerInfo> Find(uint32 id) const;
    // The contexts of the containers made so far.
    std::vector<std::shared_ptr<BWebKitContext>> Contexts() const;
private:
    mutable std::mutex fLock;
    std::vector<ContainerInfo> fList;
    std::map<uint32, std::shared_ptr<BWebKitContext>> fContexts;
};
// Website data requests of a profile that go to its own store and to every
// container's, as Firefox clears and lists them (Preferences › Privacy,
// browsingData without a cookieStoreId). Each store answers its own
// identifier (B_WEBKIT_WEBSITE_DATA_REMOVED or _SIZES); Reply() returns the
// request once the last store has, with the first error and the sizes of
// each site added up. Used on one looper.
class StoreRequests {
public:
    struct Request {
        uint64 key = 0;
        int remaining = 0;
        std::string error;
        std::map<std::string, uint64> sizes;
    };
    // Calls issue for the profile's context and each container's, each with
    // an identifier of its own; key names the request to the caller.
    void Issue(uint64 key, const std::shared_ptr<BWebKitContext>& profile,
        const std::function<void(BWebKitContext&, uint64 identifier)>& issue);
    // A store's answer: the whole request when it was the last, else none
    // (also for identifiers of other requests).
    std::optional<Request> Reply(const BMessage&);
    bool Knows(const BMessage& message) const { return fPending.count(message.GetUInt64("identifier", 0)) > 0; }
private:
    std::map<uint64, std::shared_ptr<Request>> fPending;
    uint64 fNext = 0;
};
} // namespace summit
