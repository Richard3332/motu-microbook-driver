#include <os/log.h>
#include <DriverKit/DriverKit.h>
#include <USBDriverKit/USBDriverKit.h>
#ifdef MB_AUDIO
#include <AudioDriverKit/AudioDriverKit.h>
using namespace AudioDriverKit;
#include "MicroBookAudioDevice.h"
#endif
#ifdef MB_AUDIO
#include "MicroBookAudioDriver.h"
#else
#include "MicroBookInterface.h"
#endif

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "MicroBookInterface: " fmt, ##__VA_ARGS__)

// ---- Endpoints (alt setting 1 carries the isochronous ones) --------------------------------
static const uint8_t kEpOut = 0x01, kEpIn = 0x81;          // interrupt: register command channel
static const uint8_t kEpIsoOut = 0x02, kEpIsoIn = 0x82;    // isochronous audio
static const uint32_t kPktMax = 40;

// ---- Register command channel opcodes (see MicroBook-protocol-notes.md) --------------------
static const uint8_t kOpWriteQuad = 0x00, kOpReadQuad = 0x04;
static const uint8_t kRspWriteQuad = 0x02, kRspReadQuad = 0x06;

// ---- Registers ------------------------------------------------------------------------------
static const uint32_t kRegSerial = 0x004, kRegId = 0x000, kRegMode = 0xB14, kRegLock = 0xB18;

// ---- Isochronous geometry, 48 kHz full speed (1 frame = 1 ms) -------------------------------
static const uint32_t kNumUrbs      = 4;     // requests kept queued per direction (latency ~ kNumUrbs x 10 ms)
static const uint32_t kOutFrames    = 10;    // frames per OUT request
static const uint32_t kOutFrameLen  = 288;   // buffer slot per frame (>= largest frame)
static const uint32_t kOutSamplesPerUrb = 441;   // 44.1 kHz: 10 frames = 441 samples (45 + 9*44)
static const uint32_t kInFrames     = 5;     // frames per IN request
static const uint32_t kInFrameMax   = 576;   // endpoint max packet = 48 * 4 ch * 3 bytes
static const uint32_t kInChannels   = 4;
static const uint32_t kStatsEvery   = 200;   // IN completions per log line (~1 s)
static const uint32_t kStopAfterIn  = 2000;  // ~10 s then stop by itself

#ifdef MB_AUDIO
static const bool     kAudioBuild    = true;
static const uint32_t kRingFrames    = 8192;                 // ring size = zero-timestamp period
static const uint32_t kOutBytesFrame = 6;                    // 2 ch x 24-bit little-endian (wire format)
static const uint32_t kInBytesFrame  = 12;                   // 4 ch x 24-bit little-endian (wire format)
// OUT is read from the ring this far ahead of when it is actually played.
static const uint32_t kQueueAheadFrames = kNumUrbs * 480;   // worst case (48 kHz)
#else
static const bool     kAudioBuild    = false;
#endif

// Samples in OUT frame fr (1 ms each). 44.1 kHz: 45 then 44 x9 (441 per 10 ms). 48 kHz: 48 every frame.
static inline uint32_t samplesInFrame(uint32_t rate, uint32_t fr)
{
    return rate == 48000 ? 48 : (fr == 0 ? 45 : 44);
}

struct Urb {
    IOBufferMemoryDescriptor *data = nullptr;
    IOBufferMemoryDescriptor *frames = nullptr;
    OSAction                 *action = nullptr;
};

struct MicroBookInterface_IVars {
    IOUSBHostInterface       *interface = nullptr;
    IOUSBHostPipe            *outPipe = nullptr, *inPipe = nullptr;
    IOUSBHostPipe            *isoOutPipe = nullptr, *isoInPipe = nullptr;
    IOBufferMemoryDescriptor *outBuf = nullptr, *inBuf = nullptr;
    OSAction                 *readAction = nullptr, *writeAction = nullptr;
    uint8_t                   seq = 0;
    int                       step = 0;
    bool                      stopping = false;
    bool                      runIsoc = false;
    bool                      isocRunning = false;
    Urb                       outUrb[kNumUrbs];
    Urb                       inUrb[kNumUrbs];
    uint64_t                  nextOutFrame = 0, nextInFrame = 0;
    uint32_t                  tonePos = 0;      // sample counter for the test tone
    uint32_t                  rate = 44100;     // 44100 or 48000 (set from MBForceRate)
    uint32_t                  modeValue = 1;    // value written to the mode register 0xB14
    uint32_t                  outSamplesPerUrb = 441;
#ifdef MB_AUDIO
    IOUserAudioDevice        *audioDevice = nullptr;
    IOUserAudioLevelControl  *volume = nullptr;        // software output volume (dB)
    IOUserAudioStream        *outStream = nullptr, *inStream = nullptr;
    uint32_t                  hwRate = 0;              // rate the hardware is currently set to (0 = unknown)
    double                    tbNumer = 1, tbDenom = 1;
    IOBufferMemoryDescriptor *outRing = nullptr, *inRing = nullptr;   // shared with Core Audio
    uint8_t                  *outRingPtr = nullptr, *inRingPtr = nullptr;
    uint64_t                  nextOutSample = 0;   // next frame to read from the OUT ring
    uint64_t                  inStoreSample = 0;   // next frame to write into the IN ring
    uint64_t                  playedSamples = 0;   // frames the device has finished playing
    uint64_t                  nextZts = 0;         // next zero-timestamp boundary (sample time)
    uint64_t                  playStartHost = 0;   // mach time of sample 0
    double                    ticksPerSample = 0;
    bool                      audioRunning = false;
#endif
    uint32_t                  inCompletions = 0;
    // per-second stats
    uint32_t                  stFrames = 0, stBytes = 0, stMinLen = 0xffff, stMaxLen = 0, stErrFrames = 0;
    int32_t                   stPeak[kInChannels] = {};
};

