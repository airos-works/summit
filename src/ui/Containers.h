#pragma once
#include <SupportDefs.h>
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
private:
    mutable std::mutex fLock;
    std::vector<ContainerInfo> fList;
    std::map<uint32, std::shared_ptr<BWebKitContext>> fContexts;
};
} // namespace summit
