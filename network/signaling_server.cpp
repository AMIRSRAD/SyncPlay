#include "signaling_server.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <random>
#include <unordered_set>

#include "../core/logging.h"
#include "../core/string_utils.h"
#include "../core/utf.h"

#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#endif

namespace {
bool isLinkLocal(uint32_t addr) {
    return (addr & 0xFFFF0000u) == 0xA9FE0000u; // 169.254.0.0/16
}

bool isPrivateIPv4(uint32_t addr) {
    if ((addr & 0xFF000000u) == 0x0A000000u) // 10.0.0.0/8
        return true;
    if ((addr & 0xFFF00000u) == 0xAC100000u) // 172.16.0.0/12
        return true;
    if ((addr & 0xFFFF0000u) == 0xC0A80000u) // 192.168.0.0/16
        return true;
    return false;
}

bool parseIPv4(const std::string& text, uint32_t& out) {
#ifdef _WIN32
    in_addr addr{};
    if (inet_pton(AF_INET, text.c_str(), &addr) != 1)
        return false;
    out = ntohl(addr.S_un.S_addr);
    return true;
#else
    (void)text;
    (void)out;
    return false;
#endif
}

bool isPrivateIPv4Text(const std::string& text) {
    uint32_t addr = 0;
    return parseIPv4(text, addr) && isPrivateIPv4(addr);
}

std::string resolveLocalIPv4() {
#ifdef _WIN32
    ULONG size = 0;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW)
        return "127.0.0.1";

    std::vector<unsigned char> buffer(size);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, addresses, &size) != NO_ERROR)
        return "127.0.0.1";

    std::string privateCandidate;
    std::string publicCandidate;
    for (auto* adapter = addresses; adapter; adapter = adapter->Next) {
        for (auto* ua = adapter->FirstUnicastAddress; ua; ua = ua->Next) {
            auto* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            if (!sa)
                continue;
            const uint32_t addr = ntohl(sa->sin_addr.S_un.S_addr);
            if ((addr >> 24) == 127)
                continue;
            if (isLinkLocal(addr))
                continue;

            char out[INET_ADDRSTRLEN] = {};
            if (!inet_ntop(AF_INET, &sa->sin_addr, out, sizeof(out)))
                continue;

            if (isPrivateIPv4(addr)) {
                if (privateCandidate.empty())
                    privateCandidate = out;
            } else if (publicCandidate.empty()) {
                publicCandidate = out;
            }
        }
    }
    if (!privateCandidate.empty())
        return privateCandidate;
    if (!publicCandidate.empty())
        return publicCandidate;
#endif
    return "127.0.0.1";
}

std::vector<SignalingServer::NetworkInterface> enumerateInterfaces() {
    std::vector<SignalingServer::NetworkInterface> result;
#ifdef _WIN32
    ULONG size = 0;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW)
        return result;

    std::vector<unsigned char> buffer(size);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, addresses, &size) != NO_ERROR)
        return result;

    std::unordered_set<std::string> seen;
    for (auto* adapter = addresses; adapter; adapter = adapter->Next) {
        std::string name;
        if (adapter->FriendlyName) {
            name = Utf8FromWide(adapter->FriendlyName);
            name = Trim(name);
        }
        if (name.empty() && adapter->AdapterName)
            name = Trim(adapter->AdapterName);

        for (auto* ua = adapter->FirstUnicastAddress; ua; ua = ua->Next) {
            auto* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            if (!sa)
                continue;
            const uint32_t addr = ntohl(sa->sin_addr.S_un.S_addr);
            if ((addr >> 24) == 127)
                continue;
            if (isLinkLocal(addr))
                continue;

            char out[INET_ADDRSTRLEN] = {};
            if (!inet_ntop(AF_INET, &sa->sin_addr, out, sizeof(out)))
                continue;

            const std::string address = out;
            if (seen.find(address) != seen.end())
                continue;
            seen.insert(address);
            result.push_back({name, address});
        }
    }
#endif
    return result;
}

bool hasInterfaceAddress(const std::string& address) {
    const std::string trimmed = Trim(address);
    if (trimmed.empty())
        return false;
    const auto interfaces = enumerateInterfaces();
    return std::any_of(interfaces.begin(), interfaces.end(),
                       [&](const SignalingServer::NetworkInterface& iface) {
                           return iface.address == trimmed;
                       });
}

std::string normalizeMode(const std::string& mode) {
    const std::string value = ToLower(Trim(mode));
    (void)value;
    return "relay";
}
}