bool MicroBookInterface::init()
{
    if (!super::init()) return false;
    ivars = IONewZero(MicroBookInterface_IVars, 1);
    return ivars != nullptr;
}

void MicroBookInterface::free()
{
    IOSafeDeleteNULL(ivars, MicroBookInterface_IVars, 1);
    super::free();
}

kern_return_t IMPL(MicroBookInterface, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) return ret;

    auto intf = OSDynamicCast(IOUSBHostInterface, provider);
    if (!intf) return kIOReturnBadArgument;

    // Test switch lives in the personality (Info.plist): MBRunIsocTest = true
    OSDictionary *props = nullptr;
    if (CopyProperties(&props) == kIOReturnSuccess && props) {
        auto flag = OSDynamicCast(OSBoolean, props->getObject("MBRunIsocTest"));
        ivars->runIsoc = (flag == kOSBooleanTrue);
        props->release();
    }
    {
        // IVars are zero-filled (IONewZero does NOT run default member initializers), so set every
        // non-zero default explicitly. (Forgetting this left rate = 0 -> division by zero in the clock.)
        ivars->rate = 44100;
        ivars->modeValue = 1;
        ivars->outSamplesPerUrb = 441;
        ivars->hwRate = 0;
        ivars->stMinLen = 0xffff;
        ivars->tbNumer = 1; ivars->tbDenom = 1;
        OSDictionary *p2 = nullptr;
        if (CopyProperties(&p2) == kIOReturnSuccess && p2) {
            if (auto n = OSDynamicCast(OSNumber, p2->getObject("MBForceRate"))) {
                uint32_t r = n->unsigned32BitValue();
                if (r == 44100 || r == 48000) ivars->rate = r;
            }
            ivars->modeValue = (ivars->rate == 48000) ? 0 : 1;           // hypothesis: 1 = 44.1k, 0 = 48k
            if (auto n = OSDynamicCast(OSNumber, p2->getObject("MBModeValue")))
                ivars->modeValue = n->unsigned32BitValue();                // override for experiments
            p2->release();
        }
        ivars->outSamplesPerUrb = (ivars->rate == 48000) ? 480 : 441;
        LOG("rate=%u modeValue=%u", ivars->rate, ivars->modeValue);
    }
    if (kAudioBuild) ivars->runIsoc = true;   // audio flavour: alt 1 + mode register, isoc starts on StartDevice

    ret = intf->Open(this, 0, nullptr);
    if (ret != kIOReturnSuccess) { LOG("interface Open failed 0x%x", ret); return ret; }
    intf->retain();
    ivars->interface = intf;

    if (ivars->runIsoc) {
        // Alt setting 1 exposes the isochronous endpoints. Select it before taking pipes so the
        // pipe objects stay valid for the whole session.
        ret = intf->SelectAlternateSetting(1);
        LOG("SelectAlternateSetting(1) -> 0x%x", ret);
        if (ret != kIOReturnSuccess) return ret;
    }

    ret = intf->CopyPipe(kEpOut, &ivars->outPipe);
    if (ret != kIOReturnSuccess) { LOG("CopyPipe(out) failed 0x%x", ret); return ret; }
    ret = intf->CopyPipe(kEpIn, &ivars->inPipe);
    if (ret != kIOReturnSuccess) { LOG("CopyPipe(in) failed 0x%x", ret); return ret; }

    ret = intf->CreateIOBuffer(kIOMemoryDirectionInOut, kPktMax, &ivars->outBuf);
    if (ret != kIOReturnSuccess) return ret;
    ret = intf->CreateIOBuffer(kIOMemoryDirectionInOut, kPktMax, &ivars->inBuf);
    if (ret != kIOReturnSuccess) return ret;
    ret = CreateActionReadComplete(0, &ivars->readAction);
    if (ret != kIOReturnSuccess) return ret;
    ret = CreateActionWriteComplete(0, &ivars->writeAction);
    if (ret != kIOReturnSuccess) return ret;

    LOG("pipes ready (isoc test %s); reading register 0x%03x", ivars->runIsoc ? "ON" : "off", kRegSerial);
    ivars->step = 0;
    ret = PostRead();
    if (ret != kIOReturnSuccess) { LOG("PostRead failed 0x%x", ret); return ret; }
    ret = SendReadQuad(kRegSerial);
    if (ret != kIOReturnSuccess) { LOG("SendReadQuad failed 0x%x", ret); return ret; }
    return kIOReturnSuccess;
}

kern_return_t IMPL(MicroBookInterface, Stop)
{
    ivars->stopping = true;
    StopIsoc("driver stop");
#ifdef MB_AUDIO
    if (ivars->volume) { ivars->volume->release(); ivars->volume = nullptr; }
    if (ivars->outStream) { ivars->outStream->release(); ivars->outStream = nullptr; }
    if (ivars->inStream)  { ivars->inStream->release();  ivars->inStream = nullptr; }
    if (ivars->audioDevice) {
        RemoveObject(ivars->audioDevice);
        ivars->audioDevice->release();
        ivars->audioDevice = nullptr;
    }
    OSSafeReleaseNULL(ivars->outRing);
    OSSafeReleaseNULL(ivars->inRing);
#endif
    if (ivars->inPipe)  ivars->inPipe->Abort(0, kIOReturnAborted, this);
    if (ivars->outPipe) ivars->outPipe->Abort(0, kIOReturnAborted, this);
    OSSafeReleaseNULL(ivars->readAction);
    OSSafeReleaseNULL(ivars->writeAction);
    OSSafeReleaseNULL(ivars->inBuf);
    OSSafeReleaseNULL(ivars->outBuf);
    OSSafeReleaseNULL(ivars->inPipe);
    OSSafeReleaseNULL(ivars->outPipe);
    if (ivars->interface) {
        ivars->interface->Close(this, 0);
        OSSafeReleaseNULL(ivars->interface);
    }
    return Stop(provider, SUPERDISPATCH);
}

