#ifdef ENABLE_STEAM

#include "SteamLobby.hpp"
#include "SteamManager.hpp"
#include "SteamSocket.hpp"   // steamAddr()
#include "LobbyQueue.hpp"
#include "Logger.hpp"

#include <windows.h>         // Sleep
#include <cctype>
#include <cstdlib>

using namespace std;


// Bounded pump loop: ~5s like the standalone steam() UI used. There is no EventManager loop
// during the lobby menu, so we drive SteamManager's manual dispatch directly here.
#define SETTLE_TRIES ( 500 )
#define SETTLE_SLEEP_MS ( 10 )


// ---- king-of-the-hill queue keys ----
// Lobby data (owner-written) broadcasts the current match assignment; member data (self-written)
// carries each player's ready flag and reported result.
static const char *Q_GEN_KEY    = "q_gen";      // match generation counter
static const char *Q_STATE_KEY  = "q_state";    // "PLAYING" / "ASSEMBLING"
static const char *Q_ORDER_KEY  = "q_order";    // comma-separated SteamIDs, front..back
static const char *Q_HOST_KEY   = "q_host";     // current king (host) SteamID
static const char *Q_CLIENT_KEY = "q_client";   // current challenger SteamID
static const char *READY_KEY    = "q_ready";    // member: "1" if readied to play
static const char *R_GEN_KEY    = "q_rgen";     // member: gen this reported result is for
static const char *R_WINNER_KEY = "q_rwinner";  // member: winning SteamID for R_GEN


static string joinIds ( const vector<uint64_t>& ids )
{
    string s;
    for ( size_t i = 0; i < ids.size(); ++i )
    {
        if ( i )
            s += ",";
        char b[32];
        snprintf ( b, sizeof ( b ), "%llu", ( unsigned long long ) ids[i] );
        s += b;
    }
    return s;
}

static vector<uint64_t> splitIds ( const string& s )
{
    vector<uint64_t> out;
    size_t i = 0;
    while ( i < s.size() )
    {
        size_t j = s.find ( ',', i );
        if ( j == string::npos )
            j = s.size();
        if ( j > i )
        {
            const uint64_t v = strtoull ( s.substr ( i, j - i ).c_str(), nullptr, 10 );
            if ( v )
                out.push_back ( v );
        }
        i = j + 1;
    }
    return out;
}


SteamLobby::SteamLobby ( ILobbyBackend::Owner* owner )
{
    this->owner = owner;
    mode = MENU;
}

SteamLobby::~SteamLobby()
{
    SteamManager::get().leaveLobby();
    // Release the ref taken by the MainUi backend factory. Guarded no-op if MainApp already
    // shut Steam down before launching the game (the match path).
    SteamManager::get().deref();
}

void SteamLobby::start()
{
    // Steam was already ref()'d by the MainUi factory. Nothing async to start; the UI opens at
    // the top-level lobby MENU.
    mode = MENU;
    connectionSuccess = true;
}

bool SteamLobby::isRunning()
{
    // Steam may be shut down mid-session by MainApp when a match launches; that ends the lobby.
    return SteamManager::get().isInitialized();
}

void SteamLobby::stop()
{
    // Drop out of the play queue before leaving so the owner doesn't keep us in the rotation.
    if ( _ready )
        setReady ( false );
    SteamManager::get().leaveLobby();
}


// ---- menu data (called by MainUi while it holds entryMutex; must NOT re-lock) ----

vector<string> SteamLobby::getMenu()
{
    switch ( mode )
    {
        case MENU:
            // No "Default Lobby" entry for Steam (that relay-only mode has no Steam analog).
            return { "Public Lobbies", "Create Lobby", "Enter Lobby Code" };
        case CONCERTO_BROWSE:
            return publiclobbies;
        case CONCERTO_LOBBY:
            return _qs.rows;
        default:
            return {};
    }
}

