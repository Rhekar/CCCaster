#ifdef ENABLE_STEAM

#include "SteamSocket.hpp"
#include "SteamManager.hpp"
#include "Logger.hpp"

#include <string>
#include <cstdlib>

using namespace std;


#define LOG_STEAM_SOCKET(SOCKET, FORMAT, ...) \
    LOG ( "SteamSocket=%08x; conn=%u; listen=%u; peer=%llu; state=%s; " FORMAT, \
          SOCKET, SOCKET->_conn, SOCKET->_listenHandle, \
          ( unsigned long long ) SOCKET->_peerSteamId, SOCKET->_state, ## __VA_ARGS__ )

// How many times a client ConnectP2P that closed before connecting is re-attempted (see onClosed).
#define STEAM_CONNECT_RETRIES ( 5 )


// Encode a SteamID into the synthetic address string used everywhere CCCaster expects an
// IpAddrPort. Keeping addr non-empty also makes the base Socket's isClient()/isServer()
// (which key off address.addr.empty()) behave correctly. Port is unused for Steam.
std::string steamAddr ( uint64_t steamId )
{
    return "steam:" + to_string ( steamId );
}

uint64_t steamIdFromAddr ( const IpAddrPort& address )
{
    const std::string& a = address.addr;
    if ( a.compare ( 0, 6, "steam:" ) != 0 )
        return 0;
    return strtoull ( a.c_str() + 6, nullptr, 10 );
}

static SteamSocket *asSteam ( const SocketPtr& ptr )
{
    return static_cast<SteamSocket *> ( ptr.get() );
}


// ---- constructors ----

SteamSocket::SteamSocket ( Socket::Owner *owner, int virtualPort )
    : Socket ( owner, IpAddrPort ( "", 0 ), Protocol::Steam, false )
    , _virtualPort ( virtualPort )
{
    freeBuffer();
    _state = State::Listening;

    _listenHandle = SteamManager::get().createListenSocketP2P ( virtualPort );

    if ( _listenHandle )
        SteamManager::get().registerListen ( _listenHandle, this );
    else
        LOG_STEAM_SOCKET ( this, "createListenSocketP2P failed" );
}

SteamSocket::SteamSocket ( Socket::Owner *owner, uint64_t peerSteamId, int virtualPort )
    : Socket ( owner, IpAddrPort ( steamAddr ( peerSteamId ), 0 ), Protocol::Steam, false )
    , _virtualPort ( virtualPort )
    , _peerSteamId ( peerSteamId )
{
    freeBuffer();
    _state = State::Connecting;

    _conn = SteamManager::get().connectP2P ( peerSteamId, virtualPort );

    if ( _conn )
        SteamManager::get().registerConnection ( _conn, this );
    else
        LOG_STEAM_SOCKET ( this, "connectP2P failed" );
}

SteamSocket::SteamSocket ( ChildSocketEnum, Socket::Owner *owner, uint32_t conn,
                           uint64_t peerSteamId, int virtualPort )
    : Socket ( owner, IpAddrPort ( steamAddr ( peerSteamId ), 0 ), Protocol::Steam, false )
    , _virtualPort ( virtualPort )
    , _conn ( conn )
    , _peerSteamId ( peerSteamId )
{
    freeBuffer();
    _state = State::Connecting;
}

SteamSocket::~SteamSocket()
{
    disconnect();
}

void SteamSocket::disconnect()
{
    SteamManager& sm = SteamManager::get();

    const uint32_t conn = _conn;
    const uint32_t listen = _listenHandle;

    // Detach our children FIRST so their dtors don't try to reach back into us.
    for ( auto& kv : _childSockets )
        asSteam ( kv.second )->_parentSocket = 0;
    _childSockets.clear();
    _acceptedSocket.reset();

    // Remove self from parent's child map. Hold a ref across the erase in case the parent
    // map held the only reference to us (otherwise the rest of this function would run on a
    // freed object). selfKeepAlive lives to the end of disconnect().
    SocketPtr selfKeepAlive;
    if ( _parentSocket )
    {
        const auto it = _parentSocket->_childSockets.find ( conn );
        if ( it != _parentSocket->_childSockets.end() )
        {
            selfKeepAlive = it->second;
            _parentSocket->_childSockets.erase ( it );
        }
        _parentSocket = 0;
    }

    if ( conn )
    {
        sm.unregisterConnection ( conn );
        sm.closeConnection ( conn );
        _conn = 0;
    }

    if ( listen )
    {
        sm.unregisterListen ( listen );
        sm.closeListenSocket ( listen );
        _listenHandle = 0;
    }

    Socket::disconnect();
}


// ---- factories ----

SocketPtr SteamSocket::listen ( Socket::Owner *owner, int virtualPort )
{
    return SocketPtr ( new SteamSocket ( owner, virtualPort ) );
}

SocketPtr SteamSocket::connect ( Socket::Owner *owner, uint64_t peerSteamId, int virtualPort )
{
    return SocketPtr ( new SteamSocket ( owner, peerSteamId, virtualPort ) );
}

SocketPtr SteamSocket::accept ( Socket::Owner *owner )
{
    if ( ! _acceptedSocket )
        return 0;

    _acceptedSocket->owner = owner;

    SocketPtr ret;
    _acceptedSocket.swap ( ret );
    return ret;
}


// ---- send ----

bool SteamSocket::sendEncoded ( const MsgPtr& msg, bool reliable )
{
    if ( _conn == 0 || isDisconnected() )
    {
        LOG_STEAM_SOCKET ( this, "Cannot send over disconnected socket" );
        return false;
    }

    const string buffer = ::Protocol::encode ( msg );

    if ( buffer.empty() )
        return false;

    return SteamManager::get().sendMessage ( _conn, &buffer[0], buffer.size(), reliable );
}

bool SteamSocket::send ( const char *buffer, size_t len )
{
    // Raw byte sends are unused by the netplay path over Steam.
    return false;
}

bool SteamSocket::send ( const char *buffer, size_t len, const IpAddrPort& address )
{
    return false;
}

bool SteamSocket::send ( SerializableMessage *message, const IpAddrPort& address )
{
    // Unreliable, like the UDP data path.
    return sendEncoded ( MsgPtr ( message ), false );
}

bool SteamSocket::send ( SerializableSequence *message, const IpAddrPort& address )
{
    // Reliable + ordered.
    return sendEncoded ( MsgPtr ( message ), true );
}

bool SteamSocket::send ( const MsgPtr& msg, const IpAddrPort& address )
{
    if ( ! msg.get() )
        return false;

    const bool reliable = ( msg->getBaseType().value == BaseType::SerializableSequence );
    return sendEncoded ( msg, reliable );
}


// ---- routing callbacks from SteamManager ----

void SteamSocket::onIncomingConnection ( uint32_t conn, uint64_t peerSteamId )
{
    LOG_STEAM_SOCKET ( this, "incoming conn=%u from peer=%llu", conn, ( unsigned long long ) peerSteamId );

    if ( ! SteamManager::get().acceptConnection ( conn ) )
    {
        LOG_STEAM_SOCKET ( this, "acceptConnection(%u) failed", conn );
        SteamManager::get().closeConnection ( conn );
        return;
    }

    SteamSocket *child = new SteamSocket ( ChildSocket, 0, conn, peerSteamId, _virtualPort );
    child->_parentSocket = this;

    _childSockets[conn] = SocketPtr ( child );
    SteamManager::get().registerConnection ( conn, child );

    // socketAccepted is fired once the child reaches the Connected state (onConnected).
}

void SteamSocket::onConnected()
{
    _state = State::Connected;
    _gotGoodRead = true;

    if ( _parentSocket )
    {
        // Child (host side): surface to the server owner as a newly accepted socket.
        const auto it = _parentSocket->_childSockets.find ( _conn );
        if ( it != _parentSocket->_childSockets.end() )
            _parentSocket->_acceptedSocket = it->second;

        LOG_STEAM_SOCKET ( this, "socketAccepted" );

        if ( _parentSocket->owner )
            _parentSocket->owner->socketAccepted ( _parentSocket );
    }
    else
    {
        // Client: connection established.
        LOG_STEAM_SOCKET ( this, "socketConnected" );

        if ( owner )
            owner->socketConnected ( this );
    }
}

void SteamSocket::onClosed()
{
    LOG_STEAM_SOCKET ( this, "socketDisconnected" );

    // A client connection that dropped before ever reaching Connected may have just raced the SDR
    // relay coming up (or a one-off NAT-punch failure). Re-attempt a bounded number of times
    // before surfacing the disconnect. Server/child sockets, and sockets that DID connect (a real
    // mid-match drop, state == Connected), fall through and disconnect as before.
    if ( _peerSteamId && _parentSocket == 0 && _state == State::Connecting && _conn
         && _connectRetries < STEAM_CONNECT_RETRIES )
    {
        ++_connectRetries;

        SteamManager& sm = SteamManager::get();
        sm.unregisterConnection ( _conn );
        sm.closeConnection ( _conn );
        _conn = sm.connectP2P ( _peerSteamId, _virtualPort );

        if ( _conn )
        {
            sm.registerConnection ( _conn, this );
            LOG_STEAM_SOCKET ( this, "retrying connectP2P (%d)", _connectRetries );
            return;
        }
        // connectP2P failed outright; fall through to a normal disconnect.
    }

    Socket::Owner *const ownerCopy = this->owner;

    disconnect();

    if ( ownerCopy )
        ownerCopy->socketDisconnected ( this );
}

void SteamSocket::onReceive ( const MsgPtr& msg )
{
    _gotGoodRead = true;

    if ( owner )
        owner->socketRead ( this, msg, getRemoteAddress() );
}

#endif // ENABLE_STEAM
