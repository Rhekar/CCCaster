#ifdef ENABLE_STEAM

#include "SteamManager.hpp"
#include "SteamSocket.hpp"
#include "Logger.hpp"

#include "steam/steam_api.h"
// Flat C API for ALL Steam interface calls. On 32-bit MinGW the C++ interface vtable +
// auto-callback (STEAM_CALLBACK/CCallResult) ABI does not match the MSVC-built steam_api.dll
// The flat API is plain extern "C", and we use MANUAL callback dispatch below instead of
// letting the DLL call into our C++ vtables.
// See the steamworks-mingw-flat-api note.
#include "steam/steam_api_flat.h"

#include <windows.h>         // Sleep

#include <vector>
#include <cstdlib>
#include <cstdio>
#include <ctime>

using namespace std;


// How often to pump dispatch + drain messages
#define PUMP_INTERVAL_MS ( 2 )

// Bounded wait for the SDR relay network in waitRelayNetworkReady(): ~5s total (500 * 10ms),
// matching SteamMatchmaking's gate.
#define RELAY_READY_TRIES ( 500 )
#define RELAY_READY_SLEEP_MS ( 10 )

// Max messages drained per connection per pump (leftovers come next pump).
#define RECV_BATCH ( 32 )

// Lobby-level metadata keys.
static const char *LOBBY_CODE_KEY   = "cccaster_code";   // shareable join code
static const char *LOBBY_HOST_KEY   = "host_id";         // lobby owner's SteamID64
static const char *LOBBY_TYPE_KEY   = "cccaster_type";   // "lobby" (browsable) / "private" / "mm"
static const char *LOBBY_REGION_KEY = "cccaster_region"; // matchmaking region
static const char *LOBBY_STATE_KEY  = "cccaster_state";  // matchmaking: "waiting" / "matched"

// Per-member metadata keys.
static const char *MEMBER_NAME_KEY  = "name";            // display name
static const char *MEMBER_CHAL_KEY  = "challenging";     // SteamID64 this member is challenging
static const char *MEMBER_HOST_KEY  = "host_id";         // challenger's own SteamID64 (Host side)

// Route Steam diagnostics into our log file
static void S_CALLTYPE steamNetDebugOutput ( ESteamNetworkingSocketsDebugOutputType type, const char *msg )
{
    LOG ( "[SteamNet] %s", msg ? msg : "" );
}

static void S_CALLTYPE steamWarningHook ( int severity, const char *msg )
{
    LOG ( "[SteamWarn:%d] %s", severity, msg ? msg : "" );
}

// Human-shareable join code
static string makeLobbyCode()
{
    static bool seeded = false;
    if ( ! seeded )
    {
        srand ( ( unsigned ) time ( nullptr ) );
        seeded = true;
    }
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    string s;
    for ( int i = 0; i < 6; ++i )
        s += alphabet[rand() % ( sizeof ( alphabet ) - 1 )];
    return s;
}


SteamManager& SteamManager::get()
{
    static SteamManager instance;
    return instance;
}

bool SteamManager::ref()
{
    if ( _refCount > 0 )
    {
        ++_refCount;
        return _inited;
    }

    if ( ! SteamAPI_Init() )
    {
        LOG ( "SteamAPI_Init() failed (Steam not running, app not owned, or no steam_appid.txt)" );
        return false;
    }

    _inited = true;
    _refCount = 1;

    // poll callbacks from POD structs in pump() rather than
    // having steam_api.dll call back into us
    SteamAPI_ManualDispatch_Init();
    _pipe = SteamAPI_GetHSteamPipe();

    // Redirect Steam's stdout into our log
    SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction (
        SteamNetworkingUtils(), k_ESteamNetworkingSocketsDebugOutputType_Warning, steamNetDebugOutput );
    SteamAPI_ISteamUtils_SetWarningMessageHook ( SteamUtils(), steamWarningHook );

    // Disable ECN for extra Wine compatibility
    SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32 (
        SteamNetworkingUtils(), k_ESteamNetworkingConfig_ECN, 0 );

    SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess ( SteamNetworkingUtils() );

    LOG ( "Steam initialized; user=%llu", ( unsigned long long ) getSteamID() );

    startPump();
    return true;
}