vector<string> SteamLobby::getIps()
{
    if ( mode == CONCERTO_LOBBY )
        return lobbyips;
    return {};
}

vector<string> SteamLobby::getIds()
{
    return lobbyids;
}


// ---- browse / create / join ----

void SteamLobby::fetchPublicLobby()
{
    SteamManager::get().requestPublicLobbies();
    _pending = P_BROWSE;
}

void SteamLobby::create ( string name, string type )
{
    SteamManager::get().setLobbyMemberName ( name );
    SteamManager::get().createLobby ( type == "Public" );
    _pending = P_CREATE;
}

string SteamLobby::join ( string name, int selection )
{
    SteamManager& sm = SteamManager::get();
    const vector<SteamManager::LobbyInfo>& lobbies = sm.publicLobbies();

    string code;
    if ( selection >= 0 && selection < ( int ) lobbies.size() )
    {
        sm.setLobbyMemberName ( name );
        sm.joinLobbyById ( lobbies[selection].lobbyId );
        code = lobbies[selection].code;
        _pending = P_JOIN;
    }
    return code;
}

void SteamLobby::join ( string name, string code )
{
    SteamManager::get().setLobbyMemberName ( name );
    SteamManager::get().joinLobbyByCode ( code );
    _pending = P_JOIN;
}

bool SteamLobby::checkLobbyCode ( string code )
{
    if ( code.size() < 2 || code.size() > 8 )
        return false;
    for ( size_t i = 0; i < code.size(); ++i )
        if ( ! isalnum ( ( unsigned char ) code[i] ) )
            return false;
    return true;
}


// ---- challenge handshake ----

void SteamLobby::challenge ( string target, IpAddrPort port )
{
    SteamManager& sm = SteamManager::get();
    const uint64_t targetId = strtoull ( target.c_str(), nullptr, 10 );
    const uint64_t myId = sm.getSteamID();

    // Advertise the challenge (also publishes our SteamID as host_id).
    sm.setChallenge ( targetId );

    // Let the peer's member-data arrive, then re-read for the simultaneous-challenge tie-break.
    for ( int i = 0; i < 40; ++i )
    {
        sm.pump();
        Sleep ( 5 );
    }

    bool peerChallengingMe = false;
    for ( const SteamManager::MemberInfo& m : sm.lobbyMembers() )
        if ( m.steamId == targetId && m.challengingTarget == myId )
            peerChallengingMe = true;

    // Tie-break: if we both challenged each other, the LOWER SteamID hosts. The higher SteamID
    // backs off (clears its challenge) and reconnects as Client - the peer's host row appears on
    // the next refresh, so selecting it again takes the (now non-"None") client branch.
    if ( peerChallengingMe && myId > targetId )
    {
        sm.clearChallenge();
        hostSuccess = false;
        refresh();
        return;
    }

    hostSuccess = true;
}

void SteamLobby::preaccept ( string id )
{
    // Client side: nothing to negotiate over Steam - the caller connects directly to the host's
    // SteamID (already resolved into the address by MainUi from the member's host row).
}

void SteamLobby::accept()
{
    // Server-backed lobby notifies the relay that the connection is up; over Steam the P2P
    // connection is direct, so there is nothing to confirm.
}

void SteamLobby::unhost()
{
    SteamManager::get().clearChallenge();
    hostSuccess = false;
}

void SteamLobby::end()
{
    // Stay in the lobby; just make sure our challenge advertisement is withdrawn.
    SteamManager::get().clearChallenge();
}

void SteamLobby::host ( string name, IpAddrPort port )
{
    // DEFAULT_LOBBY (relay-only peer list) has no Steam analog and is not reachable from the
    // Steam menu (getMenu() omits it). No-op.
}


// ---- backend seam ----

