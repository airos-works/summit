#include "Containers.h"
#include <WebKit/WebKitContext.h>
#include <cstdio>
#include <cstring>

namespace summit {
Containers& Containers::Shared()
{
    static Containers containers;
    return containers;
}
void Containers::Sync(const std::shared_ptr<BWebKitContext>& profile)
{
    std::vector<BWebKitContainer> engine;
    std::vector<ContainerInfo> list;
    std::map<uint32, std::shared_ptr<BWebKitContext>> contexts;
    {
        std::lock_guard lock(fLock);
        contexts = fContexts;
    }
    if (profile && profile->GetContainers(engine) == B_OK) {
        std::map<uint32, std::shared_ptr<BWebKitContext>> kept;
        for (const auto& container : engine) {
            list.push_back({container.identifier, container.name, container.color, container.icon, container.rgb});
            auto found = contexts.find(container.identifier);
            if (found != contexts.end()) { kept.insert(*found); continue; }
            // Its store and directories are made now, on this thread, so a
            // window can open a tab in it at once.
            auto context = std::make_shared<BWebKitContext>(profile, container.identifier);
            if (context->InitCheck() == B_OK) kept.emplace(container.identifier, std::move(context));
            else std::fprintf(stderr, "Summit: container %u is not available: %s\n", unsigned(container.identifier),
                std::strerror(context->InitCheck()));
        }
        contexts = std::move(kept);
    } else contexts.clear();
    std::lock_guard lock(fLock);
    // Contexts of removed containers go when their last tab closes.
    fList = std::move(list);
    fContexts = std::move(contexts);
}
std::shared_ptr<BWebKitContext> Containers::Context(uint32 id) const
{
    std::lock_guard lock(fLock);
    auto found = fContexts.find(id);
    return found == fContexts.end() ? nullptr : found->second;
}
std::vector<ContainerInfo> Containers::List() const
{
    std::lock_guard lock(fLock);
    return fList;
}
std::optional<ContainerInfo> Containers::Find(uint32 id) const
{
    std::lock_guard lock(fLock);
    for (const auto& container : fList) if (container.id == id) return container;
    return std::nullopt;
}
} // namespace summit
