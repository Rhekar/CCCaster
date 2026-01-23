

#include "DllFrameRate.hpp"
#include "TimerManager.hpp"
#include "Constants.hpp"
#include "ProcessManager.hpp"
#include "DllAsmHacks.hpp"

#include <timeapi.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wpedantic"
#include <chrono>
#pragma GCC diagnostic pop

#include <d3dx9.h>

long long getMicroSec() {
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

//using namespace std;
using namespace DllFrameRate;


namespace DllFrameRate
{

double desiredFps = 60.0;

double actualFps = 60.0;

bool isEnabled = false;


void enable()
{
    if ( isEnabled )
        return;

    // TODO find an alternative because this doesn't work on Wine
    WRITE_ASM_HACK ( AsmHacks::disableFpsLimit );
    WRITE_ASM_HACK ( AsmHacks::disableFpsCounter );

    isEnabled = true;

    LOG ( "Enabling FPS control!" );
}

}



void oldCasterFrameLimiter() {

    static uint64_t last1f = 0, last5f = 0, last30f = 0, last60f = 0;
    static uint8_t counter = 0;
    
    ++counter;

    uint64_t now = TimerManager::get().getNow ( true );
    

    /**
     * The best timer resolution is only in milliseconds, and we need to make
     * sure the spacing between frames is as close to even as possible.
     *
     * What this code does is check every 30f, 5f, and 1f how many milliseconds have
     * passed since the last check and make sure we are close to or under the desired FPS.
     */
    
     if ( counter % 30 == 0 )
    {
        while ( now - last30f < ( 30 * 1000 ) / desiredFps )
            now = TimerManager::get().getNow ( true );

        last30f = now;
    }
    else if ( counter % 5 == 0 )
    {
        while ( now - last5f < ( 5 * 1000 ) / desiredFps )
            now = TimerManager::get().getNow ( true );

        last5f = now;
    }
    else
    {
        while ( now - last1f < 1000 / desiredFps )
            now = TimerManager::get().getNow ( true );
    }

    last1f = now;

    if ( counter >= 60 )
    {
        now = TimerManager::get().getNow ( true );

        actualFps = 1000.0 / ( ( now - last60f ) / 60.0 );

        *CC_FPS_COUNTER_ADDR = uint32_t ( actualFps + 0.5 );

        counter = 0;
        last60f = now;
    }

    
}

void newCasterFrameLimiter() {

    // ive been thinking.

    static bool isFirstRun = true;
    if(isFirstRun) { 
        // this is bad code, ideally, i just set the timer res in main, but that would possibly mess up things when this fps method is not selected
        isFirstRun = false;
        timeBeginPeriod(1); 
    }
    
    static long long prevFrameTime = 0;

    long long curTime = getMicroSec();

    if(prevFrameTime == 0) {
        prevFrameTime = curTime;
        return;
    }

    long long delta = curTime - prevFrameTime;

    constexpr double desiredFPS = 60.0;
    constexpr long long lim = ((1.0 / desiredFPS) * 1000000.0);
    //long long lim = 16000; // melty being at ~62fps instead of 60 could be explained by going with .016 instead of .01666

    if(delta > lim) { // 16.66ms
        prevFrameTime = curTime;
        return;
    }

    /*
    // the previous sleep option is also busy, i see no reason to change that
    long long needSleep = lim - delta;
    needSleep /= 1000; // convert to ms
    needSleep - 3; // minus 1 here bc the scheduler is set to that time period. actually, minus 2 bc paranoia
    if(needSleep > 0) {
        Sleep(needSleep); 
    }
    */
    
    while(delta < lim) {
        curTime = getMicroSec();
        delta = curTime - prevFrameTime;
    }

    prevFrameTime = curTime;
    
}

void PresentFrameEnd ( IDirect3DDevice9 *device )
{
    if ( !isEnabled || *CC_SKIP_FRAMES_ADDR )
        return;

    //oldCasterFrameLimiter();
    newCasterFrameLimiter();

    
}