// ===== register command channel =====================================================================

kern_return_t MicroBookInterface::PostRead()
{
    if (ivars->stopping || !ivars->inPipe || !ivars->inBuf) return kIOReturnNotReady;
    return ivars->inPipe->AsyncIO(ivars->inBuf, kPktMax, ivars->readAction, 0);
}

static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

kern_return_t MicroBookInterface::SendReadQuad(uint32_t addr)
{
    if (ivars->stopping || !ivars->outPipe || !ivars->outBuf) return kIOReturnNotReady;
    IOAddressSegment seg = {};
    kern_return_t ret = ivars->outBuf->GetAddressRange(&seg);
    if (ret != kIOReturnSuccess) return ret;
    uint8_t *p = reinterpret_cast<uint8_t *>(seg.address);
    p[0] = ++ivars->seq; p[1] = kOpReadQuad; p[2] = 0; p[3] = 0;
    put32(p + 4, addr);
    return ivars->outPipe->AsyncIO(ivars->outBuf, 8, ivars->writeAction, 0);
}

kern_return_t MicroBookInterface::SendWriteQuad(uint32_t addr, uint32_t data)
{
    if (ivars->stopping || !ivars->outPipe || !ivars->outBuf) return kIOReturnNotReady;
    IOAddressSegment seg = {};
    kern_return_t ret = ivars->outBuf->GetAddressRange(&seg);
    if (ret != kIOReturnSuccess) return ret;
    uint8_t *p = reinterpret_cast<uint8_t *>(seg.address);
    p[0] = ++ivars->seq; p[1] = kOpWriteQuad; p[2] = 0; p[3] = 0;
    put32(p + 4, addr);
    put32(p + 8, data);
    return ivars->outPipe->AsyncIO(ivars->outBuf, 12, ivars->writeAction, 0);
}

// Linear startup script. Each reply advances to the next step.
//   0 read serial  1 read id  [2 write mode  3 read lock  4 start isoc]  (2..4 only in isoc test)
void MicroBookInterface::NextStep()
{
    switch (ivars->step) {
    case 1:
        PostRead(); SendReadQuad(kRegId); break;
    case 2:
        if (!ivars->runIsoc) { LOG("milestone 1 complete: command channel works"); PostRead(); break; }
        // Original driver (0x32 model): writes 1 here when the previous rate was not 48 kHz; its
        // initial "previous rate" is 44.1 kHz, so the very first write is 1.
        LOG("writing mode register 0x%03x = %u", kRegMode, ivars->modeValue);
        PostRead(); SendWriteQuad(kRegMode, ivars->modeValue); break;
    case 3:
        PostRead(); SendReadQuad(kRegLock); break;
    case 4:
        PostRead();
#ifdef MB_AUDIO
        ivars->hwRate = ivars->rate;      // the script above wrote the mode register for this rate
        {
            kern_return_t r = CreateAudioDevice();
            if (r != kIOReturnSuccess) LOG("CreateAudioDevice failed 0x%x", r);
        }
#else
        {
            kern_return_t r = StartIsoc();
            if (r != kIOReturnSuccess) LOG("StartIsoc failed 0x%x", r);
        }
#endif
        break;
#ifdef MB_AUDIO
    case 21:
        ivars->hwRate = ivars->rate;
        PostRead();
        {
            kern_return_t r = StartIsoc();
            LOG("rate switch done (%u Hz) -> StartIsoc 0x%x", ivars->rate, r);
        }
        break;
#endif
    default:
        PostRead(); break;
    }
}

void IMPL(MicroBookInterface, WriteComplete)
{
    if (status != kIOReturnSuccess) LOG("write complete status 0x%x (%u bytes)", status, actualByteCount);
}

void IMPL(MicroBookInterface, ReadComplete)
{
    if (ivars->stopping || status != kIOReturnSuccess) {
        if (status != kIOReturnAborted) LOG("read complete status 0x%x", status);
        return;
    }
    IOAddressSegment seg = {};
    if (ivars->inBuf->GetAddressRange(&seg) != kIOReturnSuccess) return;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(seg.address);
    auto be32 = [](const uint8_t *q) { return (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 8) | q[3]; };

    if (actualByteCount >= 12 && p[1] == kRspReadQuad) {
        LOG("read response seq=%u status=%u addr=0x%08x value=0x%08x", p[0], p[2], be32(p + 4), be32(p + 8));
        ivars->step++;
        NextStep();
        return;
    }
    if (actualByteCount >= 8 && p[1] == kRspWriteQuad) {
        LOG("write response seq=%u status=%u addr=0x%08x", p[0], p[2], be32(p + 4));
        ivars->step++;
        NextStep();
        return;
    }
    if (actualByteCount >= 12 && p[1] == 0x00) {
        LOG("device notice: addr=0x%08x value=0x%08x", be32(p + 4), be32(p + 8));
        PostRead();
        return;
    }
    LOG("unknown packet len=%u: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", actualByteCount,
        p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]);
    PostRead();
}

