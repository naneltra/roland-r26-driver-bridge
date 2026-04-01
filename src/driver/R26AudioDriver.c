// R26AudioDriver.c — CoreAudio AudioServerPlugIn for Roland R-26
//
// This plugin reads audio data from shared memory (written by the r26d daemon)
// and presents it to macOS as a virtual audio input device named "Roland R-26".
//
// Object hierarchy:
//   Plugin (ID=1) -> Device (ID=2) -> Stream Input (ID=3)
//                                  -> Volume Control (ID=4)

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <os/log.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <math.h>

#include "../shared/RingBuffer.h"

// ============================================================================
#pragma mark - Constants & Object IDs
// ============================================================================

#define kPlugInObjectID         ((AudioObjectID)1)  // kAudioObjectPlugInObject
#define kDeviceObjectID         ((AudioObjectID)2)
#define kInputStreamObjectID    ((AudioObjectID)3)
#define kVolumeControlObjectID  ((AudioObjectID)4)

#define kDeviceName             "Roland R-26"
#define kDeviceManufacturer     "Roland"
#define kDeviceUID              "R26USBBridge"
#define kDeviceModelUID         "RolandR26"

#define kNumChannels            2
#define kBitsPerChannel         32      // Float32
#define kBytesPerFrame          (kNumChannels * sizeof(Float32))
#define kDefaultSampleRate      48000.0
#define kRingBufferFrames       (48000 * 2) // must match R26_RING_FRAMES

// Zero time stamp period: host expects this many frames between timestamps.
// We use 512 frames as our nominal IO buffer size
#define kBufferFrameSize        512
#define kZeroTimestampPeriod    kBufferFrameSize

// ============================================================================
#pragma mark - Driver State
// ============================================================================

typedef struct {
    // Plugin interface vtable (must be first field)
    AudioServerPlugInDriverInterface    **interface;

    // Reference counting
    UInt32                              refCount;
    pthread_mutex_t                     mutex;

    // Host interface
    AudioServerPlugInHostRef            host;

    // Device state
    Float64                             sampleRate;
    bool                                ioRunning;
    UInt32                              ioClientCount;

    // Clock state
    Float64                             hostTicksPerFrame;
    UInt64                              anchorHostTime;
    Float64                             anchorSampleTime;
    UInt64                              clockSeed;

    // Volume
    Float32                             volumeL;
    Float32                             volumeR;
    bool                                muted;

    // Shared memory
    R26SharedAudio                      *shm;
    int                                 shmFd;
    bool                                shmConnected;
} R26DriverState;

static R26DriverState gDriverState = {0};

static os_log_t gLog;
#define LOG(fmt, ...) os_log(gLog, "R26Audio: " fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) os_log_error(gLog, "R26Audio: " fmt, ##__VA_ARGS__)

// ============================================================================
#pragma mark - Shared Memory
// ============================================================================

static void shm_connect(void) {
    if (gDriverState.shmConnected) return;

    int fd = shm_open(R26_SHM_NAME, O_RDWR, 0666);
    if (fd < 0) {
        // Not an error - daemon may not be running yet
        return;
    }

    void *ptr = mmap(NULL, R26_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        return;
    }

    gDriverState.shm = (R26SharedAudio *)ptr;
    gDriverState.shmFd = fd;
    gDriverState.shmConnected = true;
    LOG("Connected to shared memory");
}

static void shm_disconnect(void) {
    if (!gDriverState.shmConnected) return;
    if (gDriverState.shm) {
        munmap(gDriverState.shm, R26_SHM_SIZE);
        gDriverState.shm = NULL;
    }
    if (gDriverState.shmFd >= 0) {
        close(gDriverState.shmFd);
        gDriverState.shmFd = -1;
    }
    gDriverState.shmConnected = false;
}

// ============================================================================
#pragma mark - Utility
// ============================================================================

static void compute_host_ticks_per_frame(void) {
    mach_timebase_info_data_t tbi;
    mach_timebase_info(&tbi);
    // host ticks per second = 1e9 * tbi.denom / tbi.numer
    Float64 hostTicksPerSecond = 1e9 * (Float64)tbi.denom / (Float64)tbi.numer;
    gDriverState.hostTicksPerFrame = hostTicksPerSecond / gDriverState.sampleRate;
}

// ============================================================================
#pragma mark - IUnknown
// ============================================================================

