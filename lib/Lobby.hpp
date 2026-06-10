#pragma once

#include "ILobbyBackend.hpp"
#include "ConsoleUi.hpp"
#include "Socket.hpp"
#include "Timer.hpp"

#define DEFAULT_GET_TIMEOUT ( 5000 )

// Relay/Concerto server-backed lobby (the "fallback" backend). A background Thread runs an
// EventManager loop over a TcpSocket to the lobby server and signals MainUi's uiCondVar via the
// ILobbyBackend::Owner callbacks. See SteamLobby for the Steam-backed implementation of the same
// interface.
class Lobby
    : Socket::Owner
    , Timer::Owner
    , public Thread
    , public ILobbyBackend
{
public:

    std::vector<std::string> getMenu() override;
    std::vector<std::string> getIps() override;
    std::vector<std::string> getIds() override;

    Lobby ( ILobbyBackend::Owner* owner );

    bool connect( std::string url );
    void disconnect();
    void host( std::string name, IpAddrPort port ) override;
    void unhost() override;
    std::string join( std::string name, int selection ) override;
    void join( std::string name, std::string code ) override;
    void challenge( std::string target, IpAddrPort port ) override;
    void create( std::string name, std::string type ) override;
    void preaccept( std::string id ) override;
    void accept() override;
    void end() override;
    void fetchPublicLobby() override;
    bool checkLobbyCode( std::string code ) override;

    void init( ILobbyBackend::Owner* owner );

    // ILobbyBackend lifecycle (unify with Thread's start/isRunning)
    void start() override { Thread::start(); }
    bool isRunning() override { return Thread::isRunning(); }
    void stop() override;

    uint64_t timeout;
    bool newRequestSuccess;


private:
    SocketPtr _socket;

    TimerPtr _timer;

    std::string blankEntry;

    std::vector<std::string> entries;
    std::vector<std::string> lobbyentries;
    std::vector<std::string> publiclobbies;
    std::vector<std::string> roomcodes;
    std::vector<std::string> ips;
    std::vector<std::string> lobbyips;
    std::vector<std::string> lobbyids;

    bool hostResponse;

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

};
