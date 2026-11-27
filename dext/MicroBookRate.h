// Plain C++ helper types shared by the audio device subclass and the driver.
#pragma once
#include <DriverKit/DriverKit.h>
// Called by the audio device when Core Audio asks for a new sample rate. Return kIOReturnSuccess to accept.
typedef kern_return_t (*MBRateCallback)(void *context, double sampleRate);