static HRESULT R26_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface) {
    CFUUIDRef requestedUUID = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    CFUUIDRef driverInterfaceUUID = kAudioServerPlugInDriverInterfaceUUID;
    CFUUIDRef iUnknownUUID = CFUUIDGetConstantUUIDWithBytes(NULL,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46); // IUnknownUUID

    if (CFEqual(requestedUUID, driverInterfaceUUID) || CFEqual(requestedUUID, iUnknownUUID)) {
        CFRelease(requestedUUID);
        pthread_mutex_lock(&gDriverState.mutex);
        gDriverState.refCount++;
        pthread_mutex_unlock(&gDriverState.mutex);
        *outInterface = gDriverState.interface;
        return kAudioHardwareNoError;
    }

    CFRelease(requestedUUID);
    *outInterface = NULL;
    return E_NOINTERFACE;
}

static ULONG R26_AddRef(void *inDriver) {
    (void)inDriver;
    pthread_mutex_lock(&gDriverState.mutex);
    UInt32 rc = ++gDriverState.refCount;
    pthread_mutex_unlock(&gDriverState.mutex);
    return rc;
}

static ULONG R26_Release(void *inDriver) {
    (void)inDriver;
    pthread_mutex_lock(&gDriverState.mutex);
    UInt32 rc = --gDriverState.refCount;
    pthread_mutex_unlock(&gDriverState.mutex);
    return rc;
}

// ============================================================================
#pragma mark - Basic Operations
// ============================================================================

static OSStatus R26_Initialize(AudioServerPlugInDriverRef inDriver,
                                AudioServerPlugInHostRef inHost) {
    (void)inDriver;
    gDriverState.host = inHost;
    gDriverState.sampleRate = kDefaultSampleRate;
    gDriverState.volumeL = 1.0f;
    gDriverState.volumeR = 1.0f;
    gDriverState.muted = false;
    gDriverState.clockSeed = 1;
    gDriverState.shmFd = -1;

    compute_host_ticks_per_frame();
    shm_connect();

    LOG("Initialized (sample rate: %.0f)", gDriverState.sampleRate);
    return kAudioHardwareNoError;
}

static OSStatus R26_CreateDevice(AudioServerPlugInDriverRef inDriver,
                                  CFDictionaryRef inDescription,
                                  const AudioServerPlugInClientInfo *inClientInfo,
                                  AudioObjectID *outDeviceObjectID) {
    (void)inDriver; (void)inDescription; (void)inClientInfo; (void)outDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus R26_DestroyDevice(AudioServerPlugInDriverRef inDriver,
                                   AudioObjectID inDeviceObjectID) {
    (void)inDriver; (void)inDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus R26_AddDeviceClient(AudioServerPlugInDriverRef inDriver,
                                     AudioObjectID inDeviceObjectID,
                                     const AudioServerPlugInClientInfo *inClientInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientInfo;
    return kAudioHardwareNoError;
}

static OSStatus R26_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver,
                                        AudioObjectID inDeviceObjectID,
                                        const AudioServerPlugInClientInfo *inClientInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientInfo;
    return kAudioHardwareNoError;
}

static OSStatus R26_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                      AudioObjectID inDeviceObjectID,
                                                      UInt64 inChangeAction,
                                                      void *inChangeInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inChangeAction; (void)inChangeInfo;
    return kAudioHardwareNoError;
}

static OSStatus R26_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                    AudioObjectID inDeviceObjectID,
                                                    UInt64 inChangeAction,
                                                    void *inChangeInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inChangeAction; (void)inChangeInfo;
    return kAudioHardwareNoError;
}

// ============================================================================
#pragma mark - Property Operations
// ============================================================================

static Boolean R26_HasProperty(AudioServerPlugInDriverRef inDriver,
                                AudioObjectID inObjectID,
                                pid_t inClientProcessID,
                                const AudioObjectPropertyAddress *inAddress) {
    (void)inDriver; (void)inClientProcessID;

    switch (inObjectID) {
        case kPlugInObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyManufacturer:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioPlugInPropertyDeviceList:
                case kAudioPlugInPropertyTranslateUIDToDevice:
                case kAudioPlugInPropertyResourceBundle:
                    return true;
            }
            break;

        case kDeviceObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyName:
                case kAudioObjectPropertyManufacturer:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioDevicePropertyDeviceUID:
                case kAudioDevicePropertyModelUID:
                case kAudioDevicePropertyTransportType:
                case kAudioDevicePropertyRelatedDevices:
                case kAudioDevicePropertyClockDomain:
                case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyDeviceIsRunning:
                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                case kAudioDevicePropertyLatency:
                case kAudioDevicePropertyStreams:
                case kAudioObjectPropertyControlList:
                case kAudioDevicePropertyNominalSampleRate:
                case kAudioDevicePropertyAvailableNominalSampleRates:
                case kAudioDevicePropertyZeroTimeStampPeriod:
                case kAudioDevicePropertyIcon:
                case kAudioDevicePropertyIsHidden:
                case kAudioDevicePropertySafetyOffset:
                case kAudioDevicePropertyPreferredChannelsForStereo:
                case kAudioDevicePropertyPreferredChannelLayout:
                    return true;
            }
            break;

        case kInputStreamObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyName:
                case kAudioStreamPropertyIsActive:
                case kAudioStreamPropertyDirection:
                case kAudioStreamPropertyTerminalType:
                case kAudioStreamPropertyStartingChannel:
                case kAudioStreamPropertyLatency:
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat:
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats:
                    return true;
            }
            break;

        case kVolumeControlObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyElementName:
                case kAudioLevelControlPropertyScalarValue:
                case kAudioLevelControlPropertyDecibelValue:
                case kAudioLevelControlPropertyDecibelRange:
                case kAudioObjectPropertyScopeGlobal:
                    return true;
                case kAudioControlPropertyScope:
                case kAudioControlPropertyElement:
                    return true;
            }
            break;
    }
    return false;
}

