#include "Location.h"
#include <Entry.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Path.h>
#include <Roster.h>
#include <NetworkAddress.h>
#include <NetworkDevice.h>
#include <NetworkInterface.h>
#include <NetworkRoster.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace summit {
namespace {
constexpr const char* kHost = "api.beacondb.net";

struct AccessPoint {
    std::string address;
    int signal = 0;
};

// The Wi-Fi networks the system last saw. Networks whose name ends in
// "_nomap" asked not to be used for location, as other services honour.
std::vector<AccessPoint> NearbyNetworks()
{
    std::vector<AccessPoint> found;
    BNetworkRoster& roster = BNetworkRoster::Default();
    BNetworkInterface interface;
    uint32 cookie = 0;
    while (roster.GetNextInterface(&cookie, interface) == B_OK) {
        BNetworkDevice device(interface.Name());
        if (!device.IsWireless()) continue;
        wireless_network* networks = nullptr;
        uint32 count = 0;
        if (device.GetNetworks(networks, count) != B_OK || count < 2) {
            delete[] networks;
            networks = nullptr;
            count = 0;
            // Only the networks seen lately: no forced scan, which would
            // interrupt the connection.
            device.Scan(true, false);
            if (device.GetNetworks(networks, count) != B_OK) {
                delete[] networks;
                continue;
            }
        }
        for (uint32 i = 0; i < count; ++i) {
            const auto& network = networks[i];
            const std::string name(network.name, strnlen(network.name, sizeof(network.name)));
            if (name.size() >= 6 && name.compare(name.size() - 6, 6, "_nomap") == 0) continue;
            const uint8* mac = network.address.LinkLevelAddress();
            if (!mac || network.address.LinkLevelAddressLength() != 6) continue;
            char text[18];
            std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            // Locally administered addresses (phones' hotspots, randomised
            // addresses) say nothing about where they are.
            if (mac[0] & 0x02) continue;
            found.push_back({text, network.signal_strength ? static_cast<int>(network.signal_strength) - 256 : 0});
        }
        delete[] networks;
    }
    return found;
}

size_t Collect(char* data, size_t size, size_t count, void* user)
{
    auto* response = static_cast<std::string*>(user);
    if (response->size() + size * count > 65536) return 0;
    response->append(data, size * count);
    return size * count;
}

// One HTTPS request through libcurl, which checks the certificate against the
// system's authorities (BSecureSocket is a stub on builds without OpenSSL in
// the network kit).
bool Post(const std::string& body, std::string& response, std::string& error)
{
    static std::once_flag initialized;
    std::call_once(initialized, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "BeaconDB could not be reached";
        return false;
    }
    struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, (std::string("https://") + kHost + "/v1/geolocate").c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Summit (Haiku web browser)");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) {
        error = std::string("BeaconDB could not be reached: ") + curl_easy_strerror(result);
        return false;
    }
    if (status == 404) {
        error = "BeaconDB does not know where this network is";
        return false;
    }
    if (status != 200) {
        error = "BeaconDB answered with status " + std::to_string(status);
        return false;
    }
    return true;
}
}

// air/OS's location service (since 28 October; its private
// <location_server.h>): the position the user entered in the system's
// settings, else the same Wi-Fi and network-address lookup as below, cached
// for every program, and a system-wide switch. Asked first; Haiku without it
// gets the lookup below. Returns false when there is no such service.
bool AskLocationServer(BMessage& result)
{
    constexpr const char* kSignature = "application/x-vnd.Haiku-location_server";
    constexpr uint32 kGetPosition = 'lcgp';
    BMessenger server(kSignature);
    // It is started on demand.
    if (!server.IsValid() && be_roster->Launch(kSignature) == B_OK)
        server = BMessenger(kSignature);
    if (!server.IsValid()) return false;
    BMessage request(kGetPosition), answer;
    // A lookup takes about a second; the cache answers at once.
    if (server.SendMessage(&request, &answer, 5000000, 30000000) != B_OK) return false;
    const status_t status = answer.GetInt32("status", B_ERROR);
    if (status == B_NOT_ALLOWED) {
        result.AddString("error", "Location is turned off in the system's settings");
        return true;
    }
    double latitude, longitude;
    if (status != B_OK || answer.FindDouble("latitude", &latitude) != B_OK || answer.FindDouble("longitude", &longitude) != B_OK) {
        result.AddString("error", "The system's location service could not find this computer");
        return true;
    }
    result.AddDouble("latitude", latitude);
    result.AddDouble("longitude", longitude);
    result.AddDouble("accuracy", answer.GetDouble("accuracy", 25000.0));
    const int64 determined = answer.GetInt64("time", 0);
    result.AddDouble("timestamp", determined > 0 ? determined / 1e6 : static_cast<double>(std::time(nullptr)));
    if (const char* source = answer.GetString("source", nullptr)) result.AddString("source", source);
    return true;
}

bool HasSystemLocationService()
{
    static const bool present = [] {
        BPath servers;
        return find_directory(B_SYSTEM_SERVERS_DIRECTORY, &servers) == B_OK
            && servers.Append("location_server") == B_OK && BEntry(servers.Path()).Exists();
    }();
    return present;
}

void LookUpLocation(BMessenger reply, uint32 what)
{
    std::thread([reply, what] {
        BMessage result(what);
        if (AskLocationServer(result)) {
            reply.SendMessage(&result);
            return;
        }
        try {
            nlohmann::json request = {{"considerIp", true}};
            const auto networks = NearbyNetworks();
            // A single network can not be placed; services want two at least.
            if (networks.size() >= 2) {
                auto list = nlohmann::json::array();
                for (const auto& network : networks) {
                    nlohmann::json entry = {{"macAddress", network.address}};
                    if (network.signal < 0 && network.signal > -120) entry["signalStrength"] = network.signal;
                    list.push_back(std::move(entry));
                }
                request["wifiAccessPoints"] = std::move(list);
            }
            std::string response, error;
            if (!Post(request.dump(), response, error)) {
                result.AddString("error", error.c_str());
            } else {
                const auto answer = nlohmann::json::parse(response);
                const auto& location = answer.at("location");
                result.AddDouble("latitude", location.at("lat").get<double>());
                result.AddDouble("longitude", location.at("lng").get<double>());
                result.AddDouble("accuracy", answer.value("accuracy", 25000.0));
                result.AddDouble("timestamp", static_cast<double>(std::time(nullptr)));
                result.AddInt32("networks", static_cast<int32>(networks.size()));
                if (answer.contains("fallback")) result.AddString("fallback", answer["fallback"].get<std::string>().c_str());
            }
        } catch (const std::exception&) {
            result.AddString("error", "BeaconDB's answer could not be read");
        }
        reply.SendMessage(&result);
    }).detach();
}
}
