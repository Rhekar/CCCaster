

#include "DllFrameRate.hpp"
#include "TimerManager.hpp"
#include "Constants.hpp"
#include "ProcessManager.hpp"
#include "DllAsmHacks.hpp"

#include <timeapi.h>

/*
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wconversion"
#include <chrono>
#pragma GCC diagnostic pop

long long getMicroSec() {
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    //return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
*/

#include <d3dx9.h>



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

	// this runs regardless of the setting in caster. why. 

    // TODO find an alternative because this doesn't work on Wine
    WRITE_ASM_HACK ( AsmHacks::disableFpsLimit );
    WRITE_ASM_HACK ( AsmHacks::disableFpsCounter );

	// this will call limitfps after present (and the stuff which hooks onto present) exits
	// it might be better? but it makes my frame counter look worse, for obvious reasons
	for ( const AsmHacks::Asm& hack : AsmHacks::hookPresentCaller ) {
		//WRITE_ASM_HACK ( hack );
	}


    isEnabled = true;

    LOG ( "Enabling FPS control!" );
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

    // try using queryperfcounter
    // move it somewhere other than right after presentframeend

	// i could be converting time to doubles, but that fucks precision. this is less readable, but (hopefully) better
	// the issue is that whatever is causing this bs is related to QueryPerformanceCounter

	static LARGE_INTEGER baseFreq;
	static LARGE_INTEGER freq; // this is the amount of ticks for one frame
	static LARGE_INTEGER prevFrameTime;

	static bool isFirstRun = true;
	if(isFirstRun) {
		isFirstRun = false;

		QueryPerformanceFrequency(&baseFreq); // i need to handle errors here, and maybe fallback to a different system. is this guarenteed to have enough resolution?

		prevFrameTime.QuadPart = 0;
	}

	freq.QuadPart = baseFreq.QuadPart / desiredFps; // hopefully this doesnt mess things up? this desiredfps var isnt touched anywhere really

	LARGE_INTEGER currTime;

	while(true) {
		QueryPerformanceCounter(&currTime);
		if(currTime.QuadPart - prevFrameTime.QuadPart > freq.QuadPart) {
			break;
		}
	}

	uint32_t temp = (baseFreq.QuadPart) / (currTime.QuadPart - prevFrameTime.QuadPart - 1); // minus one is there to display 60, not 59.999999
	
	*CC_FPS_COUNTER_ADDR = temp;

	prevFrameTime.QuadPart = currTime.QuadPart;


}

void limitFPS() {

	if ( !isEnabled || *CC_SKIP_FRAMES_ADDR )
        return;

	//oldCasterFrameLimiter();
	newCasterFrameLimiter();

}

}

void PresentFrameEnd ( IDirect3DDevice9 *device )
{
	// comment this out if you uncommented the hookPresentCaller hack
	DllFrameRate::limitFPS();
}
