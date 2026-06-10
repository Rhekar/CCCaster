#pragma once

#include "IMatchmakingBackend.hpp"

#include <string>


// Steam-backed matchmaking (implements IMatchmakingBackend). Backs the existing "Server ->
// Matchmaking" UI with a region-filtered create-or-join over Steam lobbies via SteamManager. No
// background thread or socket: pumped from the UI thread (waitConnected/waitMatched). Only
// meaningful when built with ENABLE_STEAM (the .cpp body is guarded).
class SteamMatchmaking : public IMatchmakingBackend
{
public:

    SteamMatchmaking ( IMatchmakingBackend::Owner* owner, std::string region );
    ~SteamMatchmaking();

    void start() override;
    bool isRunning() override;
    void stop() override;

    void sendHostReady() override;

    bool isSteam() const override { return true; }
    bool needsEventManager() const override { return false; }
    void waitConnected() override;
    void waitMatched() override;

    void keyboardEvent ( uint32_t vkCode, uint32_t scanCode, bool isExtended, bool isDown ) override;

private:

    std::string _region;
    bool _cancel = false;   // set by ESC / stop() to break the matched-wait loop
};
