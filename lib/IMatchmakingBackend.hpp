#pragma once

#include "IpAddrPort.hpp"
#include "KeyboardManager.hpp"

#include <string>


// Backend interface for the "Server -> Matchmaking" menu. The concrete backend is either the
// relay server (ServerMatchmaking, a Thread+Socket that exchanges MMSTART/HOST/CLIENT) or Steam
// (SteamMatchmaking, a region-filtered create-or-join over Steam lobbies). Inherits
// KeyboardManager::Owner so AutoManager can hook ESC-to-cancel for either backend (the hook is
// only active under RELEASE, but the type must satisfy it). The waitConnected/waitMatched seam
// mirrors ILobbyBackend's pumpUntilSettled: the server backend signals MainUi's uiCondVar from
// its thread, while the Steam backend pumps from the UI thread.
struct IMatchmakingBackend : public KeyboardManager::Owner
{
    struct Owner
    {
        virtual void connectionFailed ( IMatchmakingBackend *mmm ) = 0;
        virtual void setAddr ( IMatchmakingBackend *mmm, std::string addr ) = 0;
        virtual void setMode ( IMatchmakingBackend *mmm, std::string mode ) = 0;
        virtual void unlock ( IMatchmakingBackend *mmm ) = 0;
    };

    Owner *owner = 0;

    bool connectionSuccess = false;
    bool matchSuccess = false;
    bool ignoreKb = false;

    virtual ~IMatchmakingBackend() {}

    virtual void start() = 0;
    virtual bool isRunning() = 0;
    virtual void stop() = 0;

    virtual void sendHostReady() = 0;

    // ---- backend seam (server vs Steam) ----
    virtual bool isSteam() const { return false; }
    virtual bool needsEventManager() const { return true; }

    // Block until connected to the pool (server) / the lobby search settles (Steam).
    virtual void waitConnected() {}
    // Block until a Host/Client role is decided (setMode/setAddr fired).
    virtual void waitMatched() {}
};