SignalingServer::SignalingServer() = default;

SignalingServer::~SignalingServer() {
    stop();
}

bool SignalingServer::start(uint16_t port) {
    stop();
    rtc::WebSocketServer::Configuration cfg;
    cfg.port = port;
    std::string bindAddress = Trim(m_preferredAddress);
    if (!bindAddress.empty() && !hasInterfaceAddress(bindAddress)) {
        LogWarn("signaling") << "Preferred interface unavailable, falling back to all interfaces: "
                             << bindAddress << std::endl;
        bindAddress.clear();
        m_preferredAddress.clear();
    }
    const std::string bindForLog = "0.0.0.0";
    cfg.bindAddress = bindForLog;
    LogInfo("signaling") << "Starting signaling server bind " << bindForLog
                         << " requested port " << port
                         << " preferred advertise "
                         << (bindAddress.empty() ? std::string("auto") : bindAddress)
                         << std::endl;
    try {
        m_server = std::make_unique<rtc::WebSocketServer>(cfg);
    } catch (const std::exception& e) {
        LogWarn("signaling") << "Signaling server start failed: " << e.what()
                             << " bind " << bindForLog
                             << " port " << port << std::endl;
        m_server.reset();
        m_port = 0;
        return false;
    } catch (...) {
        LogWarn("signaling") << "Signaling server start failed with unknown error bind "
                             << bindForLog << " port " << port << std::endl;
        m_server.reset();
        m_port = 0;
        return false;
    }
    m_port = m_server->port();
    m_server->onClient([this](std::shared_ptr<rtc::WebSocket> socket) {
        m_events.push([this, socket]() { onClientConnected(socket); });
    });
    LogInfo("signaling") << "Signaling server started on "
                          << (bindAddress.empty() ? std::string("0.0.0.0") : bindAddress)
                          << " port " << m_port << std::endl;
    return m_port > 0;
}

void SignalingServer::stop() {
    if (!m_server)
        return;
    LogInfo("signaling") << "Stopping signaling server port " << m_port << std::endl;
    m_peers.clear();
    m_sessions.clear();
    m_server->stop();
    m_server.reset();
    m_port = 0;
    m_events.clear();
}

std::string SignalingServer::createSession() {
    std::string code;
    do {
        code = generateCode();
    } while (m_sessions.find(code) != m_sessions.end());

    auto session = std::make_shared<Session>();
    session->codes.push_back(code);
    m_sessions[code] = session;
    return code;
}

bool SignalingServer::addAlias(const std::string& alias, const std::string& targetCode) {
    const std::string trimmed = Trim(alias);
    if (trimmed.empty())
        return false;
    auto targetIt = m_sessions.find(targetCode);
    if (targetIt == m_sessions.end())
        return false;

    auto target = targetIt->second;
    auto existing = m_sessions.find(trimmed);
    if (existing != m_sessions.end())
        return existing->second == target;

    target->codes.push_back(trimmed);
    m_sessions[trimmed] = target;
    return true;
}

bool SignalingServer::isRunning() const {
    return m_server != nullptr;
}

uint16_t SignalingServer::port() const {
    return m_port;
}

std::string SignalingServer::serverUrl() const {
    if (!isRunning())
        return std::string();
    const std::string preferred = Trim(m_preferredAddress);
    std::string host = resolveLocalIPv4();
    if (!preferred.empty() && hasInterfaceAddress(preferred)) {
        const bool autoFoundPrivate = isPrivateIPv4Text(host);
        const bool preferredPrivate = isPrivateIPv4Text(preferred);
        if (preferredPrivate || !autoFoundPrivate)
            host = preferred;
        else
            LogWarn("signaling") << "Preferred interface " << preferred
                                 << " is not a private LAN address; advertising "
                                 << host << " instead" << std::endl;
    }
    return "ws://" + host + ":" + std::to_string(m_port);
}

std::vector<SignalingServer::NetworkInterface> SignalingServer::interfaces() const {
    return enumerateInterfaces();
}

std::string SignalingServer::preferredInterface() const {
    return m_preferredAddress;
}

void SignalingServer::setPreferredInterface(const std::string& address) {
    m_preferredAddress = Trim(address);
}

void SignalingServer::pumpEvents() {
    m_events.drain();
}

