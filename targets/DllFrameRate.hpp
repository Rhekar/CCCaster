#pragma once

#include <cstdint>



namespace DllFrameRate
{

extern int POLL_TIMEOUT;

extern double desiredFps;

extern double actualFps;

void enable();

void limitFPS();

}

extern "C" __attribute__((visibility("default"))) void setDesiredFPS(double desiredFps_);