void SteamLobby::refresh()
{
    SteamManager& sm = SteamManager::get();

    // Process any pending lobby-data / chat callbacks so the member cache is current.
    for ( int i = 0; i < 3; ++i )
        sm.pump();

    if ( mode == CONCERTO_LOBBY )
    {
        LOCK ( entryMutex );
        readQueueState();
    }
    else if ( mode == CONCERTO_BROWSE )
    {
        LOCK ( entryMutex );

        publiclobbies.clear();
        roomcodes.clear();

        for ( const SteamManager::LobbyInfo& li : sm.publicLobbies() )
        {
            roomcodes.push_back ( li.code );
            publiclobbies.push_back ( li.code + "   " + to_string ( li.memberCount ) );
        }

        numEntries = ( int ) publiclobbies.size();
    }
}

void SteamLobby::pumpUntilSettled()
{
    SteamManager& sm = SteamManager::get();

    if ( _pending == P_NONE )
        return;

    if ( _pending == P_BROWSE )
    {
        for ( int i = 0; i < SETTLE_TRIES && sm.browseState() == SteamManager::LobbyState::Working; ++i )
        {
            sm.pump();
            Sleep ( SETTLE_SLEEP_MS );
        }
    }
    else
    {
        for ( int i = 0; i < SETTLE_TRIES && sm.lobbyState() == SteamManager::LobbyState::Working; ++i )
        {
            sm.pump();
            Sleep ( SETTLE_SLEEP_MS );
        }
    }

    finishPending();
}

void SteamLobby::finishPending()
{
    SteamManager& sm = SteamManager::get();
    const Pending p = _pending;
    _pending = P_NONE;

    switch ( p )
    {
        case P_CREATE:
        case P_JOIN:
            if ( sm.lobbyState() == SteamManager::LobbyState::Ready )
            {
                mode = CONCERTO_LOBBY;
                lobbyMsg = sm.lobbyCode(); // host's code (empty for a joiner, which is fine)
                refresh();
            }
            else
            {
                lobbyError = "Failed";
            }
            break;

        case P_BROWSE:
            if ( sm.browseState() == SteamManager::LobbyState::Ready )
                refresh();
            break;

        default:
            break;
    }
}


// ---- king-of-the-hill queue ----

void SteamLobby::setReady ( bool ready )
{
    _ready = ready;
    SteamManager::get().setMyMemberData ( READY_KEY, ready ? "1" : "0" );
}

QueueState SteamLobby::getQueueState()
{
    // Called by MainUi while it holds entryMutex (like getMenu()); just return the cached snapshot.
    return _qs;
}

void SteamLobby::reportResult ( uint32_t gen, uint64_t winnerId )
{
    SteamManager& sm = SteamManager::get();
    // Write the winner before the gen, so a reader that sees the new gen always sees a winner.
    sm.setMyMemberData ( R_WINNER_KEY, to_string ( winnerId ) );
    sm.setMyMemberData ( R_GEN_KEY, to_string ( gen ) );
}

uint64_t SteamLobby::readResult ( uint32_t gen, uint64_t host, uint64_t client )
{
    SteamManager& sm = SteamManager::get();
    const uint64_t players[2] = { host, client };
    for ( uint64_t pid : players )
    {
        if ( ! pid )
            continue;
        const uint32_t rgen = ( uint32_t ) strtoul ( sm.getMemberData ( pid, R_GEN_KEY ).c_str(), nullptr, 10 );
        if ( rgen == gen )
        {
            const uint64_t w = strtoull ( sm.getMemberData ( pid, R_WINNER_KEY ).c_str(), nullptr, 10 );
            if ( w )
                return w;
        }
    }
    return 0;
}

