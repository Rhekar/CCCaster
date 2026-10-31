#include "DllFrameRate.hpp"
#include "TimerManager.hpp"
#include "Constants.hpp"
#include "ProcessManager.hpp"
#include "DllAsmHacks.hpp"

#include <timeapi.h>
#include <synchapi.h>

#include <d3dx9.h>
#include <math.h>

#include "DllAsmHacks.hpp"

//using namespace std;
using namespace DllFrameRate;

namespace DllFrameRate
{

double desiredFps = 60.0;

double actualFps = 60.0;

#define POLL_TIMEOUT_DEFAULT (10);

int POLL_TIMEOUT = POLL_TIMEOUT_DEFAULT;

bool isEnabled = false;

LARGE_INTEGER constBaseFreq = {0};

void enable()
{
    if ( isEnabled )
        return;


	QueryPerformanceFrequency(&constBaseFreq);

	// this runs regardless of the setting in caster. why. 

    // TODO find an alternative because this doesn't work on Wine
    WRITE_ASM_HACK ( AsmHacks::disableFpsLimit );
    WRITE_ASM_HACK ( AsmHacks::disableFpsCounter );

	// this will call limitfps after present (and the stuff which hooks onto present) exits
	// it might be better? but it makes my frame counter look worse, for obvious reasons
	for ( const AsmHacks::Asm& hack : AsmHacks::hookPresentCaller ) {
		WRITE_ASM_HACK ( hack );
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
    

    /*
    below, the 1000 / desiredFps, the 1000 casts said number to an int
    causing the value to be 16, instead of 16.666, which is what led to the game running at ~62.5 instead of 60
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

LARGE_INTEGER getPerfCounter() {

	static LARGE_INTEGER baseFreq;
	static LARGE_INTEGER millisecondDuration;
	static bool isFirstRun = true;
	if(isFirstRun) {
		isFirstRun = false;
		QueryPerformanceFrequency(&baseFreq); 
		millisecondDuration.QuadPart = baseFreq.QuadPart / 1000;
	}

	LARGE_INTEGER currTime;
	QueryPerformanceCounter(&currTime);
	LARGE_INTEGER res;
	res.QuadPart = currTime.QuadPart / millisecondDuration.QuadPart;
	return res;
}

void newCasterFrameLimiter() {

    /*
    
    overall, this frame limiter is much better, but still is causing some issues
    is it needed to swing back and do a frame fast if a lag frame occurs?
    i had assumed that it would be better this way, maybe not.

    people are having issues, but ive also fixed a ton of other issues. its 50/50

    maybe aiming for a consistent 60 over ... yk what im doing now is better

    but hypothetically, if someone had a massive lagframe, it would cause the game to over/under/idkflow the value needed for the bounceback
    which would cause fastmelty? that ... makes sense tbh
    but doesnt explain how those issues last for more than 60 frames

    but tbh the other counter doesnt even do bounceback like how im saying here. it does wait every frame, and then does weird shit on every 5 and 30 and 60, but it still is just waiting
    why the fuck is this causing issues?

    if im really going to throw shit at the wall, its possible that metlys frame limiter not being disabled also causes issues?

	what if.. i just isolate the 
	i could maybe seperate the render state/gamestate

	in process explorer, nothings melty had rtkvhd64.sys as the entry instead of mbaa entry
	could that be something?

    */

    static int rollingFrameAverageSum = 0;
    static uint32_t rollingFrameAverage[60] = {0};\
    const int rollingFrameAverageLen = (sizeof(rollingFrameAverage)/sizeof(rollingFrameAverage[0]));
    static int rollingFrameAverageIndex = 0;

	static LARGE_INTEGER baseFreq;
	static LARGE_INTEGER freq; 
	static LARGE_INTEGER prevFrameTime;
	static LARGE_INTEGER millisecondDuration;

    static DWORD lastColor = 0x000000FF; // avoid excessive calls to patch

    static int lagFrameTimer = 0;
    static int totalFrames = 0;
    static int totalLagFrames = 0;
    static bool paranoia = false; // if this is true, switch to a more cpu heavy method

	static MMRESULT timeBeginPeriodRes = TIMERR_NOERROR;

    static uint32_t omfg;
	static bool isFirstRun = true;
	if(isFirstRun) {
		isFirstRun = false;

		QueryPerformanceFrequency(&baseFreq); // i need to handle errors here, and maybe fallback to a different system. is this guarenteed to have enough resolution?

		millisecondDuration.QuadPart = baseFreq.QuadPart / 1000;

        // fill the buffer with 60 to start
        uint32_t fillVar = desiredFps * 100;
        for(int i=0; i<rollingFrameAverageLen; i++) {
            rollingFrameAverage[i] = fillVar;
        }
        rollingFrameAverageSum = rollingFrameAverageLen * fillVar;

        omfg = fillVar;

		prevFrameTime.QuadPart = 0;

		timeBeginPeriodRes = timeBeginPeriod(1);
		timeEndPeriod(1);
	}

    if(!paranoia) {

        float ratio = (float)totalLagFrames / (float)totalFrames;
        //log("%f", ratio);
        if((totalFrames > (15 * 60)) && ratio > (1.0/60.0)) {
            paranoia = true;
        }
    }

	freq.QuadPart = baseFreq.QuadPart / desiredFps; // hopefully this doesnt mess things up? this desiredfps var isnt touched anywhere really

	LARGE_INTEGER currTime;

    bool wasLagFrame = true;
	
	
	//log("%lld %lld %lld", currTime.QuadPart - prevFrameTime.QuadPart, millisecondDuration.QuadPart, freq.QuadPart);

	// does this help ? 
	// no fucking clue. i think it does?
	// check process explorer for a cycle count. try setting affinity to use only one core. 
	// this change seems to provide a good reduction in cpu usage (check process explorer thread properties)
	QueryPerformanceCounter(&currTime);
	LARGE_INTEGER frameTimeLeft;
	frameTimeLeft.QuadPart = (freq.QuadPart - (currTime.QuadPart - prevFrameTime.QuadPart));
	if((timeBeginPeriodRes == TIMERR_NOERROR) && (frameTimeLeft.QuadPart > 2 * millisecondDuration.QuadPart)) { // i tried putting this shit in the wait loop, dont, bad idea

        // sleep delay to free up cpu time
		DWORD millis = frameTimeLeft.QuadPart / millisecondDuration.QuadPart;

		/*
		DWORD sleepTime = millis * 0.5;

		if(millis > 10) {
			sleepTime = millis * 0.75; // feel more comfy sleeping for longer if we have more leeway
		}*/

        // windows reduces sleep resolution when not focused, i should also that that into account here

		DWORD sleepTime = millis - 1;
        if(sleepTime > 2) {
            sleepTime--;
        }
        
		if(lagFrameTimer > 0) { // we are lagging. possibly sleep less? i need a perm solution for if the lagging is consistent
			sleepTime = millis * 0.67;
		}

        if(paranoia) { // paranoia mode triggered, sleep for even less as to avoid sleep possibly going too high?
            sleepTime = millis * 0.4;
        }

		/*
        timeBeginPeriod(1);
		Sleep(sleepTime);
		timeEndPeriod(1);
        */

        HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	
        LARGE_INTEGER time;
        time.QuadPart = -(int)(sleepTime * millisecondDuration.QuadPart); // why does this need to be negative 	
        SetWaitableTimer(timer, &time, 0, NULL, NULL, 0);
        
        timeBeginPeriod(1);  // this might be better than sleep. when tested, it had a lower max, avg, and stdev to sleep.
        WaitForSingleObject(timer, INFINITE);
        timeEndPeriod(1);
	}
	
	
    while(true) { // busywait delay for accuracy
		QueryPerformanceCounter(&currTime);
		if(currTime.QuadPart - prevFrameTime.QuadPart > freq.QuadPart) {
			break;
		}
        wasLagFrame = false; // being here means we have some time to wait.

		// ANY quantity of sleep in this loop massively frees up cpu, massively decreasing people complaining, but having it be done accurately is close to impossible.
	}

    if(wasLagFrame) { 
        if(totalFrames > (15 * 60)) {
            totalLagFrames++;
        }
        lagFrameTimer = 30;
    } else {
        if(lagFrameTimer > 0) {
            lagFrameTimer--;
        }
    }
    totalFrames++;

	uint32_t temp = (100*baseFreq.QuadPart) / (currTime.QuadPart - prevFrameTime.QuadPart - 1); // minus one is there to display 60, not 59.999999
    rollingFrameAverageSum -= rollingFrameAverage[rollingFrameAverageIndex];
    rollingFrameAverageSum += temp;
    rollingFrameAverage[rollingFrameAverageIndex] = temp;
    rollingFrameAverageIndex = (rollingFrameAverageIndex + 1) % rollingFrameAverageLen;

    if(wasLagFrame || rollingFrameAverageIndex == 0) { // old frame limiter only updated this value every second
        *CC_FPS_COUNTER_ADDR = round(((float)rollingFrameAverageSum * 0.01)/ rollingFrameAverageLen);
        if(wasLagFrame || lagFrameTimer) {
            DWORD tempCol = 0xFFFF00DD;
            if(lastColor != tempCol) {
                patchDWORD(CC_FPS_COUNTER_COLOR, tempCol);
                lastColor = tempCol;
            }
        } else {
            DWORD tempCol = 0x0000FFFF;
            if(lastColor != tempCol) {
                patchDWORD(CC_FPS_COUNTER_COLOR, tempCol);
                lastColor = tempCol;
            }
        }   
    }
   
	prevFrameTime.QuadPart = currTime.QuadPart;
}

void newerCasterFrameLimiter() {

	// ideally, if this frame occurs too "fast" i could just let the next frame go forward, and then hold the wait right before present is called??

	static LARGE_INTEGER prevTime;
	LARGE_INTEGER currTime; 

	LARGE_INTEGER idealTicksPerFrame;
	idealTicksPerFrame.QuadPart = constBaseFreq.QuadPart / desiredFps;

	QueryPerformanceCounter(&currTime);

	LARGE_INTEGER frameTimeSpent;
	frameTimeSpent.QuadPart = currTime.QuadPart - prevTime.QuadPart;

	if(currTime.QuadPart - prevTime.QuadPart < idealTicksPerFrame.QuadPart) {
	
		LARGE_INTEGER timeRemaining;
		timeRemaining.QuadPart = idealTicksPerFrame.QuadPart - frameTimeSpent.QuadPart;
		
		// its hard to know how accurate this timer is. to be safe, ill div by 2
		timeRemaining.QuadPart = (int)(timeRemaining.QuadPart / 2); // why does this need to be negative 	
		
		if(timeRemaining.QuadPart > (idealTicksPerFrame.QuadPart * 0.1)) { // to be safe, make sure we would sleep for at least a 10th of a frame.
			timeRemaining.QuadPart = -timeRemaining.QuadPart;
			HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			SetWaitableTimer(timer, &timeRemaining, 0, NULL, NULL, 0);
			timeBeginPeriod(1);  // this might be better than sleep. when tested, it had a lower max, avg, and stdev to sleep.
			WaitForSingleObject(timer, INFINITE);
			timeEndPeriod(1);
		}

		do {
			QueryPerformanceCounter(&currTime);
			frameTimeSpent.QuadPart = currTime.QuadPart - prevTime.QuadPart;
		} while(frameTimeSpent.QuadPart < idealTicksPerFrame.QuadPart);
	}

	
	prevTime.QuadPart = currTime.QuadPart; 

}

void updateFPSCounter() {

    static int bufferIndex = 0;
    const int bufferSize = 100;
    static LARGE_INTEGER buffer[bufferSize];
	
	static LARGE_INTEGER prevTime;
	LARGE_INTEGER currTime; 

	currTime = getPerfCounter();

	buffer[bufferIndex].QuadPart = currTime.QuadPart - prevTime.QuadPart;

	LARGE_INTEGER sum;
	sum.QuadPart = 0;
	for(int i=0; i<bufferSize; i++) {
		sum.QuadPart += buffer[i].QuadPart;
	}
	double temp = ((double)sum.QuadPart) / ((double)bufferSize); 

	LARGE_INTEGER avg;
	avg.QuadPart = sum.QuadPart / bufferSize;

	if(bufferIndex == 0) {
		//log("%7.8lf %7.8lf", temp, 1000.0f/temp);
	
		*CC_FPS_COUNTER_ADDR = (DWORD)((1000.0f/temp) + 0.5f);
		//*CC_FPS_COUNTER_ADDR = (DWORD)(100000.0f/temp);
	}

	
	
	bufferIndex = (bufferIndex + 1) % bufferSize;
	prevTime = currTime;

}

void limitFPS() {

	if ( !isEnabled || *CC_SKIP_FRAMES_ADDR )
        return;

	//oldCasterFrameLimiter();
	
	//newCasterFrameLimiter();

    newerCasterFrameLimiter();

    updateFPSCounter();

}

}

void __attribute__ ((noinline)) PresentFrameEnd ( IDirect3DDevice9 *device )
{
	// comment this out if you uncommented the hookPresentCaller hack
	//DllFrameRate::limitFPS();
}

void setDesiredFPS(double desiredFps_) {

	if(desiredFps_ < 100.0) {
		POLL_TIMEOUT = POLL_TIMEOUT_DEFAULT;
	} else {
		POLL_TIMEOUT = 0;
	}

    desiredFps = desiredFps_;
}