void SteamManager::deref()
{
    if ( _refCount <= 0 )
        return;

    --_refCount;

    if ( _refCount > 0 )
        return;

    stopPump();

    SteamAPI_Shutdown();
    _inited = false;
    _pipe = 0;
    _lobbyCreateCall = _lobbyListCall = _lobbyJoinCall = _browseListCall = 0;
    _mmListCall = _mmCreateCall = _mmJoinCall = 0;

    LOG ( "Steam shut down" );
}

uint64_t SteamManager::getSteamID() const
{
    if ( ! _inited )
        return 0;

    return SteamAPI_ISteamUser_GetSteamID ( SteamUser() );
}

std::string SteamManager::getPersonaName() const
{
    if ( ! _inited )
        return "";

    const char *n = SteamAPI_ISteamFriends_GetPersonaName ( SteamFriends() );
    return n ? n : "";
}

bool SteamManager::isRelayNetworkReady() const
{
    if ( ! _inited )
        return false;

    return ( SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus ( SteamNetworkingUtils(), nullptr )
             == k_ESteamNetworkingAvailability_Current );
}

bool SteamManager::waitRelayNetworkReady()
{
    // Mirror the matchmaking gate (SteamMatchmaking::waitConnected): ~5s of manual pumping, then
    // proceed regardless. After a fresh ref() the relay config is often cached and this returns
    // almost immediately; only a cold fetch takes a moment.
    for ( int i = 0; i < RELAY_READY_TRIES && ! isRelayNetworkReady(); ++i )
    {
        pump();
        Sleep ( RELAY_READY_SLEEP_MS );
    }

    return isRelayNetworkReady();
}


// ---- transport wrappers ----

uint32_t SteamManager::createListenSocketP2P ( int virtualPort )
{
    if ( ! _inited )
        return 0;

    return SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P (
               SteamNetworkingSockets(), virtualPort, 0, nullptr );
}

void SteamManager::closeListenSocket ( uint32_t listenHandle )
{
    if ( _inited && listenHandle )
        SteamAPI_ISteamNetworkingSockets_CloseListenSocket ( SteamNetworkingSockets(), listenHandle );
}

uint32_t SteamManager::connectP2P ( uint64_t peerSteamId, int virtualPort )
{
    if ( ! _inited )
        return 0;

    SteamNetworkingIdentity identity;
    identity.SetSteamID64 ( peerSteamId ); // inline, no DLL call

    return SteamAPI_ISteamNetworkingSockets_ConnectP2P (
               SteamNetworkingSockets(), identity, virtualPort, 0, nullptr );
}

bool SteamManager::acceptConnection ( uint32_t conn )
{
    if ( ! _inited || ! conn )
        return false;

    return ( SteamAPI_ISteamNetworkingSockets_AcceptConnection ( SteamNetworkingSockets(), conn )
             == k_EResultOK );
}

void SteamManager::closeConnection ( uint32_t conn )
{
    if ( _inited && conn )
        SteamAPI_ISteamNetworkingSockets_CloseConnection ( SteamNetworkingSockets(), conn, 0, nullptr, false );
}

bool SteamManager::sendMessage ( uint32_t conn, const void *data, size_t len, bool reliable )
{
    if ( ! _inited || ! conn )
        return false;

    const int flags = ( reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable );

    const EResult r = SteamAPI_ISteamNetworkingSockets_SendMessageToConnection (
                          SteamNetworkingSockets(), conn, data, ( uint32 ) len, flags, nullptr );

    return ( r == k_EResultOK );
}

uint64_t SteamManager::getConnectionPeer ( uint32_t conn ) const
{
    if ( ! _inited || ! conn )
        return 0;

    SteamNetConnectionInfo_t info;
    if ( ! SteamAPI_ISteamNetworkingSockets_GetConnectionInfo ( SteamNetworkingSockets(), conn, &info ) )
        return 0;

    return info.m_identityRemote.GetSteamID64(); // inline accessor
}


