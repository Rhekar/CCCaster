#pragma once

#ifdef ENABLE_STEAM

#include "Timer.hpp"
#include "Protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>


class SteamSocket;


// Owns the Steamworks API lifecycle and is the ONLY translation unit that includes the
// Steam SDK headers. Everything Steam-related is wrapped behind plain-typed functions
// (uint32_t connection/listen handles, uint64_t SteamIDs) so the rest of CCCaster never
// pulls in Steam headers. Calls that pass/return CSteamID use the flat C API to avoid the
// MinGW<->MSVC ABI mismatch (see the steamworks-mingw-flat-api note).
//
// A self-driven repeating Timer pumps Steam's manual callback dispatch + drains incoming
// messages, so SteamSocket does not need to register with SocketManager (which select()s on an fd).
class SteamManager : private Timer::Owner
{
public:

    static SteamManager& get();

    // Ref-counted SteamAPI init. ref() returns false if Steam is unavailable (not running,
    // app 411370 not owned, or no steam_appid.txt). Safe to call repeatedly.
    bool ref();
    void deref();

    bool isInitialized() const { return _inited; }

    // Local user's SteamID64 (0 if not initialized).
    uint64_t getSteamID() const;

    // Local user's Steam persona (display) name; "" if not initialized.
    std::string getPersonaName() const;

    // Whether the SDR relay network is ready (ping data current). ConnectP2P before this is
    // ready tends to fail, so callers should gate on it.
    bool isRelayNetworkReady() const;

    // Pump until the SDR relay network is ready (so a subsequent ConnectP2P / listen succeeds),
    // or until a bounded timeout (~5s) elapses. Cheap no-op when already ready. Returns the final
    // readiness. Safe to call directly in a wait loop (no EventManager needed). Do NOT call from
    // inside a Steam callback (e.g. SteamSocket::onClosed) - it pumps, which would be re-entrant.
    bool waitRelayNetworkReady();

    // ---- transport wrappers (all handles are plain ints; 0 == invalid) ----

    // Host: open a P2P listen socket on a virtual port. Returns the listen handle.
    uint32_t createListenSocketP2P ( int virtualPort );
    void closeListenSocket ( uint32_t listenHandle );

    // Client: begin a P2P connection to a peer SteamID on a virtual port. Returns conn handle.
    uint32_t connectP2P ( uint64_t peerSteamId, int virtualPort );

    bool acceptConnection ( uint32_t conn );
    void closeConnection ( uint32_t conn );

    // Send already-encoded bytes over a connection. reliable picks the Steam send flag.
    bool sendMessage ( uint32_t conn, const void *data, size_t len, bool reliable );

    // Peer SteamID for an established connection (0 if unknown).
    uint64_t getConnectionPeer ( uint32_t conn ) const;

    // ---- lobby / matchmaking (driven by the launcher UI) ----
    // Asynchronous; poll the relevant *State() until Ready/Failed. SteamManager's pump runs the
    // underlying Steam callbacks, so the caller just needs to keep pumping the event loop.
    enum class LobbyState { Idle, Working, Ready, Failed };

    // Create a lobby and advertise a short join code + our SteamID. isPublic lobbies are returned
    // by requestPublicLobbies(); non-public ("private") lobbies are excluded from browse but are
    // still joinable by exact code. On Ready, lobbyCode() returns the code to share.
    void createLobby ( bool isPublic );

    // Client: find the lobby advertising the given code and JOIN it (membership). On Ready,
    // joinedLobbyId() is valid and lobbyMembers() lists the occupants.
    void joinLobbyByCode ( const std::string& code );

    // Join a specific lobby by id (e.g. one picked from the public browse list).
    void joinLobbyById ( uint64_t lobbyId );

    void leaveLobby();

    LobbyState lobbyState() const { return _lobbyState; }
    const std::string& lobbyCode() const { return _lobbyCode; }
    uint64_t lobbyPeerId() const { return _lobbyPeerId; }
    uint64_t joinedLobbyId() const { return _lobbyId; }
    uint64_t lobbyOwnerId() const;

    // ---- public lobby browse ----
    struct LobbyInfo { uint64_t lobbyId; std::string code; int memberCount; };

    void requestPublicLobbies();
    LobbyState browseState() const { return _browseState; }
    const std::vector<LobbyInfo>& publicLobbies() const { return _publicLobbies; }

    // ---- members of the joined lobby ----
    struct MemberInfo { uint64_t steamId; std::string name; uint64_t challengingTarget; uint64_t hostId; };

    // Re-enumerate the current lobby's members (cheap; reads Steam's cached member data).
    std::vector<MemberInfo> lobbyMembers();

    // Advertise our display name to the lobby (so peers can list us by name).
    void setLobbyMemberName ( const std::string& name );

