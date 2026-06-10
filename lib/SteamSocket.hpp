#pragma once

#ifdef ENABLE_STEAM

#include "Socket.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>


// Virtual ports for the two logical channels, mirroring CCCaster's TCP-control + UDP-data
// split: each is a separate Steam P2P connection.
#define STEAM_CTRL_VPORT ( 0 )
#define STEAM_DATA_VPORT ( 1 )

// A Steam match is addressed by the peer's SteamID, but CCCaster threads an IpAddrPort
// "address" through its UI, launcher, and IPC. We encode the SteamID into that address as
// "steam:<id>" so Steam matches reuse the same plumbing as IP matches.
std::string steamAddr ( uint64_t steamId );             // -> IpAddrPort addr string "steam:<id>"
uint64_t steamIdFromAddr ( const IpAddrPort& address ); // parse "steam:<id>"; 0 if not a steam addr


// A Socket implementation that tunnels CCCaster's protocol over a Steam Datagram Relay
// (SDR) P2P connection, addressed by SteamID instead of IP:port. This sidesteps CGNAT by
// routing through Valve's relay backbone.
//
// Structure mirrors UdpSocket's server/child/accepted model:
//   - A "listen" SteamSocket (isServer) owns a Steam listen socket on a virtual port.
//     When a peer connects, SteamManager routes the incoming connection here; we accept it,
//     wrap it in a child SteamSocket, stash it as _acceptedSocket, and fire socketAccepted.
//   - A "client" SteamSocket owns one outbound connection (ConnectP2P by SteamID).
//   - A "child" SteamSocket wraps one accepted inbound connection on the host.
//
// No fd, so this never registers with SocketManager; SteamManager's pump timer drives it.
// All Steam SDK calls live in SteamManager (this file never includes Steam headers).
class SteamSocket : public Socket
{
public:

    // Host: listen for P2P connections on the given virtual port.
    static SocketPtr listen ( Socket::Owner *owner, int virtualPort );

    // Client: connect to a host by SteamID64 on the given virtual port.
    static SocketPtr connect ( Socket::Owner *owner, uint64_t peerSteamId, int virtualPort );

    ~SteamSocket() override;

    void disconnect() override;

    SocketPtr accept ( Socket::Owner *owner ) override;

    // The remote peer's SteamID64 (0 for a server socket).
    uint64_t getPeerSteamId() const { return _peerSteamId; }

    // Raw byte sends are not used by the netplay path, but override to avoid the base
    // winsock implementation (which would touch the null fd).
    bool send ( const char *buffer, size_t len );
    bool send ( const char *buffer, size_t len, const IpAddrPort& address );

    bool send ( SerializableMessage *message, const IpAddrPort& address = NullAddress ) override;
    bool send ( SerializableSequence *message, const IpAddrPort& address = NullAddress ) override;
    bool send ( const MsgPtr& message, const IpAddrPort& address = NullAddress ) override;

    // ---- called by SteamManager's routing/pump ----

    // Incoming connection arrived on this server socket's listen handle.
    void onIncomingConnection ( uint32_t conn, uint64_t peerSteamId );

    // This connection reached the Connected state.
    void onConnected();

    // This connection closed or failed.
    void onClosed();

    // A decoded message was received on this connection.
    void onReceive ( const MsgPtr& msg );

    friend class SteamManager;

private:

    enum ChildSocketEnum { ChildSocket };

    // Virtual port (host listen port / client target port).
    int _virtualPort = 0;

    // Steam listen handle (server) or connection handle (client/child); 0 == invalid.
    uint32_t _listenHandle = 0;
    uint32_t _conn = 0;

    // Remote peer SteamID64 (client/child only).
    uint64_t _peerSteamId = 0;

    // Bounded re-attempts of a client ConnectP2P that closed before ever reaching Connected (e.g.
    // the SDR relay momentarily flaked). Pairs with SteamManager::waitRelayNetworkReady().
    int _connectRetries = 0;

    // Server: the just-accepted child waiting to be accept()'d out, and live children.
    SocketPtr _acceptedSocket;
    std::unordered_map<uint32_t, SocketPtr> _childSockets;

    // Child: the server socket that accepted this connection (0 for server/client).
    SteamSocket *_parentSocket = 0;

    // Server constructor (listen on a virtual port).
    SteamSocket ( Socket::Owner *owner, int virtualPort );

    // Client constructor (connect to peer SteamID).
    SteamSocket ( Socket::Owner *owner, uint64_t peerSteamId, int virtualPort );

    // Child constructor (one accepted inbound connection on the host).
    SteamSocket ( ChildSocketEnum, Socket::Owner *owner, uint32_t conn, uint64_t peerSteamId, int virtualPort );

    bool sendEncoded ( const MsgPtr& msg, bool reliable );

    // Unused base callback (Steam delivers whole messages via onReceive).
    void socketRead ( const MsgPtr& msg, const IpAddrPort& address ) override {}
};

#endif // ENABLE_STEAM