// ---- registries ----

void SteamManager::registerConnection ( uint32_t conn, SteamSocket *socket )
{
    if ( conn )
        _connections[conn] = socket;
}

void SteamManager::unregisterConnection ( uint32_t conn )
{
    _connections.erase ( conn );
}

void SteamManager::registerListen ( uint32_t listenHandle, SteamSocket *server )
{
    if ( listenHandle )
        _listeners[listenHandle] = server;
}

void SteamManager::unregisterListen ( uint32_t listenHandle )
{
    _listeners.erase ( listenHandle );
}


// ---- lobby ----

void SteamManager::tagLobby ( uint64_t lobbyId, const char *type )
{
    char selfId[32];
    snprintf ( selfId, sizeof ( selfId ), "%llu", ( unsigned long long ) getSteamID() );

    SteamAPI_ISteamMatchmaking_SetLobbyData ( SteamMatchmaking(), lobbyId, LOBBY_TYPE_KEY, type );
    SteamAPI_ISteamMatchmaking_SetLobbyData ( SteamMatchmaking(), lobbyId, LOBBY_CODE_KEY, _lobbyCode.c_str() );
    SteamAPI_ISteamMatchmaking_SetLobbyData ( SteamMatchmaking(), lobbyId, LOBBY_HOST_KEY, selfId );

    if ( ! _memberName.empty() )
        SteamAPI_ISteamMatchmaking_SetLobbyMemberData ( SteamMatchmaking(), lobbyId, MEMBER_NAME_KEY,
                                                        _memberName.c_str() );
}

void SteamManager::createLobby ( bool isPublic )
{
    if ( ! _inited )
    {
        _lobbyState = LobbyState::Failed;
        return;
    }

    _lobbyIsHost = true;
    _lobbyIsPublic = isPublic;
    _lobbyState = LobbyState::Working;
    _lobbyCode.clear();
    _lobbyId = 0;
    _myChallenge = 0;

    // Both public and "private" lobbies are Steam-level public so that exact-code RequestLobbyList
    // can find them; privacy is enforced by the cccaster_type tag (browse only asks for "lobby").
    _lobbyCreateCall = SteamAPI_ISteamMatchmaking_CreateLobby ( SteamMatchmaking(), k_ELobbyTypePublic, 8 );
}

void SteamManager::onLobbyCreatedResult ( bool ok, uint64_t lobbyId )
{
    if ( ! ok )
    {
        LOG ( "CreateLobby failed" );
        _lobbyState = LobbyState::Failed;
        return;
    }

    _lobbyId = lobbyId;

    if ( _lobbyCode.empty() )
        _lobbyCode = makeLobbyCode();

    tagLobby ( lobbyId, _lobbyIsPublic ? "lobby" : "private" );

    _lobbyState = LobbyState::Ready;
    LOG ( "Lobby ready; code=%s public=%d", _lobbyCode.c_str(), ( int ) _lobbyIsPublic );
}

void SteamManager::joinLobbyByCode ( const std::string& code )
{
    if ( ! _inited )
    {
        _lobbyState = LobbyState::Failed;
        return;
    }

    _lobbyIsHost = false;
    _joinCode = code;
    _lobbyPeerId = 0;
    _lobbyId = 0;
    _myChallenge = 0;
    _lobbyState = LobbyState::Working;

    SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter (
        SteamMatchmaking(), LOBBY_CODE_KEY, code.c_str(), k_ELobbyComparisonEqual );
    SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter (
        SteamMatchmaking(), k_ELobbyDistanceFilterWorldwide );

    _lobbyListCall = SteamAPI_ISteamMatchmaking_RequestLobbyList ( SteamMatchmaking() );
}

