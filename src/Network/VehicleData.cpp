/*
 Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
 Licensed under AGPL-3.0 (or later), see <https://www.gnu.org/licenses/>.
 SPDX-License-Identifier: AGPL-3.0-or-later
*/

#include "Network/network.hpp"
#include "Voice/VoiceManager.hpp"
#include "Zlib/Compressor.h"
#include <stdexcept>

#if defined(_WIN32)
#include <ws2tcpip.h>
#elif defined(__linux__)
#include "linuxfixes.h"
#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

#include "Logger.h"
#include <array>
#include <chrono>
#include <sstream>
#include <string>
#include <vector>

SOCKET UDPSock = -1;
sockaddr_in* ToServer = nullptr;

void UDPSend(std::string Data) {
    if (ClientID == -1 || UDPSock == -1)
        return;
    if (Data.length() > 400) {
        auto res = Comp(std::span<char>(Data.data(), Data.size()));
        Data = "ABG:" + std::string(res.data(), res.size());
    }
    std::string Packet = char(ClientID + 1) + std::string(":") + Data;
    int sendOk = sendto(UDPSock, Packet.c_str(), int(Packet.size()), 0, (sockaddr*)ToServer, sizeof(*ToServer));
    if (sendOk == SOCKET_ERROR)
        error("Error Code : " + std::to_string(WSAGetLastError()));
}

void SendLarge(std::string Data) {
    if (Data.length() > 400) {
        auto res = Comp(std::span<char>(Data.data(), Data.size()));
        Data = "ABG:" + std::string(res.data(), res.size());
    }
    TCPSend(Data, TCPSock);
}

void UDPParser(std::string_view Packet) {
    if (Packet.substr(0, 4) == "ABG:") {
        auto substr = Packet.substr(4);
        try {
            auto res = DeComp(std::span<const char>(substr.data(), substr.size()));
            std::string DeCompPacket = std::string(res.data(), res.size());
            ServerParser(DeCompPacket);
        } catch (const std::runtime_error& err) {
            error("Error in decompression of UDP, ignoring");
        }
    } else {
        if (Packet.rfind("Zp:", 0) == 0) {
            // Best-effort parsing for vehicle position broadcasts:
            // Zp:<playerId>:<x>:<y>:<z>[:<vx>:<vy>:<vz>]
            std::vector<std::string_view> parts {};
            std::string_view body = Packet.substr(3);
            size_t start = 0;
            while (start <= body.size()) {
                size_t sep = body.find(':', start);
                if (sep == std::string_view::npos) {
                    parts.push_back(body.substr(start));
                    break;
                }
                parts.push_back(body.substr(start, sep - start));
                start = sep + 1;
            }
            if (parts.size() >= 4) {
                auto toFloat = [](std::string_view value, float& out) {
                    std::stringstream ss(std::string(value));
                    ss >> out;
                    return !ss.fail();
                };
                int player_id = -1;
                try {
                    player_id = std::stoi(std::string(parts[0]));
                } catch (...) {
                    player_id = -1;
                }
                if (player_id >= 0) {
                    Voice::SpatialState state {};
                    bool ok = toFloat(parts[1], state.x) && toFloat(parts[2], state.y) && toFloat(parts[3], state.z);
                    if (ok && parts.size() >= 7) {
                        ok = toFloat(parts[4], state.vx) && toFloat(parts[5], state.vy) && toFloat(parts[6], state.vz);
                    }
                    if (ok) {
                        state.updated_at = std::chrono::steady_clock::now();
                        Voice::VoiceManager::Get().UpdateSpatialState(player_id, state);
                    }
                }
            }
        }
        ServerParser(Packet);
    }
}
void UDPRcv() {
    sockaddr_in FromServer {};
#if defined(_WIN32)
    int clientLength = sizeof(FromServer);
#elif defined(__linux__)
    socklen_t clientLength = sizeof(FromServer);
#endif
    ZeroMemory(&FromServer, clientLength);
    static thread_local std::array<char, 10240> Ret {};
    if (UDPSock == -1)
        return;
    int32_t Rcv = recvfrom(UDPSock, Ret.data(), Ret.size() - 1, 0, (sockaddr*)&FromServer, &clientLength);
    if (Rcv == SOCKET_ERROR)
        return;
    Ret[Rcv] = 0;
    UDPParser(std::string_view(Ret.data(), Rcv));
}
void UDPClientMain(const std::string& IP, int Port) {
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(514, &data)) {
        error("Can't start Winsock!");
        return;
    }
#endif

    delete ToServer;
    ToServer = new sockaddr_in;
    ToServer->sin_family = AF_INET;
    ToServer->sin_port = htons(Port);
    inet_pton(AF_INET, IP.c_str(), &ToServer->sin_addr);
    UDPSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (!magic.empty())
        for (int i = 0; i < 10; i++)
            UDPSend(magic);
    GameSend("P" + std::to_string(ClientID));
    TCPSend("H", TCPSock);
    UDPSend("p");
    debug("Starting UDP receive loop");
    while (!Terminate) {
        UDPRcv();
    }
    debug("UDP receive loop done");
    KillSocket(UDPSock);
    WSACleanup();
}