// ===== isochronous streaming test ==================================================================

static void initFrameList(IOBufferMemoryDescriptor *fl, uint32_t count, uint32_t requestCount)
{
    IOAddressSegment seg = {};
    if (fl->GetAddressRange(&seg) != kIOReturnSuccess) return;
    auto f = reinterpret_cast<IOUSBIsochronousFrame *>(seg.address);
    for (uint32_t i = 0; i < count; i++) {
        f[i].status = 0; f[i].requestCount = requestCount; f[i].completeCount = 0;
        f[i].reserved = 0; f[i].timeStamp = 0;
    }
}


// Fill one OUT request: 441 stereo samples (24-bit big-endian) of a quiet 440 Hz sine, and set the
// per-frame lengths to the 44.1 kHz pattern (45 samples, then 9 x 44 samples).
static void fillTone(IOBufferMemoryDescriptor *data, IOBufferMemoryDescriptor *frames, uint32_t &pos, uint32_t rate)
{
    IOAddressSegment dseg = {}, fseg = {};
    if (data->GetAddressRange(&dseg) != kIOReturnSuccess || frames->GetAddressRange(&fseg) != kIOReturnSuccess) return;
    auto d = reinterpret_cast<uint8_t *>(dseg.address);
    auto f = reinterpret_cast<IOUSBIsochronousFrame *>(fseg.address);
    const double kTwoPi = 6.283185307179586, kFreq = 440.0, kRate = double(rate), kLevel = 0.01;   // -40 dBFS
    uint32_t n = 0, offset = 0;
    for (uint32_t fr = 0; fr < kOutFrames; fr++) {
        uint32_t samples = samplesInFrame(rate, fr);
        f[fr].status = 0; f[fr].requestCount = samples * 6; f[fr].completeCount = 0; f[fr].reserved = 0; f[fr].timeStamp = 0;
        // IMPORTANT: the host controller consumes the data buffer frame after frame with NO padding
        // (frame k starts where frame k-1's requestCount ended), so pack the frames back-to-back.
        uint8_t *dst = d + offset;
        offset += samples * 6;
        for (uint32_t i = 0; i < samples; i++, n++) {
            double ph = kTwoPi * kFreq * double(pos % rate) / kRate;
            pos++;
            int32_t v = int32_t(__builtin_sin(ph) * kLevel * 8388607.0);
            for (int ch = 0; ch < 2; ch++) {           // left and right
                // 24-bit LITTLE-endian (low byte first). The old driver's "ClipSamplesToBigEndian" actually
                // writes low byte first; sending big-endian produced a loud buzz.
                dst[i * 6 + ch * 3 + 0] = v & 0xff;
                dst[i * 6 + ch * 3 + 1] = (v >> 8) & 0xff;
                dst[i * 6 + ch * 3 + 2] = (v >> 16) & 0xff;
            }
        }
    }
}

#ifdef MB_AUDIO
// Copy the next 441 frames (44.1 kHz pattern 45,44 x9) from the Core Audio OUT ring into one request.
static void fillFromRing(MicroBookInterface_IVars *iv, IOBufferMemoryDescriptor *data, IOBufferMemoryDescriptor *frames)
{
    IOAddressSegment dseg = {}, fseg = {};
    if (data->GetAddressRange(&dseg) != kIOReturnSuccess || frames->GetAddressRange(&fseg) != kIOReturnSuccess) return;
    auto d = reinterpret_cast<uint8_t *>(dseg.address);
    auto f = reinterpret_cast<IOUSBIsochronousFrame *>(fseg.address);
    // Software volume: the device has no output level we can reach, so scale the samples here.
    float gain = 1.0f;
    if (iv->volume) {
        float db = iv->volume->GetDecibelValue();
        if (db <= -80.0f) gain = 0.0f; else if (db < 0.0f) gain = __builtin_expf(db * 0.11512925f);   // 10^(db/20)
    }
    uint32_t offset = 0;
    for (uint32_t fr = 0; fr < kOutFrames; fr++) {
        uint32_t samples = samplesInFrame(iv->rate, fr);
        f[fr].status = 0; f[fr].requestCount = samples * kOutBytesFrame; f[fr].completeCount = 0; f[fr].reserved = 0; f[fr].timeStamp = 0;
        uint8_t *dst = d + offset;      // frames are packed back-to-back (no per-frame padding)
        offset += samples * kOutBytesFrame;
        for (uint32_t i = 0; i < samples; i++) {
            if (iv->outRingPtr) {
                uint32_t idx = uint32_t((iv->nextOutSample + i) % kRingFrames);
                memcpy(dst + i * kOutBytesFrame, iv->outRingPtr + idx * kOutBytesFrame, kOutBytesFrame);
                if (gain != 1.0f) {
                    for (int ch = 0; ch < 2; ch++) {      // 24-bit little-endian, signed
                        uint8_t *q = dst + i * kOutBytesFrame + ch * 3;
                        int32_t v = (int32_t(q[2]) << 16) | (int32_t(q[1]) << 8) | q[0];
                        if (v & 0x800000) v -= 0x1000000;
                        v = int32_t(float(v) * gain);
                        q[0] = v & 0xff; q[1] = (v >> 8) & 0xff; q[2] = (v >> 16) & 0xff;
                    }
                }
            } else {
                memset(dst + i * kOutBytesFrame, 0, kOutBytesFrame);
            }
        }
        iv->nextOutSample += samples;
    }
}
#endif