static OSStatus R26_IsPropertySettable(AudioServerPlugInDriverRef inDriver,
                                        AudioObjectID inObjectID,
                                        pid_t inClientProcessID,
                                        const AudioObjectPropertyAddress *inAddress,
                                        Boolean *outIsSettable) {
    (void)inDriver; (void)inClientProcessID;

    *outIsSettable = false;

    if (inObjectID == kDeviceObjectID && inAddress->mSelector == kAudioDevicePropertyNominalSampleRate) {
        *outIsSettable = true;
    }
    if (inObjectID == kVolumeControlObjectID) {
        if (inAddress->mSelector == kAudioLevelControlPropertyScalarValue ||
            inAddress->mSelector == kAudioLevelControlPropertyDecibelValue) {
            *outIsSettable = true;
        }
    }

    return kAudioHardwareNoError;
}

static OSStatus R26_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver,
                                         AudioObjectID inObjectID,
                                         pid_t inClientProcessID,
                                         const AudioObjectPropertyAddress *inAddress,
                                         UInt32 inQualifierDataSize,
                                         const void *inQualifierData,
                                         UInt32 *outDataSize) {
    (void)inDriver; (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;

    switch (inObjectID) {
        case kPlugInObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioObjectPropertyManufacturer:
                case kAudioPlugInPropertyResourceBundle:
                    *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
                case kAudioObjectPropertyOwnedObjects:
                case kAudioPlugInPropertyDeviceList:
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioPlugInPropertyTranslateUIDToDevice:
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
            }
            break;

        case kDeviceObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioObjectPropertyName:
                case kAudioObjectPropertyManufacturer:
                case kAudioDevicePropertyDeviceUID:
                case kAudioDevicePropertyModelUID:
                    *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
                case kAudioDevicePropertyTransportType:
                case kAudioDevicePropertyClockDomain:
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioDevicePropertyRelatedDevices:
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyDeviceIsRunning:
                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                case kAudioDevicePropertyIsHidden:
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioDevicePropertyLatency:
                case kAudioDevicePropertySafetyOffset:
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioDevicePropertyStreams:
                    if (inAddress->mScope == kAudioObjectPropertyScopeInput ||
                        inAddress->mScope == kAudioObjectPropertyScopeGlobal)
                        *outDataSize = sizeof(AudioObjectID);
                    else
                        *outDataSize = 0;
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyControlList:
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioObjectPropertyOwnedObjects:
                    *outDataSize = 2 * sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioDevicePropertyNominalSampleRate:
                    *outDataSize = sizeof(Float64); return kAudioHardwareNoError;
                case kAudioDevicePropertyAvailableNominalSampleRates:
                    *outDataSize = 6 * sizeof(AudioValueRange); return kAudioHardwareNoError;
                case kAudioDevicePropertyZeroTimeStampPeriod:
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioDevicePropertyIcon:
                    *outDataSize = sizeof(CFURLRef); return kAudioHardwareNoError;
                case kAudioDevicePropertyPreferredChannelsForStereo:
                    *outDataSize = 2 * sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioDevicePropertyPreferredChannelLayout:
                    *outDataSize = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) +
                                   kNumChannels * sizeof(AudioChannelDescription));
                    return kAudioHardwareNoError;
            }
            break;

        case kInputStreamObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioObjectPropertyName:
                    *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
                case kAudioStreamPropertyIsActive:
                case kAudioStreamPropertyDirection:
                case kAudioStreamPropertyTerminalType:
                case kAudioStreamPropertyStartingChannel:
                case kAudioStreamPropertyLatency:
                    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat:
                    *outDataSize = sizeof(AudioStreamBasicDescription); return kAudioHardwareNoError;
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats:
                    *outDataSize = 6 * sizeof(AudioStreamRangedDescription); return kAudioHardwareNoError;
            }
            break;

        case kVolumeControlObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
                case kAudioObjectPropertyElementName:
                    *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
                case kAudioLevelControlPropertyScalarValue:
                    *outDataSize = sizeof(Float32); return kAudioHardwareNoError;
                case kAudioLevelControlPropertyDecibelValue:
                    *outDataSize = sizeof(Float32); return kAudioHardwareNoError;
                case kAudioLevelControlPropertyDecibelRange:
                    *outDataSize = sizeof(AudioValueRange); return kAudioHardwareNoError;
                case kAudioControlPropertyScope:
                    *outDataSize = sizeof(AudioObjectPropertyScope); return kAudioHardwareNoError;
                case kAudioControlPropertyElement:
                    *outDataSize = sizeof(AudioObjectPropertyElement); return kAudioHardwareNoError;
            }
            break;
    }

    return kAudioHardwareUnknownPropertyError;
}