    // ---- generic lobby / member data (used by the king-of-the-hill queue) ----
    // Lobby data is single-writer: Steam silently ignores SetLobbyData from non-owners, so the
    // lobby owner is the queue's authoritative coordinator. Member data is self-writable only.
    void setLobbyData ( const std::string& key, const std::string& value );
    std::string getLobbyData ( const std::string& key );
    void setMyMemberData ( const std::string& key, const std::string& value );
    std::string getMemberData ( uint64_t memberId, const std::string& key );

    // ---- challenge handshake (member-data based, no relay) ----
    // Challenger advertises {challenging:<target>, host_id:<self>}; the target sees it on its next
    // member refresh and connects as Client to our SteamID. clearChallenge() withdraws it.
    void setChallenge ( uint64_t targetId );
    void clearChallenge();
    uint64_t myChallengeTarget() const { return _myChallenge; }

    // ---- region matchmaking (create-or-join) ----
    enum class MMState { Idle, Working, BecameHost, BecameClient, Failed };

    void matchmakeRegion ( const std::string& region );
    void cancelMatchmaking();
    MMState mmState() const { return _mmState; }
    uint64_t mmPeerId() const { return _mmPeerId; }

    // Run one manual-dispatch + message-drain cycle. Normally driven by the internal pump
    // timer once the EventManager loop is running, but callers (e.g. the lobby UI) can call
    // it directly in a wait loop when no EventManager loop is pumping yet.
    void pump();

    // ---- routing registries (used by SteamSocket) ----

    void registerConnection ( uint32_t conn, SteamSocket *socket );
    void unregisterConnection ( uint32_t conn );

    void registerListen ( uint32_t listenHandle, SteamSocket *server );
    void unregisterListen ( uint32_t listenHandle );

    // Called by the internal Steam callback shim (defined in the .cpp).
    void onConnectionStatusChanged ( uint32_t conn, uint32_t listenHandle, int newState, uint64_t peerSteamId );

private:

    SteamManager() {}

    int _refCount = 0;
    bool _inited = false;

    // Steam pipe for manual callback dispatch (HSteamPipe; 0 == none).
    int _pipe = 0;

    // Pending async call-result ids (SteamAPICall_t) matched in the manual-dispatch pump.
    uint64_t _lobbyCreateCall = 0;   // createLobby()
    uint64_t _lobbyListCall = 0;     // joinLobbyByCode() code search
    uint64_t _lobbyJoinCall = 0;     // JoinLobby() for the lobby path
    uint64_t _browseListCall = 0;    // requestPublicLobbies()
    uint64_t _mmListCall = 0;        // matchmakeRegion() search
    uint64_t _mmCreateCall = 0;      // matchmakeRegion() create (host)
    uint64_t _mmJoinCall = 0;        // matchmakeRegion() join (client)

    TimerPtr _pumpTimer;

    // Lobby state
    LobbyState _lobbyState = LobbyState::Idle;
    std::string _lobbyCode;     // host's advertised code (valid when host + Ready)
    std::string _joinCode;      // code the client is searching for
    uint64_t _lobbyId = 0;      // current lobby (host or joined)
    uint64_t _lobbyPeerId = 0;  // resolved host SteamID (legacy 1v1; unused by the lobby path)
    bool _lobbyIsHost = false;
    bool _lobbyIsPublic = true;
    std::string _memberName;    // our display name, advertised on create/join
    uint64_t _myChallenge = 0;  // who we are currently challenging (0 == none)

    // Browse state
    LobbyState _browseState = LobbyState::Idle;
    std::vector<LobbyInfo> _publicLobbies;

    // Matchmaking state
    MMState _mmState = MMState::Idle;
    std::string _mmRegion;
    uint64_t _mmPeerId = 0;

    // conn handle -> the SteamSocket that owns it (client or accepted child)
    std::unordered_map<uint32_t, SteamSocket *> _connections;

    // listen handle -> the server SteamSocket that owns it
    std::unordered_map<uint32_t, SteamSocket *> _listeners;

    // --- internal callback handlers (called from pump's manual dispatch) ---
    void onLobbyCreatedResult ( bool ok, uint64_t lobbyId );
    void onLobbyListResult ( bool found, uint64_t lobbyId );
    void onBrowseListResult ( int count );
    void onLobbyEntered ( uint64_t lobbyId, bool ok );
    void onMatchmakeListResult ( int count );
    void onMatchmakeCreated ( bool ok, uint64_t lobbyId );
    void onMatchmakeEntered ( uint64_t lobbyId, bool ok );
    void onMemberJoined ( uint64_t lobbyId, uint64_t memberId );

    // Apply the lobby-data tags + our member name for a lobby we just created.
    void tagLobby ( uint64_t lobbyId, const char *type );

    void startPump();
    void stopPump();

    void timerExpired ( Timer *timer ) override;
};

#endif // ENABLE_STEAM