static void fillOut(MicroBookInterface_IVars *iv, IOBufferMemoryDescriptor *data, IOBufferMemoryDescriptor *frames)
{
#ifdef MB_AUDIO
    fillFromRing(iv, data, frames);
#else
    fillTone(data, frames, iv->tonePos, iv->rate);
#endif
}

kern_return_t MicroBookInterface::StartIsoc()
{
    if (ivars->isocRunning) return kIOReturnSuccess;
    kern_return_t ret;
    auto intf = ivars->interface;

    if (!ivars->isoOutPipe) {
        ret = intf->CopyPipe(kEpIsoOut, &ivars->isoOutPipe);
        if (ret != kIOReturnSuccess) { LOG("CopyPipe(iso out 0x%02x) failed 0x%x", kEpIsoOut, ret); return ret; }
    }
    if (!ivars->isoInPipe) {
        ret = intf->CopyPipe(kEpIsoIn, &ivars->isoInPipe);
        if (ret != kIOReturnSuccess) { LOG("CopyPipe(iso in 0x%02x) failed 0x%x", kEpIsoIn, ret); return ret; }
    }

    for (uint32_t i = 0; i < kNumUrbs; i++) {
        Urb &o = ivars->outUrb[i], &n = ivars->inUrb[i];
        if (o.data) continue;   // already allocated by an earlier run
        if ((ret = intf->CreateIOBuffer(kIOMemoryDirectionInOut, kOutFrames * kOutFrameLen, &o.data)) != kIOReturnSuccess) return ret;
        if ((ret = intf->CreateIOBuffer(kIOMemoryDirectionInOut, kOutFrames * sizeof(IOUSBIsochronousFrame), &o.frames)) != kIOReturnSuccess) return ret;
        if ((ret = intf->CreateIOBuffer(kIOMemoryDirectionInOut, kInFrames * kInFrameMax, &n.data)) != kIOReturnSuccess) return ret;
        if ((ret = intf->CreateIOBuffer(kIOMemoryDirectionInOut, kInFrames * sizeof(IOUSBIsochronousFrame), &n.frames)) != kIOReturnSuccess) return ret;
        if ((ret = CreateActionIsochOutComplete(sizeof(uint32_t), &o.action)) != kIOReturnSuccess) return ret;
        if ((ret = CreateActionIsochInComplete(sizeof(uint32_t), &n.action)) != kIOReturnSuccess) return ret;
        *reinterpret_cast<uint32_t *>(o.action->GetReference()) = i;
        *reinterpret_cast<uint32_t *>(n.action->GetReference()) = i;
    }

    uint64_t frame = 0, when = 0;
    ret = intf->GetFrameNumber(&frame, &when);
    if (ret != kIOReturnSuccess) { LOG("GetFrameNumber failed 0x%x", ret); return ret; }
    uint64_t base = frame + 40;   // lead time
    LOG("starting isoc: current frame %llu, first scheduled frame %llu", frame, base);

    ivars->nextOutFrame = base;
    ivars->nextInFrame  = base;
    ivars->isocRunning = true;
    ivars->inCompletions = 0;

    for (uint32_t i = 0; i < kNumUrbs; i++) {
        Urb &o = ivars->outUrb[i];
        fillOut(ivars, o.data, o.frames);
        ret = ivars->isoOutPipe->IsochIO(o.data, o.frames, ivars->nextOutFrame, o.action);
        if (ret != kIOReturnSuccess) { LOG("IsochIO(out %u) failed 0x%x", i, ret); StopIsoc("start failed"); return ret; }
        ivars->nextOutFrame += kOutFrames;
    }
    for (uint32_t i = 0; i < kNumUrbs; i++) {
        Urb &n = ivars->inUrb[i];
        initFrameList(n.frames, kInFrames, kInFrameMax);
        ret = ivars->isoInPipe->IsochIO(n.data, n.frames, ivars->nextInFrame, n.action);
        if (ret != kIOReturnSuccess) { LOG("IsochIO(in %u) failed 0x%x", i, ret); StopIsoc("start failed"); return ret; }
        ivars->nextInFrame += kInFrames;
    }
    LOG("isoc running: %u OUT requests x %u frames, %u IN requests x %u frames queued", kNumUrbs, kOutFrames, kNumUrbs, kInFrames);
    return kIOReturnSuccess;
}

void MicroBookInterface::StopIsoc(const char *why)
{
    bool was = ivars->isocRunning;
    ivars->isocRunning = false;
    if (ivars->isoOutPipe) ivars->isoOutPipe->Abort(0, kIOReturnAborted, this);
    if (ivars->isoInPipe)  ivars->isoInPipe->Abort(0, kIOReturnAborted, this);
    if (was) LOG("isoc stopped: %{public}s", why);
    // buffers/actions are released in Stop() after the pipes are idle
    if (ivars->stopping) {
        for (uint32_t i = 0; i < kNumUrbs; i++) {
            OSSafeReleaseNULL(ivars->outUrb[i].action); OSSafeReleaseNULL(ivars->outUrb[i].data); OSSafeReleaseNULL(ivars->outUrb[i].frames);
            OSSafeReleaseNULL(ivars->inUrb[i].action);  OSSafeReleaseNULL(ivars->inUrb[i].data);  OSSafeReleaseNULL(ivars->inUrb[i].frames);
        }
        OSSafeReleaseNULL(ivars->isoOutPipe);
        OSSafeReleaseNULL(ivars->isoInPipe);
    }
}