void SignalingServer::onClientConnected(const std::shared_ptr<rtc::WebSocket>& socket) {
    if (!socket)
        return;
    rtc::WebSocket* raw = socket.get();
    m_peers[raw] = {std::string(), std::string(), socket};
    LogInfo("signaling") << "Client connected " << raw << std::endl;

    socket->onMessage({}, [this, raw](rtc::string text) {
        if (text.empty())
            return;
        const std::string payload = text;
        m_events.push([this, raw, payload]() { onTextMessage(raw, payload); });
    });

    socket->onClosed([this, raw]() {
        m_events.push([this, raw]() { onSocketDisconnected(raw); });
    });

    socket->onError([this, raw](std::string) {
        m_events.push([this, raw]() { onSocketDisconnected(raw); });
    });
}

void SignalingServer::onSocketDisconnected(rtc::WebSocket* socket) {
    if (!socket)
        return;
    LogInfo("signaling") << "Client disconnected " << socket << std::endl;

    auto peerIt = m_peers.find(socket);
    if (peerIt == m_peers.end())
        return;
    PeerInfo info = peerIt->second;
    m_peers.erase(peerIt);

    if (info.code.empty())
        return;

    auto sessionIt = m_sessions.find(info.code);
    if (sessionIt == m_sessions.end())
        return;
    auto session = sessionIt->second;
    if (!session)
        return;

    if (session->host && session->host.get() == socket) {
        session->host.reset();
    }

    session->guests.erase(std::remove_if(session->guests.begin(), session->guests.end(),
                                         [socket](const std::shared_ptr<rtc::WebSocket>& guest) {
                                             return guest && guest.get() == socket;
                                         }), session->guests.end());

    if (!session->host && session->guests.empty()) {
        for (const auto& code : session->codes)
            m_sessions.erase(code);
        return;
    }
    sendPeerUpdate(session);
}

