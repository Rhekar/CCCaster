#pragma once

#include "IpAddrPort.hpp"
#include "Thread.hpp"

#include <string>
#include <vector>


// Game types advertised by a lobby (server-backed Concerto lobbies use these).
enum GameType
{
    FFA,
    WinnerStaysOn,
};

// The lobby UI in MainUi::lobby() is a small state machine over these modes. Both the
// server-backed (ServerLobby) and Steam-backed (SteamLobby) implementations populate the
// same menu/ip/id vectors per mode, so the UI loop is backend-agnostic.
enum LobbyMode
{
    MENU,
    CONCERTO_BROWSE,
    CONCERTO_LOBBY,
    DEFAULT_LOBBY,
};


// Role this client plays in the current king-of-the-hill match.
enum class QueueRole
{
    None,       // not in a match: sit in the lobby menu (un-readied)
    Host,       // play, hosting (the reigning "king")
    Client,     // play, connecting to the king
    Spectator,  // readied but waiting: auto-spectate the live match
};

// Phase of the queue, broadcast by the lobby owner.
enum class QueuePhase
{
    Assembling, // fewer than two ready, or between matches: no live match
    Playing,    // a match is live (hostId vs clientId)
};

// Snapshot of the king-of-the-hill queue, rebuilt each refresh() and read by MainUi (Steam only).
struct QueueState
{
    uint32_t gen = 0;                       // current match generation; 0 == none yet
    QueuePhase phase = QueuePhase::Assembling;
    QueueRole myRole = QueueRole::None;
    uint64_t hostId = 0;                    // current match host (the king)
    uint64_t clientId = 0;                  // current challenger
    IpAddrPort hostAddr;                    // steam:<hostId>, for Client / Spectator to connect to
    bool iAmReady = false;
    std::vector<std::string> rows;          // display rows for the lobby UI
};


// Backend interface for the "Server -> Lobby" menu. MainUi drives this surface; a concrete
// backend is either the relay/Concerto server (ServerLobby, a Thread+Socket) or Steam lobbies
// (SteamLobby, driven by SteamManager's pump). The seam virtuals (isSteam/needsEventManager/
// refresh/pumpUntilSettled) let the single UI loop in MainUi::lobby() serve both: the server
// backend signals MainUi's uiCondVar from its own thread, while the Steam backend has no thread
// and must be pumped from the UI thread instead.
struct ILobbyBackend
{
    struct Owner
    {
        virtual void connectionFailed ( ILobbyBackend *lobby ) = 0;
        virtual void unlock ( ILobbyBackend *lobby ) = 0;
    };

    Owner *owner = 0;

    // ---- shared observable state (read by MainUi, some under entryMutex) ----
    IpAddrPort _address;            // host port ("46318") on the server host path; unused for Steam
    Mutex entryMutex;               // guards the menu/ip/id vectors during refresh
    LobbyMode mode = MENU;
    int numEntries = 0;
    bool hostSuccess = false;
    bool connectionSuccess = false;
    std::string lobbyError;
    std::string lobbyMsg;           // room code shown after create

    virtual ~ILobbyBackend() {}

    // ---- lifecycle ----
    virtual void start() = 0;
    virtual bool isRunning() = 0;
    virtual void stop() = 0;

    // ---- menu data (per mode) ----
    virtual std::vector<std::string> getMenu() = 0;
    virtual std::vector<std::string> getIps() = 0;
    virtual std::vector<std::string> getIds() = 0;

    // ---- browse / create / join ----
    virtual void fetchPublicLobby() = 0;
    virtual void create ( std::string name, std::string type ) = 0;      // type: "Public" / "Private"
    virtual std::string join ( std::string name, int selection ) = 0;    // browse-index join, returns code
    virtual void join ( std::string name, std::string code ) = 0;        // code join
    virtual bool checkLobbyCode ( std::string code ) = 0;

    // ---- in-lobby challenge handshake ----
    virtual void challenge ( std::string target, IpAddrPort port ) = 0;  // becomes Host
    virtual void preaccept ( std::string id ) = 0;                       // becomes Client
    virtual void accept() = 0;                                           // confirm connection
    virtual void unhost() = 0;
    virtual void end() = 0;
    virtual void host ( std::string name, IpAddrPort port ) = 0;         // DEFAULT_LOBBY only

    // ---- backend seam (server vs Steam) ----

    // True for the Steam backend. MainUi uses this to gate the OR-in of ClientMode::IsSteam +
    // a steam:<id> address at the RUN sites, and to choose pump-vs-condvar at every wait point.
    virtual bool isSteam() const { return false; }

    // The server backend runs its own EventManager loop on a thread; the Steam backend does not.
    // MainUi gates its EventManager::isRunning() guards on this.
    virtual bool needsEventManager() const { return true; }

    // Re-read the current mode's menu/ip/id vectors (under entryMutex). The server backend keeps
    // these current via its background thread + poll timer, so this is a no-op there; the Steam
    // backend re-enumerates lobby members / public lobbies here, called on the UI's periodic tick.
    virtual void refresh() {}

    // Block (pumping Steam) until the in-flight async op settles. The server backend signals
    // MainUi's uiCondVar from its thread instead, so this is a no-op there; the Steam backend
    // runs a bounded pump loop until its pending op reaches a terminal state.
    virtual void pumpUntilSettled() {}

    // ---- king-of-the-hill queue (Steam backend only; no-ops elsewhere) ----

    // True if this backend drives the auto-matchmaking queue instead of the manual challenge flow.
    virtual bool supportsQueue() const { return false; }

    // Toggle our own "ready to play" flag (advertised as member data).
    virtual void setReady ( bool ready ) {}

    // The current queue snapshot. Called by MainUi while it holds entryMutex (like getMenu()), so
    // implementations must NOT re-lock; refresh() rebuilds the cached snapshot under the lock.
    virtual QueueState getQueueState() { return {}; }

    // Publish the outcome of match `gen` (our own member data). Both players report; on a clean
    // finish they agree, on a disconnect only the survivor reports itself.
    virtual void reportResult ( uint32_t gen, uint64_t winnerId ) {}

    // Owner-only: advance the queue and publish the next assignment. No-op when not the owner.
    // Safe to call every UI tick.
    virtual void coordinatorTick() {}
};