static OSStatus R26_GetPropertyData(AudioServerPlugInDriverRef inDriver,
                                     AudioObjectID inObjectID,
                                     pid_t inClientProcessID,
                                     const AudioObjectPropertyAddress *inAddress,
                                     UInt32 inQualifierDataSize,
                                     const void *inQualifierData,
                                     UInt32 inDataSize,
                                     UInt32 *outDataSize,
                                     void *outData) {
    (void)inDriver; (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;

    static const Float64 sSampleRates[] = {32000.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0};
    static const int kNumSampleRates = 6;

    switch (inObjectID) {
        // ---- Plugin Object ----
        case kPlugInObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    *outDataSize = sizeof(AudioClassID);
                    *((AudioClassID *)outData) = kAudioObjectClassID;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID);
                    *((AudioClassID *)outData) = kAudioPlugInClassID;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID);
                    *((AudioObjectID *)outData) = kAudioObjectUnknown;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyManufacturer:
                    *outDataSize = sizeof(CFStringRef);
                    *((CFStringRef *)outData) = CFSTR(kDeviceManufacturer);
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyOwnedObjects:
                case kAudioPlugInPropertyDeviceList:
                    *outDataSize = sizeof(AudioObjectID);
                    *((AudioObjectID *)outData) = kDeviceObjectID;
                    return kAudioHardwareNoError;

                case kAudioPlugInPropertyTranslateUIDToDevice: {
                    CFStringRef uid = *((CFStringRef *)inQualifierData);
                    *outDataSize = sizeof(AudioObjectID);
                    if (CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo) {
                        *((AudioObjectID *)outData) = kDeviceObjectID;
                    } else {
                        *((AudioObjectID *)outData) = kAudioObjectUnknown;
                    }
                    return kAudioHardwareNoError;
                }

                case kAudioPlugInPropertyResourceBundle:
                    *outDataSize = sizeof(CFStringRef);
                    *((CFStringRef *)outData) = CFSTR("");
                    return kAudioHardwareNoError;
            }
            break;

        // ---- Device Object ----
        case kDeviceObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    *outDataSize = sizeof(AudioClassID);
                    *((AudioClassID *)outData) = kAudioObjectClassID;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID);
                    *((AudioClassID *)outData) = kAudioDeviceClassID;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID);
                    *((AudioObjectID *)outData) = kPlugInObjectID;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyName:
                    *outDataSize = sizeof(CFStringRef);
                    *((CFStringRef *)outData) = CFSTR(kDeviceName);
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyManufacturer:
                    *outDataSize = sizeof(CFStringRef);
                    *((CFStringRef *)outData) = CFSTR(kDeviceManufacturer);
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyDeviceUID:
                    *outDataSize = sizeof(CFStringRef);
                    *((CFStringRef *)outData) = CFSTR(kDeviceUID);
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyModelUID:
                    *outDataSize = sizeof(CFStringRef);
                    *((CFStringRef *)outData) = CFSTR(kDeviceModelUID);
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyTransportType:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = kAudioDeviceTransportTypeUSB;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyRelatedDevices:
                    *outDataSize = sizeof(AudioObjectID);
                    *((AudioObjectID *)outData) = kDeviceObjectID;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyClockDomain:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = 0;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyDeviceIsAlive:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = 1;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyDeviceIsRunning:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = gDriverState.ioRunning ? 1 : 0;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = (inAddress->mScope == kAudioObjectPropertyScopeInput) ? 1 : 0;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = 0;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyLatency:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = 0;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertySafetyOffset:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = 0;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyStreams:
                    if (inAddress->mScope == kAudioObjectPropertyScopeInput ||
                        inAddress->mScope == kAudioObjectPropertyScopeGlobal) {
                        *outDataSize = sizeof(AudioObjectID);
                        *((AudioObjectID *)outData) = kInputStreamObjectID;
                    } else {
                        *outDataSize = 0;
                    }
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyOwnedObjects: {
                    UInt32 count = 0;
                    AudioObjectID *ids = (AudioObjectID *)outData;
                    if (inDataSize >= sizeof(AudioObjectID)) {
                        ids[count++] = kInputStreamObjectID;
                    }
                    if (inDataSize >= 2 * sizeof(AudioObjectID)) {
                        ids[count++] = kVolumeControlObjectID;
                    }
                    *outDataSize = count * sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                }

                case kAudioObjectPropertyControlList:
                    *outDataSize = sizeof(AudioObjectID);
                    *((AudioObjectID *)outData) = kVolumeControlObjectID;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyNominalSampleRate:
                    *outDataSize = sizeof(Float64);
                    *((Float64 *)outData) = gDriverState.sampleRate;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyAvailableNominalSampleRates: {
                    UInt32 count = 0;
                    AudioValueRange *ranges = (AudioValueRange *)outData;
                    for (int i = 0; i < kNumSampleRates && (count + 1) * sizeof(AudioValueRange) <= inDataSize; i++) {
                        ranges[count].mMinimum = sSampleRates[i];
                        ranges[count].mMaximum = sSampleRates[i];
                        count++;
                    }
                    *outDataSize = count * sizeof(AudioValueRange);
                    return kAudioHardwareNoError;
                }

                case kAudioDevicePropertyZeroTimeStampPeriod:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = kZeroTimestampPeriod;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyIsHidden:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = 0;
                    return kAudioHardwareNoError;

                case kAudioDevicePropertyPreferredChannelsForStereo: {
                    UInt32 *ch = (UInt32 *)outData;
                    ch[0] = 1;
                    ch[1] = 2;
                    *outDataSize = 2 * sizeof(UInt32);
                    return kAudioHardwareNoError;
                }

                case kAudioDevicePropertyPreferredChannelLayout: {
                    AudioChannelLayout *layout = (AudioChannelLayout *)outData;
                    layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
                    layout->mChannelBitmap = 0;
                    layout->mNumberChannelDescriptions = kNumChannels;
                    layout->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
                    layout->mChannelDescriptions[0].mChannelFlags = 0;
                    layout->mChannelDescriptions[0].mCoordinates[0] = 0;
                    layout->mChannelDescriptions[0].mCoordinates[1] = 0;
                    layout->mChannelDescriptions[0].mCoordinates[2] = 0;
                    layout->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
                    layout->mChannelDescriptions[1].mChannelFlags = 0;
                    layout->mChannelDescriptions[1].mCoordinates[0] = 0;
                    layout->mChannelDescriptions[1].mCoordinates[1] = 0;
                    layout->mChannelDescriptions[1].mCoordinates[2] = 0;
                    *outDataSize = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) +
                                   kNumChannels * sizeof(AudioChannelDescription));
                    return kAudioHardwareNoError;
                }
            }
            break;

        // ---- Input Stream Object ----
        case kInputStreamObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    *outDataSize = sizeof(AudioClassID);
                    *((AudioClassID *)outData) = kAudioObjectClassID;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID);
                    *((AudioClassID *)outData) = kAudioStreamClassID;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID);
                    *((AudioObjectID *)outData) = kDeviceObjectID;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyName:
                    *outDataSize = sizeof(CFStringRef);
                    *((CFStringRef *)outData) = CFSTR("R-26 Input");
                    return kAudioHardwareNoError;

                case kAudioStreamPropertyIsActive:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = 1;
                    return kAudioHardwareNoError;

                case kAudioStreamPropertyDirection:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = 1; // 1 = input
                    return kAudioHardwareNoError;

                case kAudioStreamPropertyTerminalType:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = kAudioStreamTerminalTypeMicrophone;
                    return kAudioHardwareNoError;

                case kAudioStreamPropertyStartingChannel:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = 1;
                    return kAudioHardwareNoError;

                case kAudioStreamPropertyLatency:
                    *outDataSize = sizeof(UInt32);
                    *((UInt32 *)outData) = 0;
                    return kAudioHardwareNoError;

                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat: {
                    AudioStreamBasicDescription *fmt = (AudioStreamBasicDescription *)outData;
                    fmt->mSampleRate = gDriverState.sampleRate;
                    fmt->mFormatID = kAudioFormatLinearPCM;
                    fmt->mFormatFlags = kAudioFormatFlagIsFloat |
                                        kAudioFormatFlagsNativeEndian |
                                        kAudioFormatFlagIsPacked;
                    fmt->mBytesPerPacket = kBytesPerFrame;
                    fmt->mFramesPerPacket = 1;
                    fmt->mBytesPerFrame = kBytesPerFrame;
                    fmt->mChannelsPerFrame = kNumChannels;
                    fmt->mBitsPerChannel = kBitsPerChannel;
                    *outDataSize = sizeof(AudioStreamBasicDescription);
                    return kAudioHardwareNoError;
                }

                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats: {
                    UInt32 count = 0;
                    AudioStreamRangedDescription *descs = (AudioStreamRangedDescription *)outData;
                    for (int i = 0; i < kNumSampleRates && (count + 1) * sizeof(AudioStreamRangedDescription) <= inDataSize; i++) {
                        descs[count].mFormat.mSampleRate = sSampleRates[i];
                        descs[count].mFormat.mFormatID = kAudioFormatLinearPCM;
                        descs[count].mFormat.mFormatFlags = kAudioFormatFlagIsFloat |
                                                             kAudioFormatFlagsNativeEndian |
                                                             kAudioFormatFlagIsPacked;
                        descs[count].mFormat.mBytesPerPacket = kBytesPerFrame;
                        descs[count].mFormat.mFramesPerPacket = 1;
                        descs[count].mFormat.mBytesPerFrame = kBytesPerFrame;
                        descs[count].mFormat.mChannelsPerFrame = kNumChannels;
                        descs[count].mFormat.mBitsPerChannel = kBitsPerChannel;
                        descs[count].mSampleRateRange.mMinimum = sSampleRates[i];
                        descs[count].mSampleRateRange.mMaximum = sSampleRates[i];
                        count++;
                    }
                    *outDataSize = count * sizeof(AudioStreamRangedDescription);
                    return kAudioHardwareNoError;
                }
            }
            break;

        // ---- Volume Control Object ----
        case kVolumeControlObjectID:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    *outDataSize = sizeof(AudioClassID);
                    *((AudioClassID *)outData) = kAudioObjectClassID;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID);
                    *((AudioClassID *)outData) = kAudioVolumeControlClassID;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID);
                    *((AudioObjectID *)outData) = kDeviceObjectID;
                    return kAudioHardwareNoError;

                case kAudioObjectPropertyElementName:
                    *outDataSize = sizeof(CFStringRef);
                    *((CFStringRef *)outData) = CFSTR("Volume");
                    return kAudioHardwareNoError;

                case kAudioControlPropertyScope:
                    *outDataSize = sizeof(AudioObjectPropertyScope);
                    *((AudioObjectPropertyScope *)outData) = kAudioObjectPropertyScopeInput;
                    return kAudioHardwareNoError;

                case kAudioControlPropertyElement:
                    *outDataSize = sizeof(AudioObjectPropertyElement);
                    *((AudioObjectPropertyElement *)outData) = kAudioObjectPropertyElementMain;
                    return kAudioHardwareNoError;

                case kAudioLevelControlPropertyScalarValue:
                    *outDataSize = sizeof(Float32);
                    *((Float32 *)outData) = gDriverState.volumeL;
                    return kAudioHardwareNoError;

                case kAudioLevelControlPropertyDecibelValue:
                    *outDataSize = sizeof(Float32);
                    if (gDriverState.volumeL <= 0.0f)
                        *((Float32 *)outData) = -96.0f;
                    else
                        *((Float32 *)outData) = 20.0f * log10f(gDriverState.volumeL);
                    return kAudioHardwareNoError;

                case kAudioLevelControlPropertyDecibelRange: {
                    AudioValueRange *range = (AudioValueRange *)outData;
                    range->mMinimum = -96.0;
                    range->mMaximum = 0.0;
                    *outDataSize = sizeof(AudioValueRange);
                    return kAudioHardwareNoError;
                }
            }
            break;
    }

    return kAudioHardwareUnknownPropertyError;
}