void SteamManager::onLobbyListResult ( bool found, uint64_t lobbyId )
{
    if ( ! found )
    {
        LOG ( "No lobby found for code '%s'", _joinCode.c_str() );
        _lobbyState = LobbyState::Failed;
        return;
    }

    // Found the lobby advertising the code; join it for membership (the lobby UI lists members
    // and issues challenges; it does not connect P2P directly here).
    joinLobbyById ( lobbyId );
}

void SteamManager::joinLobbyById ( uint64_t lobbyId )
{
    if ( ! _inited || ! lobbyId )
    {
        _lobbyState = LobbyState::Failed;
        return;
    }

    _lobbyIsHost = false;
    _myChallenge = 0;
    _lobbyState = LobbyState::Working;

    _lobbyJoinCall = SteamAPI_ISteamMatchmaking_JoinLobby ( SteamMatchmaking(), lobbyId );
}

void SteamManager::onLobbyEntered ( uint64_t lobbyId, bool ok )
{
    if ( ! ok )
    {
        LOG ( "JoinLobby failed" );
        _lobbyState = LobbyState::Failed;
        return;
    }

    _lobbyId = lobbyId;

    if ( ! _memberName.empty() )
        SteamAPI_ISteamMatchmaking_SetLobbyMemberData ( SteamMatchmaking(), lobbyId, MEMBER_NAME_KEY,
                                                        _memberName.c_str() );

    _lobbyState = LobbyState::Ready;
    LOG ( "Entered lobby %llu", ( unsigned long long ) lobbyId );
}

void SteamManager::requestPublicLobbies()
{
    if ( ! _inited )
    {
        _browseState = LobbyState::Failed;
        return;
    }

    _browseState = LobbyState::Working;
    _publicLobbies.clear();

    SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter (
        SteamMatchmaking(), LOBBY_TYPE_KEY, "lobby", k_ELobbyComparisonEqual );
    SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter (
        SteamMatchmaking(), k_ELobbyDistanceFilterWorldwide );

    _browseListCall = SteamAPI_ISteamMatchmaking_RequestLobbyList ( SteamMatchmaking() );
}

void SteamManager::onBrowseListResult ( int count )
{
    _publicLobbies.clear();

    for ( int i = 0; i < count; ++i )
    {
        const uint64_t id = SteamAPI_ISteamMatchmaking_GetLobbyByIndex ( SteamMatchmaking(), i );
        const char *code = SteamAPI_ISteamMatchmaking_GetLobbyData ( SteamMatchmaking(), id, LOBBY_CODE_KEY );
        const int cnt = SteamAPI_ISteamMatchmaking_GetNumLobbyMembers ( SteamMatchmaking(), id );
        _publicLobbies.push_back ( { id, code ? code : "", cnt } );
    }

    _browseState = LobbyState::Ready;
    LOG ( "Browse: %d public lobbies", ( int ) _publicLobbies.size() );
}

uint64_t SteamManager::lobbyOwnerId() const
{
    if ( ! _inited || ! _lobbyId )
        return 0;

    return SteamAPI_ISteamMatchmaking_GetLobbyOwner ( SteamMatchmaking(), _lobbyId );
}

std::vector<SteamManager::MemberInfo> SteamManager::lobbyMembers()
{
    std::vector<MemberInfo> out;

    if ( ! _inited || ! _lobbyId )
        return out;

    const int n = SteamAPI_ISteamMatchmaking_GetNumLobbyMembers ( SteamMatchmaking(), _lobbyId );

    for ( int i = 0; i < n; ++i )
    {
        const uint64_t mid = SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex ( SteamMatchmaking(), _lobbyId, i );

        const char *nm  = SteamAPI_ISteamMatchmaking_GetLobbyMemberData ( SteamMatchmaking(), _lobbyId, mid, MEMBER_NAME_KEY );
        const char *chl = SteamAPI_ISteamMatchmaking_GetLobbyMemberData ( SteamMatchmaking(), _lobbyId, mid, MEMBER_CHAL_KEY );
        const char *hid = SteamAPI_ISteamMatchmaking_GetLobbyMemberData ( SteamMatchmaking(), _lobbyId, mid, MEMBER_HOST_KEY );

        std::string name = ( nm && nm[0] ) ? nm : "";
        if ( name.empty() )
        {
            const char *pn = SteamAPI_ISteamFriends_GetFriendPersonaName ( SteamFriends(), mid );
            name = ( pn ? pn : "" );
        }

        const uint64_t chalTarget = ( chl && chl[0] ) ? strtoull ( chl, nullptr, 10 ) : 0;
        const uint64_t hostId     = ( hid && hid[0] ) ? strtoull ( hid, nullptr, 10 ) : 0;

        out.push_back ( { mid, name, chalTarget, hostId } );
    }

    return out;
}

