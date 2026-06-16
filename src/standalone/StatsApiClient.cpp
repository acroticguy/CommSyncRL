#include "SyncComms/StatsApiClient.h"

// Force WIN32_LEAN_AND_MEAN before <windows.h> so winsock2 isn't shadowed by
// the legacy winsock1 declarations dragged in by windows.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <string>

#pragma comment(lib, "ws2_32.lib")

namespace SyncComms {

namespace {

constexpr int kRecvBufBytes        = 65536;
constexpr int kReconnectDelayMs    = 2000;
constexpr int kConnectTimeoutMs    = 5000; // not enforced rigorously here; OS default kicks in

// Streaming JSON splitter. Matches the Python probe's behavior: handle
// newline-delimited, concatenated, or stray-prefix-byte streams. Mutates `buf`
// (consuming whatever it parses) and yields each parsed object as a JSON
// string back to `out`. Returns true if any objects were produced.
bool DrainJsonObjects(std::string& buf, std::vector<std::string>& out) {
    bool produced = false;
    while (true) {
        // Skip leading whitespace.
        size_t start = 0;
        while (start < buf.size() && (buf[start] == ' ' || buf[start] == '\r' ||
                                      buf[start] == '\n' || buf[start] == '\t')) {
            ++start;
        }
        if (start == buf.size()) {
            buf.clear();
            return produced;
        }

        // Skip up to next '{' (handles length-prefix junk if any emitter inserts it).
        if (buf[start] != '{') {
            size_t open = buf.find('{', start);
            if (open == std::string::npos) {
                buf.clear();
                return produced;
            }
            buf.erase(0, open);
            continue;
        }

        if (start > 0) buf.erase(0, start);

        // Try to parse a single complete JSON object from the start of buf.
        // We use a brace-depth scanner that respects strings + escapes, which
        // is faster than asking nlohmann::json to parse-and-fail repeatedly
        // on partial inputs (and avoids the throw cost).
        int depth = 0;
        bool inStr = false;
        bool esc = false;
        size_t end = std::string::npos;
        for (size_t i = 0; i < buf.size(); ++i) {
            char c = buf[i];
            if (inStr) {
                if (esc) { esc = false; }
                else if (c == '\\') { esc = true; }
                else if (c == '"') { inStr = false; }
            } else {
                if (c == '"') { inStr = true; }
                else if (c == '{') { ++depth; }
                else if (c == '}') {
                    if (--depth == 0) { end = i + 1; break; }
                }
            }
        }
        if (end == std::string::npos) {
            // Incomplete object; wait for more bytes.
            return produced;
        }

        std::string objStr = buf.substr(0, end);
        buf.erase(0, end);

        // Validate it actually parses (catches malformed nesting). Cheap on
        // small payloads and we only want to forward valid JSON downstream.
        try {
            (void)nlohmann::json::parse(objStr);
            out.push_back(std::move(objStr));
            produced = true;
        } catch (...) {
            // Drop and continue.
        }
    }
}

class WinsockBootstrap {
public:
    WinsockBootstrap() {
        WSADATA d{};
        m_ok = (WSAStartup(MAKEWORD(2, 2), &d) == 0);
    }
    ~WinsockBootstrap() {
        if (m_ok) WSACleanup();
    }
    bool ok() const { return m_ok; }
private:
    bool m_ok = false;
};

WinsockBootstrap& Winsock() {
    static WinsockBootstrap g;
    return g;
}

} // namespace

StatsApiClient::StatsApiClient() {
    Winsock(); // ensure WSAStartup ran
}

StatsApiClient::~StatsApiClient() {
    Stop();
}

void StatsApiClient::SetState(StatsApiConnectionState s) {
    auto prev = m_state.exchange(s);
    if (prev != s && m_onState) {
        m_onState(s);
    }
}

void StatsApiClient::Start() {
    if (m_running.exchange(true)) return; // already running
    m_thread = std::thread([this] { Run(); });
}

void StatsApiClient::Stop() {
    if (!m_running.exchange(false)) {
        if (m_thread.joinable()) m_thread.join();
        return;
    }
    // Force any blocking recv() to wake up by closing the socket.
    SOCKET s = static_cast<SOCKET>(m_socket.exchange(static_cast<uintptr_t>(INVALID_SOCKET)));
    if (s != INVALID_SOCKET) {
        ::shutdown(s, SD_BOTH);
        ::closesocket(s);
    }
    if (m_thread.joinable()) m_thread.join();
}

void StatsApiClient::Run() {
    if (!Winsock().ok()) {
        SetState(StatsApiConnectionState::Disconnected);
        return;
    }

    while (m_running.load()) {
        SetState(StatsApiConnectionState::Connecting);

        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            SetState(StatsApiConnectionState::Disconnected);
            std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectDelayMs));
            continue;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(m_port);
        ::inet_pton(AF_INET, m_host.c_str(), &addr.sin_addr);

        if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::closesocket(s);
            SetState(StatsApiConnectionState::Disconnected);
            // Sleep in 100ms slices so Stop() can interrupt promptly.
            for (int i = 0; i < kReconnectDelayMs / 100 && m_running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        m_socket.store(static_cast<uintptr_t>(s));
        SetState(StatsApiConnectionState::Connected);

        std::string buf;
        std::vector<std::string> out;
        char chunk[kRecvBufBytes];
        bool peerClosed = false;

        while (m_running.load() && !peerClosed) {
            int n = ::recv(s, chunk, sizeof(chunk), 0);
            if (n > 0) {
                buf.append(chunk, static_cast<size_t>(n));
                out.clear();
                DrainJsonObjects(buf, out);
                if (m_onEvent) {
                    for (auto& obj : out) m_onEvent(obj);
                }
            } else if (n == 0) {
                peerClosed = true;
            } else {
                int err = WSAGetLastError();
                if (err == WSAEINTR || err == WSAESHUTDOWN || err == WSAENOTSOCK ||
                    err == WSAECONNRESET || err == WSAECONNABORTED) {
                    break;
                }
                // Other errors: bail and let outer loop reconnect.
                break;
            }
        }

        // Tear down current socket; outer loop will reconnect if still running.
        m_socket.store(static_cast<uintptr_t>(INVALID_SOCKET));
        ::shutdown(s, SD_BOTH);
        ::closesocket(s);
        SetState(StatsApiConnectionState::Disconnected);

        if (m_running.load()) {
            for (int i = 0; i < kReconnectDelayMs / 100 && m_running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    SetState(StatsApiConnectionState::Disconnected);
}

} // namespace SyncComms