void SteamLobby::publishMatchOrAssemble()
{
    SteamManager& sm = SteamManager::get();

    if ( _queueOrder.size() >= 2 )
    {
        ++_gen;
        sm.setLobbyData ( Q_ORDER_KEY, joinIds ( _queueOrder ) );
        sm.setLobbyData ( Q_HOST_KEY, to_string ( _queueOrder[0] ) );
        sm.setLobbyData ( Q_CLIENT_KEY, to_string ( _queueOrder[1] ) );
        sm.setLobbyData ( Q_GEN_KEY, to_string ( _gen ) );
        sm.setLobbyData ( Q_STATE_KEY, "PLAYING" );
        LOG ( "Queue gen=%u host=%llu client=%llu", _gen,
              ( unsigned long long ) _queueOrder[0], ( unsigned long long ) _queueOrder[1] );
    }
    else
    {
        // Only (re)write when something changed, to avoid SetLobbyData rate-limit spam.
        const string order = joinIds ( _queueOrder );
        if ( sm.getLobbyData ( Q_STATE_KEY ) != "ASSEMBLING" )
            sm.setLobbyData ( Q_STATE_KEY, "ASSEMBLING" );
        if ( sm.getLobbyData ( Q_ORDER_KEY ) != order )
            sm.setLobbyData ( Q_ORDER_KEY, order );
    }
}

void SteamLobby::coordinatorTick()
{
    if ( mode != CONCERTO_LOBBY )
        return;

    SteamManager& sm = SteamManager::get();
    const uint64_t myId = sm.getSteamID();

    // Only the lobby owner coordinates. SetLobbyData from anyone else is ignored by Steam anyway,
    // and ownership migrates automatically if the owner leaves, so a successor seamlessly resumes.
    if ( sm.lobbyOwnerId() != myId )
        return;

    // Snapshot members once; derive the ready set and a membership test from it.
    const vector<SteamManager::MemberInfo> members = sm.lobbyMembers();
    vector<uint64_t> ready;
    for ( const SteamManager::MemberInfo& m : members )
        if ( sm.getMemberData ( m.steamId, READY_KEY ) == "1" )
            ready.push_back ( m.steamId );

    auto isMember = [&] ( uint64_t id ) -> bool
    {
        for ( const SteamManager::MemberInfo& m : members )
            if ( m.steamId == id )
                return true;
        return false;
    };

    // Adopt the last-published order on first run / after an ownership migration.
    if ( _queueOrder.empty() )
    {
        _queueOrder = splitIds ( sm.getLobbyData ( Q_ORDER_KEY ) );
        _gen = ( uint32_t ) strtoul ( sm.getLobbyData ( Q_GEN_KEY ).c_str(), nullptr, 10 );
    }

    if ( sm.getLobbyData ( Q_STATE_KEY ) == "PLAYING" )
    {
        const uint32_t gen   = ( uint32_t ) strtoul ( sm.getLobbyData ( Q_GEN_KEY ).c_str(), nullptr, 10 );
        const uint64_t host  = strtoull ( sm.getLobbyData ( Q_HOST_KEY ).c_str(), nullptr, 10 );
        const uint64_t client = strtoull ( sm.getLobbyData ( Q_CLIENT_KEY ).c_str(), nullptr, 10 );

        const uint64_t winner = readResult ( gen, host, client );
        if ( winner )
        {
            // Normal case: a player reported the outcome.
            _queueOrder = LobbyQueue::advance ( _queueOrder, host, client, winner, ready );
            publishMatchOrAssemble();
        }
        else if ( ! isMember ( host ) && ! isMember ( client ) )
        {
            // Both competitors have left the lobby (a live match keeps them as members, so this
            // is a hard crash, not just a slow result). Drop the pair to the very back.
            LOG ( "Queue gen=%u: both players left without a result; rotating", gen );
            const vector<uint64_t> reconciled = LobbyQueue::reconcile ( _queueOrder, ready );
            vector<uint64_t> head, tail;
            for ( uint64_t id : reconciled )
                ( ( id == host || id == client ) ? tail : head ).push_back ( id );
            head.insert ( head.end(), tail.begin(), tail.end() );
            _queueOrder = head;
            publishMatchOrAssemble();
        }
        // Otherwise the match is still in progress (or one player crashed and the survivor's
        // self-report will arrive next tick) -> wait.
    }
    else
    {
        // Assembling / between matches: keep the order reconciled and start once 2+ are ready.
        _queueOrder = LobbyQueue::reconcile ( _queueOrder, ready );
        publishMatchOrAssemble();
    }

    // Reflect whatever we just published in our own snapshot this same tick, so an owner who is
    // also a player starts its match without a one-tick lag.
    LOCK ( entryMutex );
    readQueueState();
}

