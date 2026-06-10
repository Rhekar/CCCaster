#pragma once

#include "IMatchmakingBackend.hpp"
#include "ConsoleUi.hpp"
#include "Socket.hpp"
#include "Timer.hpp"
#include "Pinger.hpp"
#include "KeyboardManager.hpp"

#define DEFAULT_GET_TIMEOUT ( 5000 )

// Relay server-backed matchmaking (the "fallback" backend). A background Thread connects to the
// matchmaking server (MMSTART,region) and is told HOST or CLIENT,<addr>, signalling MainUi's
// uiCondVar via the IMatchmakingBackend::Owner callbacks. See SteamMatchmaking for the
// Steam-backed implementation of the same interface.
class MatchmakingManager
    : Socket::Owner
    , Timer::Owner
    , public Thread
    , public IMatchmakingBackend
{
public:

    MatchmakingManager ( IMatchmakingBackend::Owner* owner, IpAddrPort _address, std::string region );

    void start() override { Thread::start(); }
    bool isRunning() override { return Thread::isRunning(); }
    void stop() override;

    void connect();
    void disconnect();

    void sendHostReady() override;

    uint64_t timeout;
    IpAddrPort _address;

    Mutex hostMutex;
    CondVar hostCondVar;

private:

    SocketPtr serversocket;
    TimerPtr _timer;

    std::string region;

    // Socket callbacks
    void socketAccepted ( Socket *socket ) override {}
    void socketConnected ( Socket *socket ) override;
    void socketDisconnected ( Socket *socket ) override;
    void socketRead ( Socket *socket, const MsgPtr& msg, const IpAddrPort& address ) override {}
    void socketRead ( Socket *socket, const char *bytes, size_t len, const IpAddrPort& address ) override;

    // Timer callback
    void timerExpired ( Timer *timer ) override;

    // Thread
    void run() override;

    // Keyboard
    void keyboardEvent ( uint32_t vkCode, uint32_t scanCode, bool isExtended, bool isDown ) override;
};