void SteamManager::setLobbyMemberName ( const std::string& name )
{
    _memberName = name;

    if ( _inited && _lobbyId )
        SteamAPI_ISteamMatchmaking_SetLobbyMemberData ( SteamMatchmaking(), _lobbyId, MEMBER_NAME_KEY, name.c_str() );
}

void SteamManager::setLobbyData ( const std::string& key, const std::string& value )
{
    // No-op for non-owners: Steam ignores SetLobbyData unless we own the lobby.
    if ( _inited && _lobbyId )
        SteamAPI_ISteamMatchmaking_SetLobbyData ( SteamMatchmaking(), _lobbyId, key.c_str(), value.c_str() );
}

std::string SteamManager::getLobbyData ( const std::string& key )
{
    if ( ! _inited || ! _lobbyId )
        return "";

    const char *v = SteamAPI_ISteamMatchmaking_GetLobbyData ( SteamMatchmaking(), _lobbyId, key.c_str() );
    return v ? v : "";
}

void SteamManager::setMyMemberData ( const std::string& key, const std::string& value )
{
    if ( _inited && _lobbyId )
        SteamAPI_ISteamMatchmaking_SetLobbyMemberData ( SteamMatchmaking(), _lobbyId, key.c_str(), value.c_str() );
}

std::string SteamManager::getMemberData ( uint64_t memberId, const std::string& key )
{
    if ( ! _inited || ! _lobbyId )
        return "";

    const char *v = SteamAPI_ISteamMatchmaking_GetLobbyMemberData ( SteamMatchmaking(), _lobbyId, memberId, key.c_str() );
    return v ? v : "";
}

void SteamManager::setChallenge ( uint64_t targetId )
{
    if ( ! _inited || ! _lobbyId )
        return;

    _myChallenge = targetId;

    char target[32], self[32];
    snprintf ( target, sizeof ( target ), "%llu", ( unsigned long long ) targetId );
    snprintf ( self, sizeof ( self ), "%llu", ( unsigned long long ) getSteamID() );

    SteamAPI_ISteamMatchmaking_SetLobbyMemberData ( SteamMatchmaking(), _lobbyId, MEMBER_CHAL_KEY, target );
    SteamAPI_ISteamMatchmaking_SetLobbyMemberData ( SteamMatchmaking(), _lobbyId, MEMBER_HOST_KEY, self );
}

void SteamManager::clearChallenge()
{
    _myChallenge = 0;

    if ( ! _inited || ! _lobbyId )
        return;

    SteamAPI_ISteamMatchmaking_SetLobbyMemberData ( SteamMatchmaking(), _lobbyId, MEMBER_CHAL_KEY, "" );
    SteamAPI_ISteamMatchmaking_SetLobbyMemberData ( SteamMatchmaking(), _lobbyId, MEMBER_HOST_KEY, "" );
}

void SteamManager::leaveLobby()
{
    if ( _inited && _lobbyId )
        SteamAPI_ISteamMatchmaking_LeaveLobby ( SteamMatchmaking(), _lobbyId );

    _lobbyId = 0;
    _lobbyPeerId = 0;
    _lobbyCode.clear();
    _lobbyState = LobbyState::Idle;
    _browseState = LobbyState::Idle;
    _publicLobbies.clear();
    _myChallenge = 0;
}


// ---- region matchmaking (create-or-join) ----