void IMPL(MicroBookInterface, IsochOutComplete)
{
    if (!ivars->isocRunning || ivars->stopping) return;
    uint32_t i = *reinterpret_cast<uint32_t *>(action->GetReference());
    if (i >= kNumUrbs) return;
    if (status == kIOReturnAborted) return;
    if (status != kIOReturnSuccess) { LOG("OUT request %u completed with 0x%x", i, status); StopIsoc("OUT error"); return; }
#ifdef MB_AUDIO
    {   // device finished playing one request: advance the Core Audio clock
        uint64_t now = mach_absolute_time();
        if (ivars->playedSamples == 0) {
            ivars->playStartHost = now - uint64_t(ivars->outSamplesPerUrb * ivars->ticksPerSample);
            if (ivars->audioDevice) ivars->audioDevice->UpdateCurrentZeroTimestamp(0, ivars->playStartHost);
            ivars->nextZts = kRingFrames;
        }
        ivars->playedSamples += ivars->outSamplesPerUrb;
        while (ivars->playedSamples >= ivars->nextZts) {
            if (ivars->audioDevice)
                ivars->audioDevice->UpdateCurrentZeroTimestamp(ivars->nextZts,
                    ivars->playStartHost + uint64_t(double(ivars->nextZts) * ivars->ticksPerSample));
            ivars->nextZts += kRingFrames;
        }
    }
#endif
    Urb &o = ivars->outUrb[i];
    fillOut(ivars, o.data, o.frames);
    kern_return_t ret = ivars->isoOutPipe->IsochIO(o.data, o.frames, ivars->nextOutFrame, o.action);
    if (ret != kIOReturnSuccess) { LOG("IsochIO(out resubmit) failed 0x%x", ret); StopIsoc("OUT resubmit failed"); return; }
    ivars->nextOutFrame += kOutFrames;
}

void IMPL(MicroBookInterface, IsochInComplete)
{
    if (!ivars->isocRunning || ivars->stopping) return;
    uint32_t i = *reinterpret_cast<uint32_t *>(action->GetReference());
    if (i >= kNumUrbs) return;
    if (status == kIOReturnAborted) return;
    if (status != kIOReturnSuccess) { LOG("IN request %u completed with 0x%x", i, status); StopIsoc("IN error"); return; }
    Urb &n = ivars->inUrb[i];

    IOAddressSegment fseg = {}, dseg = {};
    if (n.frames->GetAddressRange(&fseg) == kIOReturnSuccess && n.data->GetAddressRange(&dseg) == kIOReturnSuccess) {
        auto f = reinterpret_cast<const IOUSBIsochronousFrame *>(fseg.address);
        auto d = reinterpret_cast<const uint8_t *>(dseg.address);
        for (uint32_t k = 0; k < kInFrames; k++) {
            uint32_t len = f[k].completeCount;
            if (f[k].status != kIOReturnSuccess) ivars->stErrFrames++;
#ifdef MB_AUDIO
            if (ivars->inRingPtr && f[k].status == kIOReturnSuccess) {
                const uint8_t *src = d + k * kInFrameMax;
                for (uint32_t off = 0; off + kInBytesFrame <= len; off += kInBytesFrame) {
                    uint32_t idx = uint32_t(ivars->inStoreSample % kRingFrames);
                    memcpy(ivars->inRingPtr + idx * kInBytesFrame, src + off, kInBytesFrame);
                    ivars->inStoreSample++;
                }
            }
#endif
            ivars->stFrames++; ivars->stBytes += len;
            if (len < ivars->stMinLen) ivars->stMinLen = len;
            if (len > ivars->stMaxLen) ivars->stMaxLen = len;
            const uint8_t *s = d + k * kInFrameMax;
            for (uint32_t off = 0; off + 3 * kInChannels <= len; off += 3 * kInChannels) {
                for (uint32_t c = 0; c < kInChannels; c++) {
                    // Samples are 24-bit LITTLE-endian in both directions.
                    int32_t v = (int32_t(s[off + 3 * c + 2]) << 16) | (int32_t(s[off + 3 * c + 1]) << 8) | s[off + 3 * c];
                    if (v & 0x800000) v -= 0x1000000;
                    if (v < 0) v = -v;
                    if (v > ivars->stPeak[c]) ivars->stPeak[c] = v;
                }
            }
        }
    }

    ivars->inCompletions++;
    if (ivars->inCompletions % kStatsEvery == 1 && ivars->inCompletions < 1000) {
        IOAddressSegment fs2 = {}, ds2 = {};
        if (n.frames->GetAddressRange(&fs2) == kIOReturnSuccess && n.data->GetAddressRange(&ds2) == kIOReturnSuccess) {
            auto ff = reinterpret_cast<const IOUSBIsochronousFrame *>(fs2.address);
            auto dd = reinterpret_cast<const uint8_t *>(ds2.address);
            LOG("RAW IN frame0 len=%u: %02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x | %02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x | %02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x",
                ff[0].completeCount,
                dd[0], dd[1], dd[2], dd[3], dd[4], dd[5], dd[6], dd[7], dd[8], dd[9], dd[10], dd[11],
                dd[12], dd[13], dd[14], dd[15], dd[16], dd[17], dd[18], dd[19], dd[20], dd[21], dd[22], dd[23],
                dd[24], dd[25], dd[26], dd[27], dd[28], dd[29], dd[30], dd[31], dd[32], dd[33], dd[34], dd[35]);
        }
    }
    if (ivars->inCompletions % kStatsEvery == 0) {
        LOG("IN: %u frames, %u bytes, frame len %u..%u, bad frames %u, peak ch1..4 = %d %d %d %d (of 8388607)",
            ivars->stFrames, ivars->stBytes, ivars->stMinLen == 0xffff ? 0 : ivars->stMinLen, ivars->stMaxLen, ivars->stErrFrames,
            ivars->stPeak[0], ivars->stPeak[1], ivars->stPeak[2], ivars->stPeak[3]);
        ivars->stFrames = ivars->stBytes = ivars->stErrFrames = 0; ivars->stMinLen = 0xffff; ivars->stMaxLen = 0;
        for (auto &pk : ivars->stPeak) pk = 0;
    }
    if (!kAudioBuild && ivars->inCompletions >= kStopAfterIn) { StopIsoc("test finished (10 s)"); return; }

    initFrameList(n.frames, kInFrames, kInFrameMax);
    kern_return_t ret = ivars->isoInPipe->IsochIO(n.data, n.frames, ivars->nextInFrame, n.action);
    if (ret != kIOReturnSuccess) { LOG("IsochIO(in resubmit) failed 0x%x", ret); StopIsoc("IN resubmit failed"); return; }
    ivars->nextInFrame += kInFrames;
}


