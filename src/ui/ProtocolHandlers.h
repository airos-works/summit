#pragma once
#include "core/Protocol.h"
#include <Handler.h>
#include <Messenger.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
class BFilePanel;
namespace summit {
class SharedProfile;
// One asynchronous chooser per browser window. A reply is used only while
// the requesting tab still displays the document which asked for it.
class ProtocolHandlers final : public BHandler {
public:
    using SourceCheck = std::function<bool(int64, const std::string&)>;
    ProtocolHandlers(std::shared_ptr<SharedProfile>, bool privateBrowsing, SourceCheck,
        std::function<void(const std::string&)> navigate, std::function<void(const std::string&)> error);
    ~ProtocolHandlers() override;
    void Open(const std::string& url, int64 tab, const std::string& source, bool askAlways = false);
    void Register(const std::string& scheme, const std::string& target, bool remove, int64 tab, const std::string& source);
    void MessageReceived(BMessage*) override;
private:
    struct Request {
        uint64 id;
        int64 tab;
        std::string scheme, url, source;
        ProtocolHandler handler;
        bool registration = false;
    };
    std::optional<ProtocolHandler> Saved(const std::string& scheme) const;
    void Save(const Request&);
    void Launch(const Request&);
    void Confirm();
    void Choose();
    void Clear();
    std::shared_ptr<SharedProfile> fProfile;
    bool fPrivate;
    SourceCheck fCheck;
    std::function<void(const std::string&)> fNavigate, fError;
    std::map<std::string, ProtocolHandler> fPrivateHandlers;
    std::set<std::string> fPrivateRemoved;
    std::map<std::string, std::set<std::string>> fPrivateDeclined;
    std::optional<Request> fRequest;
    uint64 fNext = 0;
    BMessenger fAlert;
    std::unique_ptr<BFilePanel> fPanel;
};
}