void SteamManager::matchmakeRegion ( const std::string& region )
{
    if ( ! _inited )
    {
        _mmState = MMState::Failed;
        return;
    }

    _mmRegion = region;
    _mmPeerId = 0;
    _mmState = MMState::Working;
    _lobbyId = 0;

    SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter (
        SteamMatchmaking(), LOBBY_TYPE_KEY, "mm", k_ELobbyComparisonEqual );
    SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter (
        SteamMatchmaking(), LOBBY_REGION_KEY, region.c_str(), k_ELobbyComparisonEqual );
    SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter (
        SteamMatchmaking(), LOBBY_STATE_KEY, "waiting", k_ELobbyComparisonEqual );
    SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter ( SteamMatchmaking(), 1 );

    _mmListCall = SteamAPI_ISteamMatchmaking_RequestLobbyList ( SteamMatchmaking() );
}

void SteamManager::onMatchmakeListResult ( int count )
{
    if ( count > 0 )
    {
        // A peer is already waiting in this region; join as Client.
        const uint64_t id = SteamAPI_ISteamMatchmaking_GetLobbyByIndex ( SteamMatchmaking(), 0 );
        _mmJoinCall = SteamAPI_ISteamMatchmaking_JoinLobby ( SteamMatchmaking(), id );
    }
    else
    {
        // Nobody waiting; create a waiting lobby and become Host.
        _mmCreateCall = SteamAPI_ISteamMatchmaking_CreateLobby ( SteamMatchmaking(), k_ELobbyTypePublic, 2 );
    }
}

void SteamManager::onMatchmakeCreated ( bool ok, uint64_t lobbyId )
{
    if ( ! ok )
    {
        _mmState = MMState::Failed;
        return;
    }

    _lobbyId = lobbyId;

    char selfId[32];
    snprintf ( selfId, sizeof ( selfId ), "%llu", ( unsigned long long ) getSteamID() );

    SteamAPI_ISteamMatchmaking_SetLobbyData ( SteamMatchmaking(), lobbyId, LOBBY_TYPE_KEY, "mm" );
    SteamAPI_ISteamMatchmaking_SetLobbyData ( SteamMatchmaking(), lobbyId, LOBBY_REGION_KEY, _mmRegion.c_str() );
    SteamAPI_ISteamMatchmaking_SetLobbyData ( SteamMatchmaking(), lobbyId, LOBBY_STATE_KEY, "waiting" );
    SteamAPI_ISteamMatchmaking_SetLobbyData ( SteamMatchmaking(), lobbyId, LOBBY_HOST_KEY, selfId );

    // Stay Working; a joining peer triggers onMemberJoined -> BecameHost.
    LOG ( "Matchmaking: created waiting lobby in region %s", _mmRegion.c_str() );
}

void SteamManager::onMatchmakeEntered ( uint64_t lobbyId, bool ok )
{
    if ( ! ok )
    {
        _mmState = MMState::Failed;
        return;
    }

    _lobbyId = lobbyId;
    _mmPeerId = SteamAPI_ISteamMatchmaking_GetLobbyOwner ( SteamMatchmaking(), lobbyId );

    if ( _mmPeerId == 0 )
    {
        _mmState = MMState::Failed;
        return;
    }

    _mmState = MMState::BecameClient;
    LOG ( "Matchmaking: joined region %s; host=%llu", _mmRegion.c_str(), ( unsigned long long ) _mmPeerId );
}

void SteamManager::onMemberJoined ( uint64_t lobbyId, uint64_t memberId )
{
    if ( lobbyId != _lobbyId )
        return;

    // Matchmaking host: a peer joined our waiting lobby -> we are matched as Host.
    if ( _mmState == MMState::Working && memberId != 0 && memberId != getSteamID() )
    {
        _mmPeerId = memberId;
        SteamAPI_ISteamMatchmaking_SetLobbyData ( SteamMatchmaking(), lobbyId, LOBBY_STATE_KEY, "matched" );
        _mmState = MMState::BecameHost;
        LOG ( "Matchmaking: peer %llu joined; we host", ( unsigned long long ) memberId );
    }
}

