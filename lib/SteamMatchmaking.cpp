#ifdef ENABLE_STEAM

#include "SteamMatchmaking.hpp"
#include "SteamManager.hpp"
#include "SteamSocket.hpp"   // steamAddr()
#include "Logger.hpp"

#include <windows.h>         // Sleep, VK_ESCAPE

using namespace std;


// Bounded relay-readiness wait (~5s). The matched-wait below is intentionally unbounded: queueing
// for an opponent can take arbitrarily long (cancel with ESC under RELEASE, like the server path).
#define READY_TRIES ( 500 )
#define READY_SLEEP_MS ( 10 )
#define MATCH_SLEEP_MS ( 20 )


SteamMatchmaking::SteamMatchmaking ( IMatchmakingBackend::Owner* owner, string region )
    : _region ( region )
{
    this->owner = owner;
}

SteamMatchmaking::~SteamMatchmaking()
{
    SteamManager::get().cancelMatchmaking();
    // Release the ref taken by the MainUi backend factory. Guarded no-op if MainApp already shut
    // Steam down when the match launched.
    SteamManager::get().deref();
}

void SteamMatchmaking::start()
{
    // Steam was already ref()'d by the MainUi factory; the actual search begins in waitConnected().
    _cancel = false;
}

bool SteamMatchmaking::isRunning()
{
    return SteamManager::get().isInitialized();
}

void SteamMatchmaking::stop()
{
    _cancel = true;
    SteamManager::get().cancelMatchmaking();
}

void SteamMatchmaking::sendHostReady()
{
    // No-op for Steam: the client already resolved the host's SteamID from the lobby, so there is
    // no "host is ready" round-trip to make (that exists only in the relay protocol).
}

void SteamMatchmaking::waitConnected()
{
    SteamManager& sm = SteamManager::get();

    // Wait for the SDR relay network so the later ConnectP2P succeeds, then kick the search.
    for ( int i = 0; i < READY_TRIES && ! sm.isRelayNetworkReady(); ++i )
    {
        sm.pump();
        Sleep ( READY_SLEEP_MS );
    }

    sm.matchmakeRegion ( _region );
    connectionSuccess = true;
}

void SteamMatchmaking::waitMatched()
{
    SteamManager& sm = SteamManager::get();

    // Queue until a peer is found (host) or we joined someone (client), or the user cancels.
    while ( ! _cancel && sm.mmState() == SteamManager::MMState::Working )
    {
        sm.pump();
        Sleep ( MATCH_SLEEP_MS );
    }

    const SteamManager::MMState st = sm.mmState();

    if ( st == SteamManager::MMState::BecameHost )
    {
        matchSuccess = true;
        if ( owner )
            owner->setMode ( this, "Host" );
    }
    else if ( st == SteamManager::MMState::BecameClient )
    {
        matchSuccess = true;
        if ( owner )
        {
            owner->setMode ( this, "Client" );
            owner->setAddr ( this, steamAddr ( sm.mmPeerId() ) );
        }
    }
    else
    {
        // Cancelled or failed.
        if ( owner )
            owner->setMode ( this, "Offline" );
    }
}

void SteamMatchmaking::keyboardEvent ( uint32_t vkCode, uint32_t scanCode, bool isExtended, bool isDown )
{
    if ( vkCode == VK_ESCAPE && ! ignoreKb )
        stop();
}

#endif // ENABLE_STEAM
