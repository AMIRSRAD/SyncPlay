#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <rtc/websocket.hpp>
#include <rtc/websocketserver.hpp>

#include "../core/task_queue.h"

class SignalingServer {
public:
    struct NetworkInterface {
        std::string name;
        std::string address;
    };

    SignalingServer();
    ~SignalingServer();

    bool start(uint16_t port = 0);
    void stop();

    std::string createSession();
    bool addAlias(const std::string& alias, const std::string& targetCode);
    bool isRunning() const;
    uint16_t port() const;
    std::string serverUrl() const;
    std::vector<NetworkInterface> interfaces() const;
    std::string preferredInterface() const;
    void setPreferredInterface(const std::string& address);

    void pumpEvents();

private:
    struct Session {
        std::shared_ptr<rtc::WebSocket> host;
        std::vector<std::string> codes;
        std::vector<std::shared_ptr<rtc::WebSocket>> guests;
        std::string mode;
    };

    struct PeerInfo {
        std::string code;
        std::string role;
        std::shared_ptr<rtc::WebSocket> socket;
    };

    void onClientConnected(const std::shared_ptr<rtc::WebSocket>& socket);
    void onSocketDisconnected(rtc::WebSocket* socket);
    void onTextMessage(rtc::WebSocket* socket, const std::string& message);

    std::string generateCode() const;
    std::string resolveLocalAddress() const;
    void sendPeerUpdate(const std::shared_ptr<Session>& session);

    std::unique_ptr<rtc::WebSocketServer> m_server;
    std::unordered_map<std::string, std::shared_ptr<Session>> m_sessions;
    std::unordered_map<rtc::WebSocket*, PeerInfo> m_peers;
    uint16_t m_port = 0;
    std::string m_preferredAddress;

    TaskQueue m_events;
};