void SteamManager::cancelMatchmaking()
{
    if ( _inited && _lobbyId )
        SteamAPI_ISteamMatchmaking_LeaveLobby ( SteamMatchmaking(), _lobbyId );

    _lobbyId = 0;
    _mmState = MMState::Idle;
    _mmPeerId = 0;
}


// ---- callback routing ----

void SteamManager::onConnectionStatusChanged ( uint32_t conn, uint32_t listenHandle,
                                               int newState, uint64_t peerSteamId )
{
    switch ( newState )
    {
        case k_ESteamNetworkingConnectionState_Connecting:
        {
            if ( listenHandle )
            {
                const auto it = _listeners.find ( listenHandle );
                if ( it != _listeners.end() )
                    it->second->onIncomingConnection ( conn, peerSteamId );
                else
                    closeConnection ( conn );
            }
            break;
        }

        case k_ESteamNetworkingConnectionState_Connected:
        {
            const auto it = _connections.find ( conn );
            if ( it != _connections.end() )
                it->second->onConnected();
            break;
        }

        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        {
            const auto it = _connections.find ( conn );
            if ( it != _connections.end() )
                it->second->onClosed();
            else
                closeConnection ( conn );
            break;
        }

        default:
            break;
    }
}


// ---- pump (manual dispatch) ----

void SteamManager::startPump()
{
    if ( ! _pumpTimer )
        _pumpTimer.reset ( new Timer ( this ) );
    _pumpTimer->start ( PUMP_INTERVAL_MS );
}

void SteamManager::stopPump()
{
    _pumpTimer.reset();
}

void SteamManager::timerExpired ( Timer *timer )
{
    if ( timer != _pumpTimer.get() )
        return;

    pump();

    if ( _pumpTimer )
        _pumpTimer->start ( PUMP_INTERVAL_MS );
}

