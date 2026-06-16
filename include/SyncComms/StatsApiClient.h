#pragma once

// TCP/JSON consumer for Rocket League's Stats API.
//
// Connects to 127.0.0.1:49123 (configurable in DefaultStatsAPI.ini), reads
// the streaming JSON event feed, and invokes a user-supplied callback for
// each parsed event. Mirrors the framing logic from tools/stats_api_probe.py:
// the wire format isn't strictly defined, so we tolerate newline-delimited,
// concatenated, and stray-prefix-byte framings via streaming JSON parse.
//
// Lifecycle:
//   - Construct → idle.
//   - Start() spins up a background thread that loops connect → read → reconnect.
//   - Stop() flips an atomic flag, closes the socket to interrupt recv(), and
//     joins the thread.
//   - Destructor calls Stop() if needed.

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace SyncComms {

enum class StatsApiConnectionState {
    Disconnected,
    Connecting,
    Connected,
};

class StatsApiClient {
public:
    /// Callback invoked once per parsed event. Receives the *raw JSON* of the
    /// event envelope (including the outer { "event": ..., "data": ... }) so
    /// the caller can re-inject it directly into a JS bridge without having
    /// to re-serialize. Called from the worker thread.
    using EventCallback = std::function<void(const std::string& jsonEvent)>;

    /// Connection state changes are reported here. Called from the worker thread.
    using StateCallback = std::function<void(StatsApiConnectionState newState)>;

    StatsApiClient();
    ~StatsApiClient();

    StatsApiClient(const StatsApiClient&) = delete;
    StatsApiClient& operator=(const StatsApiClient&) = delete;

    void SetEventCallback(EventCallback cb)        { m_onEvent = std::move(cb); }
    void SetStateCallback(StateCallback cb)        { m_onState = std::move(cb); }
    void SetEndpoint(std::string host, uint16_t port) { m_host = std::move(host); m_port = port; }

    void Start();
    void Stop();

    StatsApiConnectionState GetState() const { return m_state.load(); }

private:
    void Run();
    void SetState(StatsApiConnectionState s);

    EventCallback m_onEvent;
    StateCallback m_onState;

    std::string  m_host = "127.0.0.1";
    uint16_t     m_port = 49123;

    std::thread                          m_thread;
    std::atomic<bool>                    m_running{false};
    std::atomic<StatsApiConnectionState> m_state{StatsApiConnectionState::Disconnected};

    // Underlying socket (SOCKET on Windows). Stored as uintptr_t so the header
    // doesn't need to drag in <winsock2.h> for everyone who includes it.
    std::atomic<uintptr_t> m_socket{~uintptr_t{0}}; // INVALID_SOCKET
};

} // namespace SyncComms
