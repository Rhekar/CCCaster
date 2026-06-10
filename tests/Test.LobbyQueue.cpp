#ifndef RELEASE

#include "LobbyQueue.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace std;


// Convenience: all of {1,2,3,...} readied, in the given order.
static vector<uint64_t> ids ( initializer_list<uint64_t> l ) { return vector<uint64_t> ( l ); }


TEST ( LobbyQueue, WinnerStaysLoserToBack )
{
    // 1 (king) beats 2; 1 stays at the front, 2 drops behind 3.
    const vector<uint64_t> order = ids ( { 1, 2, 3 } );
    const vector<uint64_t> ready = ids ( { 1, 2, 3 } );

    const vector<uint64_t> next = LobbyQueue::advance ( order, /*host*/ 1, /*client*/ 2, /*winner*/ 1, ready );

    EXPECT_EQ ( next, ids ( { 1, 3, 2 } ) );
}


TEST ( LobbyQueue, ChallengerDethronesKing )
{
    // 2 beats the king 1; 2 becomes the new king, 1 drops to the back behind 3.
    const vector<uint64_t> order = ids ( { 1, 2, 3 } );
    const vector<uint64_t> ready = ids ( { 1, 2, 3 } );

    const vector<uint64_t> next = LobbyQueue::advance ( order, 1, 2, /*winner*/ 2, ready );

    EXPECT_EQ ( next, ids ( { 2, 3, 1 } ) );
}


TEST ( LobbyQueue, LoserUnreadiedIsDropped )
{
    // 2 loses and un-readies before the next round; it disappears from the queue entirely.
    const vector<uint64_t> order = ids ( { 1, 2, 3 } );
    const vector<uint64_t> ready = ids ( { 1, 3 } );

    const vector<uint64_t> next = LobbyQueue::advance ( order, 1, 2, 1, ready );

    EXPECT_EQ ( next, ids ( { 1, 3 } ) );
}


TEST ( LobbyQueue, NewJoinerAppendedAheadOfLoser )
{
    // 4 readied up during the match; it should sit ahead of the just-demoted loser (2).
    const vector<uint64_t> order = ids ( { 1, 2, 3 } );
    const vector<uint64_t> ready = ids ( { 1, 2, 3, 4 } );

    const vector<uint64_t> next = LobbyQueue::advance ( order, 1, 2, 1, ready );

    EXPECT_EQ ( next, ids ( { 1, 3, 4, 2 } ) );
}


TEST ( LobbyQueue, WinnerUnreadiedYieldsToField )
{
    // The king 1 wins but un-readied; it is not re-seated and 3 leads the next field.
    const vector<uint64_t> order = ids ( { 1, 2, 3 } );
    const vector<uint64_t> ready = ids ( { 2, 3 } );

    const vector<uint64_t> next = LobbyQueue::advance ( order, 1, 2, 1, ready );

    EXPECT_EQ ( next, ids ( { 3, 2 } ) );
}


TEST ( LobbyQueue, ReconcileDropsAndAppends )
{
    // 2 left; 4 and 5 readied. Survivors keep order, newcomers append in ready order.
    const vector<uint64_t> order = ids ( { 1, 2, 3 } );
    const vector<uint64_t> ready = ids ( { 3, 1, 4, 5 } );

    const vector<uint64_t> next = LobbyQueue::reconcile ( order, ready );

    EXPECT_EQ ( next, ids ( { 1, 3, 4, 5 } ) );
}


TEST ( LobbyQueue, TwoPlayersJustReplay )
{
    // With exactly two ready players, the loser-to-back is the same pair again next round.
    const vector<uint64_t> order = ids ( { 1, 2 } );
    const vector<uint64_t> ready = ids ( { 1, 2 } );

    const vector<uint64_t> next = LobbyQueue::advance ( order, 1, 2, 2, ready );

    EXPECT_EQ ( next, ids ( { 2, 1 } ) );
}


#endif // NOT RELEASE