static OSStatus R26_SetPropertyData(AudioServerPlugInDriverRef inDriver,
                                     AudioObjectID inObjectID,
                                     pid_t inClientProcessID,
                                     const AudioObjectPropertyAddress *inAddress,
                                     UInt32 inQualifierDataSize,
                                     const void *inQualifierData,
                                     UInt32 inDataSize,
                                     const void *inData) {
    (void)inDriver; (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;

    if (inObjectID == kDeviceObjectID && inAddress->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inDataSize == sizeof(Float64)) {
            Float64 newRate = *((const Float64 *)inData);
            pthread_mutex_lock(&gDriverState.mutex);
            gDriverState.sampleRate = newRate;
            compute_host_ticks_per_frame();
            gDriverState.clockSeed++;
            pthread_mutex_unlock(&gDriverState.mutex);

            // Notify host
            AudioObjectPropertyAddress addr = {
                kAudioDevicePropertyNominalSampleRate,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            gDriverState.host->PropertiesChanged(gDriverState.host, kDeviceObjectID, 1, &addr);
            LOG("Sample rate changed to %.0f", newRate);
        }
        return kAudioHardwareNoError;
    }

    if (inObjectID == kVolumeControlObjectID) {
        if (inAddress->mSelector == kAudioLevelControlPropertyScalarValue && inDataSize == sizeof(Float32)) {
            Float32 vol = *((const Float32 *)inData);
            if (vol < 0.0f) vol = 0.0f;
            if (vol > 1.0f) vol = 1.0f;
            gDriverState.volumeL = vol;
            gDriverState.volumeR = vol;

            AudioObjectPropertyAddress addr = {
                kAudioLevelControlPropertyScalarValue,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            gDriverState.host->PropertiesChanged(gDriverState.host, kVolumeControlObjectID, 1, &addr);
            return kAudioHardwareNoError;
        }
        if (inAddress->mSelector == kAudioLevelControlPropertyDecibelValue && inDataSize == sizeof(Float32)) {
            Float32 dB = *((const Float32 *)inData);
            if (dB < -96.0f) dB = -96.0f;
            if (dB > 0.0f) dB = 0.0f;
            Float32 vol = powf(10.0f, dB / 20.0f);
            gDriverState.volumeL = vol;
            gDriverState.volumeR = vol;

            AudioObjectPropertyAddress addr = {
                kAudioLevelControlPropertyScalarValue,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            gDriverState.host->PropertiesChanged(gDriverState.host, kVolumeControlObjectID, 1, &addr);
            return kAudioHardwareNoError;
        }
    }

    return kAudioHardwareUnknownPropertyError;
}

// ============================================================================
#pragma mark - IO Operations
// ============================================================================

static OSStatus R26_StartIO(AudioServerPlugInDriverRef inDriver,
                             AudioObjectID inDeviceObjectID,
                             UInt32 inClientID) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;

    pthread_mutex_lock(&gDriverState.mutex);

    if (gDriverState.ioClientCount == 0) {
        // First client starting IO
        gDriverState.anchorHostTime = mach_absolute_time();
        gDriverState.anchorSampleTime = 0.0;
        gDriverState.ioRunning = true;

        // Try to connect to shared memory
        shm_connect();

        LOG("IO started");
    }
    gDriverState.ioClientCount++;

    pthread_mutex_unlock(&gDriverState.mutex);
    return kAudioHardwareNoError;
}

static OSStatus R26_StopIO(AudioServerPlugInDriverRef inDriver,
                            AudioObjectID inDeviceObjectID,
                            UInt32 inClientID) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;

    pthread_mutex_lock(&gDriverState.mutex);

    if (gDriverState.ioClientCount > 0) {
        gDriverState.ioClientCount--;
        if (gDriverState.ioClientCount == 0) {
            gDriverState.ioRunning = false;
            LOG("IO stopped");
        }
    }

    pthread_mutex_unlock(&gDriverState.mutex);
    return kAudioHardwareNoError;
}

static OSStatus R26_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver,
                                      AudioObjectID inDeviceObjectID,
                                      UInt32 inClientID,
                                      Float64 *outSampleTime,
                                      UInt64 *outHostTime,
                                      UInt64 *outSeed) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;

    UInt64 now = mach_absolute_time();
    Float64 ticksPerFrame = gDriverState.hostTicksPerFrame;

    // Calculate how many complete zero-timestamp periods have elapsed
    Float64 ticksSinceAnchor = (Float64)(now - gDriverState.anchorHostTime);
    Float64 framesSinceAnchor = ticksSinceAnchor / ticksPerFrame;
    UInt64 periods = (UInt64)(framesSinceAnchor / kZeroTimestampPeriod);

    *outSampleTime = (Float64)(periods * kZeroTimestampPeriod);
    *outHostTime = gDriverState.anchorHostTime +
                   (UInt64)(*outSampleTime * ticksPerFrame);
    *outSeed = gDriverState.clockSeed;

    return kAudioHardwareNoError;
}

