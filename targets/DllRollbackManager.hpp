#pragma once

#include "DllNetplayManager.hpp"
#include "Constants.hpp"

#include <memory>
#include <stack>
#include <list>
#include <array>
#include <cfenv>

struct __attribute__((packed)) RepInputState
{
    char unk1;
    char frameCount;
    char unk2[6];
};

struct __attribute__((packed)) RepInputContainer
{
    char           unk1[4];
    RepInputState* states;
    char*          statesEnd;
    char           unk2[4];
    int            totalFrameCount;
    int            totalFrameCount2;
    int            activeIndex;
    char           unk3[4];
};

struct __attribute__((packed)) RepRound
{
    char               unk1[0x120];
    RepInputContainer* inputs;      // points to an array of 4 input structs
    char               unk2[0x1C];
};

// One player's replay recorder state at a rollback save point: length of the run list in bytes, index of the active run,
// both frame totals, and the active run itself
struct RepTrackSnapshot
{
    int len;
    int active;
    int total1;
    int total2;
    RepInputState last;
};

class DllRollbackManager
{
public:

    // Allocate / deallocate memory for saving game states
    void allocateStates();
    void deallocateStates();

    // Save / load current game state
    void saveState ( const NetplayManager& netMan );
    bool loadState ( IndexedFrame indexedFrame, NetplayManager& netMan );

    // Save sounds during rollback re-run
    void saveRerunSounds ( uint32_t frame );

    // Finalize rollback sound effects
    void finishedRerunSounds();

private:

    struct GameState
    {
        // Each game state is uniquely identified by (netplayState, startWorldTime, indexedFrame).
        // They are chronologically ordered by index and then frame.
        NetplayState netplayState;
        uint32_t startWorldTime;
        IndexedFrame indexedFrame;
        std::fenv_t fp_env;

        // The pointer to the raw bytes in the state pool
        char *rawBytes;

        // The replay recorder at the time this state was saved, and the number of round entries in the recorder table
        // (identifies the round the snapshot belongs to)
        RepTrackSnapshot repTrack[4];
        int repRoundCount;

        // Save / load the game state
        void save();
        void load();
    };

    // Memory pool to allocate game states
    std::shared_ptr<char> _memoryPool;

    // Unused indices in the memory pool, each game state has the same size
    std::stack<size_t> _freeStack;

    // List of saved game states in chronological order
    std::list<GameState> _statesList;

    // History of sound effect playbacks
    std::array<std::array<uint8_t, CC_SFX_ARRAY_LEN>, NUM_ROLLBACK_STATES> _sfxHistory;
};