// ===== Core Audio layer (AudioDriverKit) - only in the MB_AUDIO flavour =============================
#ifdef MB_AUDIO

static OSSharedPtr<OSString> mkStr(const char *c) { return OSSharedPtr(OSString::withCString(c), OSNoRetain); }

// Core Audio asked for a new sample rate (called from MicroBookAudioDevice::HandleChangeSampleRate).
// Only records the new rate and updates the stream formats; the hardware register is written when IO starts.
static kern_return_t mbRateCallback(void *ctx, double rate)
{
    auto iv = static_cast<MicroBookInterface_IVars *>(ctx);
    uint32_t nr = uint32_t(rate + 0.5);
    if (!iv || (nr != 44100 && nr != 48000)) return kIOReturnUnsupported;
    iv->rate = nr;
    iv->outSamplesPerUrb = (nr == 48000) ? 480 : 441;
    iv->modeValue = (nr == 48000) ? 0 : 1;          // verified: 0 = 48 kHz, 1 = 44.1 kHz
    iv->ticksPerSample = (1.0e9 / double(nr)) * iv->tbDenom / iv->tbNumer;
    if (iv->outStream) iv->outStream->DeviceSampleRateChanged(rate);
    if (iv->inStream)  iv->inStream->DeviceSampleRateChanged(rate);
    os_log(OS_LOG_DEFAULT, "MicroBookInterface: sample rate -> %u", nr);
    return kIOReturnSuccess;
}