void SteamManager::pump()
{
    if ( ! _inited )
        return;

    // --- manual callback dispatch ---
    SteamAPI_ManualDispatch_RunFrame ( _pipe );

    CallbackMsg_t cb;
    while ( SteamAPI_ManualDispatch_GetNextCallback ( _pipe, &cb ) )
    {
        if ( cb.m_iCallback == SteamAPICallCompleted_t::k_iCallback )
        {
            // Async call result (CreateLobby / RequestLobbyList / JoinLobby), matched by call id.
            const SteamAPICallCompleted_t *cc = ( const SteamAPICallCompleted_t * ) cb.m_pubParam;
            void *result = malloc ( cc->m_cubParam );
            bool failed = false;

            if ( result && SteamAPI_ManualDispatch_GetAPICallResult (
                     _pipe, cc->m_hAsyncCall, result, cc->m_cubParam, cc->m_iCallback, &failed ) )
            {
                const uint64_t call = cc->m_hAsyncCall;

                if ( _lobbyCreateCall && call == _lobbyCreateCall )
                {
                    const LobbyCreated_t *r = ( const LobbyCreated_t * ) result;
                    _lobbyCreateCall = 0;
                    onLobbyCreatedResult ( ( ! failed && r->m_eResult == k_EResultOK ), r->m_ulSteamIDLobby );
                }
                else if ( _lobbyListCall && call == _lobbyListCall )
                {
                    const LobbyMatchList_t *r = ( const LobbyMatchList_t * ) result;
                    _lobbyListCall = 0;
                    const bool found = ( ! failed && r->m_nLobbiesMatching > 0 );
                    const uint64_t id =
                        found ? SteamAPI_ISteamMatchmaking_GetLobbyByIndex ( SteamMatchmaking(), 0 ) : 0;
                    onLobbyListResult ( found, id );
                }
                else if ( _browseListCall && call == _browseListCall )
                {
                    const LobbyMatchList_t *r = ( const LobbyMatchList_t * ) result;
                    _browseListCall = 0;
                    onBrowseListResult ( failed ? 0 : ( int ) r->m_nLobbiesMatching );
                }
                else if ( _lobbyJoinCall && call == _lobbyJoinCall )
                {
                    const LobbyEnter_t *r = ( const LobbyEnter_t * ) result;
                    _lobbyJoinCall = 0;
                    onLobbyEntered ( r->m_ulSteamIDLobby,
                                     ( ! failed && r->m_EChatRoomEnterResponse == k_EChatRoomEnterResponseSuccess ) );
                }
                else if ( _mmListCall && call == _mmListCall )
                {
                    const LobbyMatchList_t *r = ( const LobbyMatchList_t * ) result;
                    _mmListCall = 0;
                    onMatchmakeListResult ( failed ? 0 : ( int ) r->m_nLobbiesMatching );
                }
                else if ( _mmCreateCall && call == _mmCreateCall )
                {
                    const LobbyCreated_t *r = ( const LobbyCreated_t * ) result;
                    _mmCreateCall = 0;
                    onMatchmakeCreated ( ( ! failed && r->m_eResult == k_EResultOK ), r->m_ulSteamIDLobby );
                }
                else if ( _mmJoinCall && call == _mmJoinCall )
                {
                    const LobbyEnter_t *r = ( const LobbyEnter_t * ) result;
                    _mmJoinCall = 0;
                    onMatchmakeEntered ( r->m_ulSteamIDLobby,
                                         ( ! failed && r->m_EChatRoomEnterResponse == k_EChatRoomEnterResponseSuccess ) );
                }
            }

            free ( result );
        }
        else if ( cb.m_iCallback == SteamNetConnectionStatusChangedCallback_t::k_iCallback )
        {
            const SteamNetConnectionStatusChangedCallback_t *p =
                ( const SteamNetConnectionStatusChangedCallback_t * ) cb.m_pubParam;
            onConnectionStatusChanged ( p->m_hConn, p->m_info.m_hListenSocket,
                                        ( int ) p->m_info.m_eState,
                                        p->m_info.m_identityRemote.GetSteamID64() );
        }
        else if ( cb.m_iCallback == LobbyChatUpdate_t::k_iCallback )
        {
            // Member entered/left the lobby. Used to detect a peer joining a matchmaking lobby
            // (host side); the lobby member list re-reads on the UI's periodic refresh anyway.
            const LobbyChatUpdate_t *p = ( const LobbyChatUpdate_t * ) cb.m_pubParam;
            if ( p->m_rgfChatMemberStateChange & k_EChatMemberStateChangeEntered )
                onMemberJoined ( p->m_ulSteamIDLobby, p->m_ulSteamIDUserChanged );
        }
        // LobbyDataUpdate_t / LobbyChatMsg_t: Steam keeps the member-data cache current
        // automatically, so the periodic lobbyMembers() refresh sees the latest values; no
        // explicit handling needed.

        SteamAPI_ManualDispatch_FreeLastCallback ( _pipe );
    }

    // --- drain inbound messages ---
    vector<uint32_t> conns;
    conns.reserve ( _connections.size() );
    for ( const auto& kv : _connections )
        conns.push_back ( kv.first );

    for ( uint32_t conn : conns )
    {
        if ( _connections.find ( conn ) == _connections.end() )
            continue;

        SteamNetworkingMessage_t *msgs[RECV_BATCH];
        const int n = SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection (
                          SteamNetworkingSockets(), conn, msgs, RECV_BATCH );

        vector<MsgPtr> decoded;
        decoded.reserve ( n > 0 ? n : 0 );
        for ( int i = 0; i < n; ++i )
        {
            size_t consumed = 0;
            MsgPtr msg = ::Protocol::decode ( ( const char * ) msgs[i]->m_pData, msgs[i]->m_cbSize, consumed );
            msgs[i]->Release(); // frees via the message's own fn ptr
            if ( msg.get() )
                decoded.push_back ( msg );
        }

        for ( const MsgPtr& msg : decoded )
        {
            const auto it = _connections.find ( conn );
            if ( it == _connections.end() )
                break;
            it->second->onReceive ( msg );
        }
    }
}

#endif // ENABLE_STEAM