void SteamLobby::readQueueState()
{
    // Assumes entryMutex is held by the caller (refresh() / coordinatorTick()).
    SteamManager& sm = SteamManager::get();
    const uint64_t myId = sm.getSteamID();
    const vector<SteamManager::MemberInfo> members = sm.lobbyMembers();

    QueueState qs;
    qs.gen      = ( uint32_t ) strtoul ( sm.getLobbyData ( Q_GEN_KEY ).c_str(), nullptr, 10 );
    qs.phase    = ( sm.getLobbyData ( Q_STATE_KEY ) == "PLAYING" ) ? QueuePhase::Playing
                                                                   : QueuePhase::Assembling;
    qs.hostId   = strtoull ( sm.getLobbyData ( Q_HOST_KEY ).c_str(), nullptr, 10 );
    qs.clientId = strtoull ( sm.getLobbyData ( Q_CLIENT_KEY ).c_str(), nullptr, 10 );
    qs.iAmReady = _ready;
    if ( qs.hostId )
        qs.hostAddr = IpAddrPort ( steamAddr ( qs.hostId ), ( uint16_t ) 0 );

    // My role in the current match. Un-readied players stay in the menu (None); readied players who
    // are not one of the two competitors auto-spectate.
    if ( qs.phase == QueuePhase::Playing && qs.gen )
    {
        if ( qs.hostId == myId )
            qs.myRole = QueueRole::Host;
        else if ( qs.clientId == myId )
            qs.myRole = QueueRole::Client;
        else if ( _ready )
            qs.myRole = QueueRole::Spectator;
        else
            qs.myRole = QueueRole::None;
    }

    auto nameOf = [&] ( uint64_t id ) -> string
    {
        for ( const SteamManager::MemberInfo& m : members )
            if ( m.steamId == id )
                return m.name.empty() ? to_string ( id ) : m.name;
        return to_string ( id );
    };

    // Row 0 is the ready toggle (MainUi maps selection 0 -> setReady). The rest are informational.
    qs.rows.clear();
    qs.rows.push_back ( _ready ? "[*] Ready - select here to leave the queue"
                               : "[ ] Ready Up to join the queue" );

    const vector<uint64_t> order = splitIds ( sm.getLobbyData ( Q_ORDER_KEY ) );
    int pos = 1;
    for ( uint64_t id : order )
    {
        string tag;
        if ( qs.phase == QueuePhase::Playing && id == qs.hostId )
            tag = "  [KING - playing]";
        else if ( qs.phase == QueuePhase::Playing && id == qs.clientId )
            tag = "  [challenger - playing]";
        const string me = ( id == myId ) ? " (you)" : "";
        qs.rows.push_back ( to_string ( pos++ ) + ". " + nameOf ( id ) + me + tag );
    }

    // Members present but not in the play queue (un-readied watchers).
    for ( const SteamManager::MemberInfo& m : members )
    {
        if ( LobbyQueue::contains ( order, m.steamId ) )
            continue;
        const string me = ( m.steamId == myId ) ? " (you)" : "";
        qs.rows.push_back ( "- " + ( m.name.empty() ? to_string ( m.steamId ) : m.name ) + me + " (watching)" );
    }

    if ( qs.phase != QueuePhase::Playing && order.size() < 2 )
        qs.rows.push_back ( "Waiting for players to ready up..." );

    _qs = qs;
    numEntries = ( int ) qs.rows.size();
}

#endif // ENABLE_STEAM
