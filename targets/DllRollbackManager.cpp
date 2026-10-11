#include "DllRollbackManager.hpp"
#include "MemDump.hpp"
#include "DllAsmHacks.hpp"
#include "ErrorStringsExt.hpp"

#include <utility>
#include <algorithm>

using namespace std;


// Linked rollback memory data (binary message format)
extern const unsigned char binary_res_rollback_bin_start;
extern const unsigned char binary_res_rollback_bin_end;

// Deserialized rollback memory data
static MemDumpList allAddrs;

// The game's replay recorder for the round being played, or 0 outside of a recorded round
static RepRound *currentRepRound()
{
    RepRound *end = *( RepRound ** ) CC_REPROUND_TBL_ENDPTR_ADDR;

    if ( !end )
        return 0;

    RepRound *round = end - 1;
    return ( round->inputs ? round : 0 );
}

// Number of round entries in the game's replay recorder table
static int repRoundCount()
{
    const char *begin = *( char ** ) ( ( char * ) CC_REPROUND_TBL_ENDPTR_ADDR - 4 );
    const char *end = *( char ** ) CC_REPROUND_TBL_ENDPTR_ADDR;

    return ( ( begin && end >= begin ) ? ( end - begin ) / sizeof ( RepRound ) : 0 );
}

// Remember where the replay recorder is, so that a rollback can return it to exactly this point.
// The recorder appends one input per simulated tick, which is not one per real frame (slow motion, hit freeze), so
// erasing one input per rolled back frame removes inputs that are never recorded again.
static void snapshotRepTracks ( RepTrackSnapshot *snapshots )
{
    RepRound *round = currentRepRound();

    for ( int i = 0; i < 4; ++i )
    {
        memset ( &snapshots[i], 0, sizeof ( RepTrackSnapshot ) );

        if ( !round || !round->inputs[i].states )
            continue;

        const RepInputContainer& inputs = round->inputs[i];

        snapshots[i].len = inputs.statesEnd - ( char * ) inputs.states;
        snapshots[i].active = inputs.activeIndex;
        snapshots[i].total1 = inputs.totalFrameCount;
        snapshots[i].total2 = inputs.totalFrameCount2;

        if ( snapshots[i].len >= ( int ) sizeof ( RepInputState ) )
            snapshots[i].last = * ( RepInputState * ) ( inputs.statesEnd - sizeof ( RepInputState ) );
    }
}

// Return the replay recorder to the state it was in when the snapshots were taken.
// Appending only ever changes the active run or adds a new one, so this is exact.
static void restoreRepTracks ( const RepTrackSnapshot *snapshots, int savedRoundCount )
{
    RepRound *round = currentRepRound();

    if ( !round )
    {
        LOG ( "Missing replay table or inputs" );
        return;
    }

    if ( savedRoundCount != repRoundCount() )
    {
        LOG ( "Saved state is from a different round; leaving replay inputs as is" );
        return;
    }

    // Assumes there are always containers for 4 players in input container table; may not be true
    for ( int i = 0; i < 4; ++i )
    {
        RepInputContainer& inputs = round->inputs[i];

        if ( !inputs.states )
            continue;

        const RepTrackSnapshot& snapshot = snapshots[i];
        const int len = inputs.statesEnd - ( char * ) inputs.states;

        if ( snapshot.len < 0 || snapshot.len > len || ( snapshot.len % sizeof ( RepInputState ) ) )
        {
            LOG ( "Cannot restore replay inputs for p%d: saved length %d, current length %d", i + 1, snapshot.len, len );
            continue;
        }

        // Erase the runs appended after the save point
        memset ( ( char * ) inputs.states + snapshot.len, 0, len - snapshot.len );
        inputs.statesEnd = ( char * ) inputs.states + snapshot.len;
        inputs.activeIndex = snapshot.active;
        inputs.totalFrameCount = snapshot.total1;
        inputs.totalFrameCount2 = snapshot.total2;

        // The active run's frame count at the save point
        if ( snapshot.len >= ( int ) sizeof ( RepInputState ) )
            * ( RepInputState * ) ( inputs.statesEnd - sizeof ( RepInputState ) ) = snapshot.last;
    }
}

template<typename T>
static inline void deleteArray ( T *ptr ) { delete[] ptr; }


void DllRollbackManager::GameState::save()
{
    ASSERT ( rawBytes != 0 );

    char *dump = rawBytes;

    for ( const MemDump& mem : allAddrs.addrs )
        mem.saveDump ( dump );

    ASSERT ( dump == rawBytes + allAddrs.totalSize );
}

void DllRollbackManager::GameState::load()
{
    fesetenv(&fp_env);

    ASSERT ( rawBytes != 0 );

    const char *dump = rawBytes;

    for ( const MemDump& mem : allAddrs.addrs )
        mem.loadDump ( dump );

    ASSERT ( dump == rawBytes + allAddrs.totalSize );
}

void DllRollbackManager::allocateStates()
{
    if ( allAddrs.empty() )
    {
        const size_t size = ( ( char * ) &binary_res_rollback_bin_end ) - ( char * ) &binary_res_rollback_bin_start;
        allAddrs.load ( ( char * ) &binary_res_rollback_bin_start, size );
    }

    if ( allAddrs.empty() )
        THROW_EXCEPTION ( "Failed to load rollback data!", ERROR_BAD_ROLLBACK_DATA );

    if ( ! _memoryPool )
        _memoryPool.reset ( new char[NUM_ROLLBACK_STATES * allAddrs.totalSize], deleteArray<char> );

    for ( size_t i = 0; i < NUM_ROLLBACK_STATES; ++i )
        _freeStack.push ( i * allAddrs.totalSize );

    _statesList.clear();

    for ( auto& sfxArray : _sfxHistory )
        memset ( &sfxArray[0], 0, CC_SFX_ARRAY_LEN );
}

