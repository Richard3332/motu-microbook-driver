#include <os/log.h>
#include <DriverKit/DriverKit.h>
#include <AudioDriverKit/AudioDriverKit.h>
#include "MicroBookAudioDevice.h"
#include "MicroBookRate.h"

using namespace AudioDriverKit;

struct MicroBookAudioDevice_IVars {
    MBRateCallback callback = nullptr;
    void          *context = nullptr;
};

OSSharedPtr<MicroBookAudioDevice> MicroBookAudioDevice::Create(IOUserAudioDriver *in_driver, bool in_supports_prewarming,
                                                                OSString *in_device_uid, OSString *in_model_uid,
                                                                OSString *in_manufacturer_uid, uint32_t in_zero_timestamp_period)
{
    // OSTypeAlloc returns an object with refcount 1, so adopt it without retaining.
    OSSharedPtr<MicroBookAudioDevice> dev(OSTypeAlloc(MicroBookAudioDevice), OSNoRetain);
    if (!dev) return nullptr;
    if (!dev->init(in_driver, in_supports_prewarming, in_device_uid, in_model_uid, in_manufacturer_uid, in_zero_timestamp_period))
        return nullptr;
    return dev;
}

bool MicroBookAudioDevice::init(IOUserAudioDriver *in_driver, bool in_supports_prewarming, OSString *in_device_uid,
                                OSString *in_model_uid, OSString *in_manufacturer_uid, uint32_t in_zero_timestamp_period)
{
    if (!super::init(in_driver, in_supports_prewarming, in_device_uid, in_model_uid, in_manufacturer_uid, in_zero_timestamp_period))
        return false;
    ivars = IONewZero(MicroBookAudioDevice_IVars, 1);
    return ivars != nullptr;
}

void MicroBookAudioDevice::free()
{
    IOSafeDeleteNULL(ivars, MicroBookAudioDevice_IVars, 1);
    super::free();
}

void MicroBookAudioDevice::SetRateCallback(void *callback, void *context)
{
    if (!ivars) return;
    ivars->callback = reinterpret_cast<MBRateCallback>(callback);
    ivars->context = context;
}

kern_return_t MicroBookAudioDevice::HandleChangeSampleRate(double in_sample_rate)
{
    // Let the driver validate/prepare the hardware, then record the new rate.
    if (ivars && ivars->callback) {
        kern_return_t ret = ivars->callback(ivars->context, in_sample_rate);
        if (ret != kIOReturnSuccess) return ret;
    }
    return SetSampleRate(in_sample_rate);
}
