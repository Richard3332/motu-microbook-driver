#include <os/log.h>
#include <DriverKit/DriverKit.h>
#include <USBDriverKit/USBDriverKit.h>
#include "MicroBookDevice.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "MicroBookDevice: " fmt, ##__VA_ARGS__)

struct MicroBookDevice_IVars {
    IOUSBHostDevice *device = nullptr;
};

bool MicroBookDevice::init()
{
    if (!super::init()) return false;
    ivars = IONewZero(MicroBookDevice_IVars, 1);
    return ivars != nullptr;
}

void MicroBookDevice::free()
{
    IOSafeDeleteNULL(ivars, MicroBookDevice_IVars, 1);
    super::free();
}

kern_return_t IMPL(MicroBookDevice, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) return ret;

    auto dev = OSDynamicCast(IOUSBHostDevice, provider);
    if (!dev) return kIOReturnBadArgument;

    ret = dev->Open(this, 0, 0);
    if (ret != kIOReturnSuccess) { LOG("device Open failed 0x%x", ret); return ret; }
    dev->retain();
    ivars->device = dev;

    const IOUSBDeviceDescriptor *dd = dev->CopyDeviceDescriptor();
    uint16_t bcd = dd ? dd->bcdDevice : 0;
    LOG("attached, bcdDevice=0x%04x", bcd);
    if (dd) IOUSBHostFreeDescriptor(dd);

    if (bcd == 0x0060) {
        // Original driver sends a vendor request here to leave a loader state. Unverified,
        // so we do nothing for that case.
        LOG("device is in loader state (bcdDevice 0x60); not configuring");
        return kIOReturnSuccess;
    }

    // Only the hardware revision this driver was developed and tested on. Other MOTU revisions share
    // the same vendor/product ID but may speak a different protocol, so leave them alone.
    if (bcd != 0x0032) {
        LOG("unsupported bcdDevice 0x%04x; not configuring", bcd);
        return kIOReturnSuccess;
    }

    ret = dev->SetConfiguration(1, true);
    LOG("SetConfiguration(1) -> 0x%x", ret);
    return kIOReturnSuccess;
}

kern_return_t IMPL(MicroBookDevice, Stop)
{
    if (ivars->device) {
        ivars->device->Close(this, 0);
        ivars->device->release();
        ivars->device = nullptr;
    }
    return Stop(provider, SUPERDISPATCH);
}