kern_return_t MicroBookInterface::CreateAudioDevice()
{
    if (ivars->audioDevice) return kIOReturnSuccess;
    kern_return_t ret;

    mach_timebase_info_data_t tb = {};
    mach_timebase_info(&tb);
    ivars->tbNumer = double(tb.numer); ivars->tbDenom = double(tb.denom);
    ivars->ticksPerSample = (1.0e9 / double(ivars->rate)) * double(tb.denom) / double(tb.numer);

    auto devName = mkStr("MOTU MicroBook");
    SetName(devName.get());
    SetTransportType(IOUserAudioTransportType::USB);

    auto uid = mkStr("MOTU-MicroBook-07fd-0004");
    auto model = mkStr("MicroBook");
    auto mfr = mkStr("MOTU");
    auto dev = MicroBookAudioDevice::Create(this, false, uid.get(), model.get(), mfr.get(), kRingFrames);
    if (!dev) return kIOReturnNoMemory;
    dev->SetRateCallback(reinterpret_cast<void *>(mbRateCallback), ivars);
    dev->SetName(devName.get());

    // Mode register 0xB14: 1 = 44.1 kHz, 0 = 48 kHz (both verified on hardware).
    const double rates[] = { 44100.0, 48000.0 };
    dev->SetAvailableSampleRates(rates, 2);
    dev->SetSampleRate(double(ivars->rate));
    dev->SetTransportType(IOUserAudioTransportType::USB);
    dev->SetClockIsStable(true);

    // Output is read from the ring kQueueAheadFrames before it is played; tell Core Audio.
    dev->SetOutputLatency(kQueueAheadFrames);
    dev->SetOutputSafetyOffset(kQueueAheadFrames + 441);
    dev->SetInputLatency(221);            // ~5 ms: IN requests carry 5 frames
    dev->SetInputSafetyOffset(512);

    // Shared ring buffers (their layout is exactly the USB wire format, so the USB side just copies)
    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, kRingFrames * kOutBytesFrame, 0, &ivars->outRing);
    if (ret != kIOReturnSuccess) return ret;
    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, kRingFrames * kInBytesFrame, 0, &ivars->inRing);
    if (ret != kIOReturnSuccess) return ret;
    IOAddressSegment seg = {};
    if (ivars->outRing->GetAddressRange(&seg) == kIOReturnSuccess) { ivars->outRingPtr = reinterpret_cast<uint8_t *>(seg.address); bzero(ivars->outRingPtr, kRingFrames * kOutBytesFrame); }
    if (ivars->inRing->GetAddressRange(&seg) == kIOReturnSuccess)  { ivars->inRingPtr  = reinterpret_cast<uint8_t *>(seg.address); bzero(ivars->inRingPtr,  kRingFrames * kInBytesFrame); }

    // Output stream: stereo, 24-bit packed signed integer, LITTLE-endian (verified: same as the input direction)
    IOUserAudioStreamBasicDescription outFmt = {};
    outFmt.mSampleRate = double(ivars->rate);
    outFmt.mFormatID = IOUserAudioFormatID::LinearPCM;
    outFmt.mFormatFlags = static_cast<IOUserAudioFormatFlags>(FormatFlagIsSignedInteger | FormatFlagIsPacked);
    outFmt.mBytesPerPacket = kOutBytesFrame; outFmt.mFramesPerPacket = 1; outFmt.mBytesPerFrame = kOutBytesFrame;
    outFmt.mChannelsPerFrame = 2; outFmt.mBitsPerChannel = 24;
    auto outStream = IOUserAudioStream::Create(this, IOUserAudioStreamDirection::Output, ivars->outRing);
    if (!outStream) return kIOReturnNoMemory;
    auto outName = mkStr("Main Output");
    outStream->SetName(outName.get());
    outStream->SetTerminalType(IOUserAudioStreamTerminalType::Speaker);
    IOUserAudioStreamBasicDescription outFmts[2] = { outFmt, outFmt };
    outFmts[0].mSampleRate = 44100.0; outFmts[1].mSampleRate = 48000.0;
    outStream->SetAvailableStreamFormats(outFmts, 2);
    outStream->SetCurrentStreamFormat(&outFmt);
    if ((ret = dev->AddStream(outStream.get())) != kIOReturnSuccess) return ret;
    outStream->retain(); ivars->outStream = outStream.get();

    // Input stream: 4 channels, 24-bit packed signed integer, LITTLE-endian
    IOUserAudioStreamBasicDescription inFmt = {};
    inFmt.mSampleRate = double(ivars->rate);
    inFmt.mFormatID = IOUserAudioFormatID::LinearPCM;
    inFmt.mFormatFlags = static_cast<IOUserAudioFormatFlags>(FormatFlagIsSignedInteger | FormatFlagIsPacked);
    inFmt.mBytesPerPacket = kInBytesFrame; inFmt.mFramesPerPacket = 1; inFmt.mBytesPerFrame = kInBytesFrame;
    inFmt.mChannelsPerFrame = 4; inFmt.mBitsPerChannel = 24;
    auto inStream = IOUserAudioStream::Create(this, IOUserAudioStreamDirection::Input, ivars->inRing);
    if (!inStream) return kIOReturnNoMemory;
    auto inName = mkStr("Inputs 1-4");
    inStream->SetName(inName.get());
    inStream->SetTerminalType(IOUserAudioStreamTerminalType::Line);
    IOUserAudioStreamBasicDescription inFmts[2] = { inFmt, inFmt };
    inFmts[0].mSampleRate = 44100.0; inFmts[1].mSampleRate = 48000.0;
    inStream->SetAvailableStreamFormats(inFmts, 2);
    inStream->SetCurrentStreamFormat(&inFmt);
    if ((ret = dev->AddStream(inStream.get())) != kIOReturnSuccess) return ret;
    inStream->retain(); ivars->inStream = inStream.get();

    // Output volume slider: -80 dB .. 0 dB, starts at -24 dB.
    IOUserAudioLevelControlRange volRange = { -80.0f, 0.0f };
    auto vol = IOUserAudioLevelControl::Create(this, true, -24.0f, volRange,
                                               IOUserAudioObjectPropertyElementMain,
                                               IOUserAudioObjectPropertyScope::Output,
                                               IOUserAudioClassID::VolumeControl);
    if (vol) {
        if (dev->AddControl(vol.get()) == kIOReturnSuccess) { vol->retain(); ivars->volume = vol.get(); }
    }

    // All audio data moves through the shared rings; nothing to convert per IO operation.
    dev->SetIOOperationHandler(^kern_return_t(IOUserAudioObjectID, IOUserAudioIOOperation, uint32_t, uint64_t, uint64_t) {
        return kIOReturnSuccess;
    });

    if ((ret = AddObject(dev.get())) != kIOReturnSuccess) return ret;
    dev->retain();
    ivars->audioDevice = dev.get();
    ret = RegisterService();
    LOG("Core Audio device published (RegisterService -> 0x%x)", ret);
    return ret;
}

kern_return_t MicroBookInterface::StartDevice(IOUserAudioObjectID in_object_id, IOUserAudioStartStopFlags in_flags)
{
    kern_return_t ret = super::StartDevice(in_object_id, in_flags);
    if (ret != kIOReturnSuccess) return ret;
    ivars->nextOutSample = 0; ivars->inStoreSample = 0; ivars->playedSamples = 0; ivars->nextZts = 0;
    if (ivars->outRingPtr) bzero(ivars->outRingPtr, kRingFrames * kOutBytesFrame);
    if (ivars->inRingPtr)  bzero(ivars->inRingPtr,  kRingFrames * kInBytesFrame);
    if (ivars->hwRate != ivars->rate) {
        // Switch the hardware rate first (step 20); the write acknowledgement (step 21) starts the streams.
        LOG("StartDevice: switching hardware to %u Hz (mode register = %u)", ivars->rate, ivars->modeValue);
        ivars->step = 20;
        PostRead();
        ret = SendWriteQuad(kRegMode, ivars->modeValue);
        if (ret != kIOReturnSuccess) { LOG("rate switch write failed 0x%x", ret); super::StopDevice(in_object_id, in_flags); }
        return ret;
    }
    ret = StartIsoc();
    LOG("StartDevice -> StartIsoc 0x%x", ret);
    if (ret != kIOReturnSuccess) super::StopDevice(in_object_id, in_flags);
    return ret;
}

kern_return_t MicroBookInterface::StopDevice(IOUserAudioObjectID in_object_id, IOUserAudioStartStopFlags in_flags)
{
    StopIsoc("Core Audio stopped the device");
    return super::StopDevice(in_object_id, in_flags);
}

#endif // MB_AUDIO