static OSStatus R26_WillDoIOOperation(AudioServerPlugInDriverRef inDriver,
                                       AudioObjectID inDeviceObjectID,
                                       UInt32 inClientID,
                                       UInt32 inOperationID,
                                       Boolean *outWillDo,
                                       Boolean *outWillDoInPlace) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;

    *outWillDo = false;
    *outWillDoInPlace = true;

    switch (inOperationID) {
        case kAudioServerPlugInIOOperationReadInput:
            *outWillDo = true;
            *outWillDoInPlace = true;
            break;
    }

    return kAudioHardwareNoError;
}

static OSStatus R26_BeginIOOperation(AudioServerPlugInDriverRef inDriver,
                                      AudioObjectID inDeviceObjectID,
                                      UInt32 inClientID,
                                      UInt32 inOperationID,
                                      UInt32 inIOBufferFrameSize,
                                      const AudioServerPlugInIOCycleInfo *inIOCycleInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    return kAudioHardwareNoError;
}

static OSStatus R26_DoIOOperation(AudioServerPlugInDriverRef inDriver,
                                   AudioObjectID inDeviceObjectID,
                                   AudioObjectID inStreamObjectID,
                                   UInt32 inClientID,
                                   UInt32 inOperationID,
                                   UInt32 inIOBufferFrameSize,
                                   const AudioServerPlugInIOCycleInfo *inIOCycleInfo,
                                   void *ioMainBuffer,
                                   void *ioSecondaryBuffer) {
    (void)inDriver; (void)inDeviceObjectID; (void)inStreamObjectID;
    (void)inClientID; (void)inIOCycleInfo; (void)ioSecondaryBuffer;

    if (inOperationID == kAudioServerPlugInIOOperationReadInput) {
        Float32 *buffer = (Float32 *)ioMainBuffer;
        UInt32 totalSamples = inIOBufferFrameSize * kNumChannels;

        // Try to read from shared memory
        if (gDriverState.shmConnected && gDriverState.shm) {
            uint32_t status = atomic_load_explicit(&gDriverState.shm->status, memory_order_acquire);
            if (status == R26_STATUS_RUNNING) {
                uint64_t read = r26_ring_read(gDriverState.shm, buffer, inIOBufferFrameSize);
                // If we didn't get enough frames, zero-fill the rest
                if (read < inIOBufferFrameSize) {
                    memset(buffer + (read * kNumChannels), 0,
                           (inIOBufferFrameSize - read) * kBytesPerFrame);
                }

                // Apply volume
                Float32 vol = gDriverState.volumeL;
                if (vol < 1.0f) {
                    for (UInt32 i = 0; i < totalSamples; i++) {
                        buffer[i] *= vol;
                    }
                }
                return kAudioHardwareNoError;
            }
        } else {
            // Try reconnecting periodically
            shm_connect();
        }

        // No data available - output silence
        memset(buffer, 0, totalSamples * sizeof(Float32));
    }

    return kAudioHardwareNoError;
}