void SignalingServer::onTextMessage(rtc::WebSocket* socket, const std::string& message) {
    if (!socket)
        return;

    auto j = nlohmann::json::parse(message, nullptr, false);
    if (!j.is_object())
        return;

    std::string type = j.value("type", "");
    if (type == "ping") {
        // RTT probe: echo straight back to the sender with its timestamp intact.
        // Works pre-join too, so latency is known before the first state message.
        auto pingPeer = m_peers.find(socket);
        if (pingPeer != m_peers.end() && pingPeer->second.socket) {
            j["type"] = "pong";
            pingPeer->second.socket->send(j.dump());
        }
        return;
    }
    if (type == "join") {
        std::string code = Trim(j.value("code", std::string()));
        std::string role = ToLower(Trim(j.value("role", std::string())));
        std::string mode = normalizeMode(j.value("mode", std::string("relay")));
        LogInfo("signaling") << "Join request role " << role
                             << " code " << code
                             << " mode " << mode
                             << " socket " << socket << std::endl;
        if (code.empty() || (role != "host" && role != "guest")) {
            LogWarn("signaling") << "Join rejected; invalid code/role " << role << " " << code << std::endl;
            return;
        }

        if (m_sessions.find(code) == m_sessions.end()) {
            auto session = std::make_shared<Session>();
            session->codes.push_back(code);
            session->mode = mode;
            m_sessions[code] = session;
            LogInfo("signaling") << "Created session " << code << " mode " << mode << std::endl;
        }

        auto session = m_sessions[code];
        if (!session)
            return;
        if (session->mode.empty())
            session->mode = mode;
        if (session->mode != mode) {
            LogWarn("signaling") << "Join rejected; mode mismatch " << mode << " expected "
                                 << session->mode << std::endl;
            auto peerIt = m_peers.find(socket);
            if (peerIt != m_peers.end() && peerIt->second.socket)
                peerIt->second.socket->close();
            return;
        }

        if (role == "host") {
            if (session->host && session->host.get() != socket)
                session->host->close();
            session->host = m_peers[socket].socket;
        } else {
            bool exists = false;
            for (const auto& guest : session->guests) {
                if (guest.get() == socket) {
                    exists = true;
                    break;
                }
            }
            if (!exists)
                session->guests.push_back(m_peers[socket].socket);
        }

        PeerInfo& info = m_peers[socket];
        info.code = code;
        info.role = role;
        LogInfo("signaling") << "Join " << role << " " << code << " mode " << session->mode << std::endl;
        sendPeerUpdate(session);
        return;
    }

    auto peerIt = m_peers.find(socket);
    if (peerIt == m_peers.end())
        return;

    PeerInfo info = peerIt->second;
    if (m_sessions.find(info.code) == m_sessions.end())
        return;

    auto session = m_sessions[info.code];
    if (!session)
        return;

    if (type == "share_meta" || type == "share_chunk" || type == "share_done") {
        if (session->mode != "relay")
            return;
        j["code"] = info.code;
        std::string payload = j.dump();
        if (info.role == "host") {
            for (const auto& guest : session->guests) {
                if (guest)
                    guest->send(payload);
            }
        } else {
            if (session->host)
                session->host->send(payload);
            for (const auto& guest : session->guests) {
                if (guest && guest.get() != socket)
                    guest->send(payload);
            }
        }
        LogInfo("signaling") << "Relay share " << type << " for " << info.code
                             << " from " << info.role << std::endl;
        return;
    }

    if (type == "intent") {
        if (session->mode != "relay") {
            LogWarn("signaling") << "Intent ignored outside relay session " << info.code << std::endl;
            return;
        }
        if (info.role != "guest") {
            LogWarn("signaling") << "Intent rejected; only guests can send intents" << std::endl;
            return;
        }
        j["code"] = info.code;
        std::string payload = j.dump();
        if (session->host)
            session->host->send(payload);
        LogInfo("signaling") << "Relay intent for " << info.code << " from guest" << std::endl;
        return;
    }

    if (type == "state" || type == "file" || type == "chat" || type == "reaction" ||
        type == "open_url") {
        if (session->mode != "relay") {
            LogWarn("signaling") << "Relay message ignored outside relay session " << type << std::endl;
            return;
        }
        if ((type == "file" || type == "open_url") && info.role != "host") {
            LogWarn("signaling") << "Relay message rejected; only host can send " << type << std::endl;
            return;
        }
        j["code"] = info.code;
        std::string payload = j.dump();
        if (type == "chat" || type == "state" || type == "reaction") {
            if (info.role == "host") {
                for (const auto& guest : session->guests) {
                    if (guest)
                        guest->send(payload);
                }
            } else {
                if (session->host)
                    session->host->send(payload);
                for (const auto& guest : session->guests) {
                    if (guest && guest.get() != socket)
                        guest->send(payload);
                }
            }
        } else {
            for (const auto& guest : session->guests) {
                if (guest)
                    guest->send(payload);
            }
        }
        LogInfo("signaling") << "Relay " << type << " for " << info.code
                             << " from " << info.role << std::endl;
        return;
    }

    if (type == "relay_voice_start" || type == "relay_voice_stop" || type == "relay_voice_frame") {
        if (session->mode != "relay") {
            LogWarn("signaling") << "Relay voice ignored outside relay session " << info.code << std::endl;
            return;
        }
        if (session->guests.size() > 1) {
            LogWarn("signaling") << "Relay voice rejected; multiple guests in session "
                                 << info.code << std::endl;
            return;
        }

        std::shared_ptr<rtc::WebSocket> target;
        if (info.role == "host") {
            if (!session->guests.empty())
                target = session->guests.front();
        } else {
            target = session->host;
        }
        if (!target)
            return;

        j["code"] = info.code;
        const std::string payload = j.dump();
        target->send(payload);
        if (type == "relay_voice_frame") {
            LogDebug("signaling") << "Relay voice frame for " << info.code << std::endl;
        } else {
            LogInfo("signaling") << "Relay " << type << " for " << info.code << std::endl;
        }
        return;
    }
    LogWarn("signaling") << "Message ignored in relay session " << type << std::endl;
}

std::string SignalingServer::generateCode() const {
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    static constexpr int alphabet_len = static_cast<int>(sizeof(alphabet) - 1);
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, alphabet_len - 1);
    std::string code;
    code.reserve(6);
    for (int i = 0; i < 6; ++i) {
        int idx = dist(rng);
        code.push_back(alphabet[idx]);
    }
    return code;
}

std::string SignalingServer::resolveLocalAddress() const {
    return resolveLocalIPv4();
}

void SignalingServer::sendPeerUpdate(const std::shared_ptr<Session>& session) {
    if (!session)
        return;
    const int guestCount = static_cast<int>(session->guests.size());
    nlohmann::json hostPayload{
            {"type", "peer_update"},
            {"host", true},
            {"guests", guestCount}
    };
    if (session->host)
        session->host->send(hostPayload.dump());

    nlohmann::json guestPayload{
            {"type", "peer_update"},
            {"host", session->host != nullptr},
            {"guests", guestCount}
    };
    const std::string payload = guestPayload.dump();
    for (const auto& guest : session->guests) {
        if (guest)
            guest->send(payload);
    }
}
