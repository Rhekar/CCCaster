#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>


// King-of-the-hill queue ordering rules, deliberately free of any Steam / networking dependency
// so they can be unit-tested in isolation (see tests/Test.LobbyQueue.cpp). SteamLobby owns the
// transport + state; these functions only decide who is where in line.
//
// The queue is an ordered list of readied players' SteamIDs, front..back. The two at the front
// play the current match: order[0] hosts (the reigning "king") and order[1] challenges. When the
// match ends the winner stays at the front and the loser drops to the very back.
namespace LobbyQueue
{

inline bool contains ( const std::vector<uint64_t>& v, uint64_t x )
{
    return std::find ( v.begin(), v.end(), x ) != v.end();
}

// Drop ids no longer in `ready` (left or un-readied) while preserving their relative order, then
// append any newly-readied ids (in `ready` but missing from `order`) to the back in `ready` order.
inline std::vector<uint64_t> reconcile ( const std::vector<uint64_t>& order,
                                         const std::vector<uint64_t>& ready )
{
    std::vector<uint64_t> out;

    for ( uint64_t id : order )
        if ( contains ( ready, id ) && ! contains ( out, id ) )
            out.push_back ( id );

    for ( uint64_t id : ready )
        if ( ! contains ( out, id ) )
            out.push_back ( id );

    return out;
}

// Advance the queue after a match between `host` (front) and `client` (second) won by `winner`.
// King-of-the-hill: the winner becomes/stays the front king; the loser drops to the very back.
// Members missing from `ready` are removed; newly-readied members are appended ahead of the loser.
// `winner` is only re-seated if it is still in `ready`.
inline std::vector<uint64_t> advance ( const std::vector<uint64_t>& order,
                                       uint64_t host, uint64_t client, uint64_t winner,
                                       const std::vector<uint64_t>& ready )
{
    const uint64_t loser = ( winner == host ) ? client : host;

    std::vector<uint64_t> out;

    // The winner is the new king at the front (if still ready).
    if ( winner && contains ( ready, winner ) )
        out.push_back ( winner );

    // The existing queue order, minus the two who just played.
    for ( uint64_t id : order )
        if ( id != winner && id != loser && contains ( ready, id ) && ! contains ( out, id ) )
            out.push_back ( id );

    // Members who readied / joined during the match, ahead of the loser.
    for ( uint64_t id : ready )
        if ( id != loser && ! contains ( out, id ) )
            out.push_back ( id );

    // The loser goes to the very back.
    if ( loser && loser != winner && contains ( ready, loser ) )
        out.push_back ( loser );

    return out;
}

} // namespace LobbyQueue