static OSStatus R26_EndIOOperation(AudioServerPlugInDriverRef inDriver,
                                    AudioObjectID inDeviceObjectID,
                                    UInt32 inClientID,
                                    UInt32 inOperationID,
                                    UInt32 inIOBufferFrameSize,
                                    const AudioServerPlugInIOCycleInfo *inIOCycleInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    return kAudioHardwareNoError;
}

// ============================================================================
#pragma mark - Driver Interface vtable
// ============================================================================

static AudioServerPlugInDriverInterface gDriverInterface = {
    NULL, // _reserved
    R26_QueryInterface,
    R26_AddRef,
    R26_Release,
    R26_Initialize,
    R26_CreateDevice,
    R26_DestroyDevice,
    R26_AddDeviceClient,
    R26_RemoveDeviceClient,
    R26_PerformDeviceConfigurationChange,
    R26_AbortDeviceConfigurationChange,
    R26_HasProperty,
    R26_IsPropertySettable,
    R26_GetPropertyDataSize,
    R26_GetPropertyData,
    R26_SetPropertyData,
    R26_StartIO,
    R26_StopIO,
    R26_GetZeroTimeStamp,
    R26_WillDoIOOperation,
    R26_BeginIOOperation,
    R26_DoIOOperation,
    R26_EndIOOperation,
};

static AudioServerPlugInDriverInterface *gDriverInterfacePtr = &gDriverInterface;

// ============================================================================
#pragma mark - Factory Function
// ============================================================================

void *R26Audio_Create(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID) {
    (void)allocator;

    CFUUIDRef pluginTypeUUID = kAudioServerPlugInTypeUUID;
    if (!CFEqual(requestedTypeUUID, pluginTypeUUID)) {
        return NULL;
    }

    gLog = os_log_create("com.r26bridge.audio", "driver");

    pthread_mutex_init(&gDriverState.mutex, NULL);
    gDriverState.refCount = 1;
    gDriverState.interface = &gDriverInterfacePtr;
    gDriverState.sampleRate = kDefaultSampleRate;
    gDriverState.volumeL = 1.0f;
    gDriverState.volumeR = 1.0f;
    gDriverState.shmFd = -1;

    compute_host_ticks_per_frame();

    LOG("R26Audio plugin created");
    return &gDriverInterfacePtr;
}