void DllRollbackManager::deallocateStates()
{
    _memoryPool.reset();

    while ( ! _freeStack.empty() )
        _freeStack.pop();

    _statesList.clear();
}

void DllRollbackManager::saveState ( const NetplayManager& netMan )
{
    if ( _freeStack.empty() )
    {
        ASSERT ( _statesList.empty() == false );

        if ( _statesList.front().indexedFrame.parts.frame <= netMan.getRemoteFrame() )
        {
            auto it = _statesList.begin();
            ++it;
            _freeStack.push ( it->rawBytes - _memoryPool.get() );
            _statesList.erase ( it );
        }
        else
        {
            _freeStack.push ( _statesList.front().rawBytes - _memoryPool.get() );
            _statesList.pop_front();
        }
    }

    std::fenv_t fp_env;

    fegetenv(&fp_env);

    GameState state =
    {
        netMan._state,
        netMan._startWorldTime,
        netMan._indexedFrame,
        fp_env,
        _memoryPool.get() + _freeStack.top()
    };

    snapshotRepTracks ( state.repTrack );
    state.repRoundCount = repRoundCount();

    _freeStack.pop();
    state.save();
    _statesList.push_back ( state );

    uint8_t *currentSfxArray = &_sfxHistory [ netMan.getFrame() % NUM_ROLLBACK_STATES ][0];
    memcpy ( currentSfxArray, AsmHacks::sfxFilterArray, CC_SFX_ARRAY_LEN );
}

bool DllRollbackManager::loadState ( IndexedFrame indexedFrame, NetplayManager& netMan )
{
    if ( _statesList.empty() )
    {
        LOG ( "Failed to load state: indexedFrame=%s", indexedFrame );
        return false;
    }

    LOG ( "Trying to load state: indexedFrame=%s; _statesList={ %s ... %s }",
          indexedFrame, _statesList.front().indexedFrame, _statesList.back().indexedFrame );

    const uint32_t origFrame = netMan.getFrame();

    for ( auto it = _statesList.rbegin(); it != _statesList.rend(); ++it )
    {
#ifdef RELEASE
        if ( ( it->indexedFrame.value <= indexedFrame.value ) || ( & ( *it ) == &_statesList.front() ) )
#else
        if ( it->indexedFrame.value <= indexedFrame.value )
#endif
        {
            LOG ( "Loaded state: indexedFrame=%s", it->indexedFrame );

            // Overwrite the current game state
            netMan._state = it->netplayState;
            netMan._startWorldTime = it->startWorldTime;
            netMan._indexedFrame = it->indexedFrame;
            it->load();

            // Erase all other states after the current one.
            // Note: it.base() returns 1 after the position of it, but moving forward.
            for ( auto jt = it.base(); jt != _statesList.end(); ++jt )
            {
                _freeStack.push ( jt->rawBytes - _memoryPool.get() );
            }

            // Disable rollback for input history if in training mode
            if ( !netMan.config.mode.isTraining() )
                restoreRepTracks ( it->repTrack, it->repRoundCount );

            _statesList.erase ( it.base(), _statesList.end() );

            // Initialize the SFX filter by flagging all played SFX flags in the range (R,S),
            // where R is the actual reset frame, and S is the original starting frame.
            // Note: we can skip frame S, because the current SFX filter array is already initialized by frame S.
            for ( uint32_t i = netMan.getFrame() + 1; i < origFrame; ++i )
            {
                for ( uint32_t j = 0; j < CC_SFX_ARRAY_LEN; ++j )
                    AsmHacks::sfxFilterArray[j] |= _sfxHistory [ i % NUM_ROLLBACK_STATES ][j];
            }

            // We set the SFX filter flag to 0x80. Since played (but filtered) SFX are incremented,
            // unplayed sound effects in the filter will stay as 0 or 0x80.
            for ( uint32_t j = 0; j < CC_SFX_ARRAY_LEN; ++j )
            {
                if ( AsmHacks::sfxFilterArray[j] )
                    AsmHacks::sfxFilterArray[j] = 0x80;
            }

            return true;
        }
    }

    LOG ( "Failed to load state: indexedFrame=%s", indexedFrame );
    return false;
}

void DllRollbackManager::saveRerunSounds ( uint32_t frame )
{
    uint8_t *currentSfxArray = &_sfxHistory [ frame % NUM_ROLLBACK_STATES ][0];

    // Rewrite the sound effects history during re-run
    for ( uint32_t j = 0; j < CC_SFX_ARRAY_LEN; ++j )
    {
        if ( AsmHacks::sfxFilterArray[j] & ~0x80 )
            currentSfxArray[j] = 1;
        else
            currentSfxArray[j] = 0;
    }
}

void DllRollbackManager::finishedRerunSounds()
{
    // Cancel unplayed sound effects after rollback
    for ( uint32_t j = 0; j < CC_SFX_ARRAY_LEN; ++j )
    {
        // Filter flag 0x80 means the SFX didn't play after rollback since the filter didn't get incremented
        if ( AsmHacks::sfxFilterArray[j] == 0x80 )
        {
            // Play the SFX muted to cancel it
            CC_SFX_ARRAY_ADDR[j] = 1;
            AsmHacks::sfxMuteArray[j] = 1;
        }
    }

    // Cleared last played sound effects
    memset ( AsmHacks::sfxFilterArray, 0, CC_SFX_ARRAY_LEN );
}
