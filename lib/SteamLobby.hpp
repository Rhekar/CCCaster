#pragma once

#include "ILobbyBackend.hpp"

#include <cstdint>
#include <string>
#include <vector>


// Steam-backed lobby (implements ILobbyBackend). Backs the existing "Server -> Lobby" UI with
// Steam lobbies via SteamManager. Unlike the relay ServerLobby it has NO background thread and NO
// socket: it is pumped from the UI thread (refresh() on the periodic tick, pumpUntilSettled() at
// each async wait point). Only meaningful when built with ENABLE_STEAM (the .cpp body is guarded).
class SteamLobby : public ILobbyBackend
{
public:

    SteamLobby ( ILobbyBackend::Owner* owner );
    ~SteamLobby();

    // lifecycle
    void start() override;
    bool isRunning() override;
    void stop() override;

    // menu data (per mode)
    std::vector<std::string> getMenu() override;
    std::vector<std::string> getIps() override;
    std::vector<std::string> getIds() override;

    // browse / create / join
    void fetchPublicLobby() override;
    void create ( std::string name, std::string type ) override;
    std::string join ( std::string name, int selection ) override;
    void join ( std::string name, std::string code ) override;
    bool checkLobbyCode ( std::string code ) override;

    // challenge handshake
    void challenge ( std::string target, IpAddrPort port ) override;
    void preaccept ( std::string id ) override;
    void accept() override;
    void unhost() override;
    void end() override;
    void host ( std::string name, IpAddrPort port ) override;

    // backend seam
    bool isSteam() const override { return true; }
    bool needsEventManager() const override { return false; }
    void refresh() override;
    void pumpUntilSettled() override;

    // king-of-the-hill queue
    bool supportsQueue() const override { return true; }
    void setReady ( bool ready ) override;
    QueueState getQueueState() override;
    void reportResult ( uint32_t gen, uint64_t winnerId ) override;
    void coordinatorTick() override;

private:

    // Which async Steam op is in flight, so pumpUntilSettled() knows what to wait on + apply.
    enum Pending { P_NONE, P_CREATE, P_JOIN, P_BROWSE };
    Pending _pending = P_NONE;

    // ---- queue state ----
    bool _ready = false;                   // our own ready flag (source of truth we advertise)
    QueueState _qs;                        // cached snapshot, rebuilt in refresh() under entryMutex

    // Owner-only authoritative queue (front..back). Seeded from the published order on first tick
    // / after ownership migration, then maintained in memory.
    std::vector<uint64_t> _queueOrder;
    uint32_t _gen = 0;                     // last gen we (as owner) published

    // Rebuild _qs from the lobby + member data (called at the end of refresh() in CONCERTO_LOBBY).
    void readQueueState();

    // Owner helpers.
    uint64_t readResult ( uint32_t gen, uint64_t host, uint64_t client );
    void publishMatchOrAssemble();

    // Per-mode menu vectors (rebuilt in refresh(); read by getMenu/getIps/getIds while MainUi
    // holds entryMutex, so those readers must not re-lock).
    std::vector<std::string> publiclobbies;   // CONCERTO_BROWSE
    std::vector<std::string> roomcodes;        // browse-index -> code
    std::vector<std::string> lobbyentries;     // CONCERTO_LOBBY member display names
    std::vector<std::string> lobbyips;         // CONCERTO_LOBBY: "None" or steam:<hostId>
    std::vector<std::string> lobbyids;         // CONCERTO_LOBBY member SteamID64 strings

    void finishPending();
};
