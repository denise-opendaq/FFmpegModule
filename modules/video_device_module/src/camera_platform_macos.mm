#include <video_device_module/camera_platform.h>

#import <AVFoundation/AVFoundation.h>

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE

namespace
{
// Mirrors libavdevice's own getDevicesWithMediaType() (libavdevice/avfoundation.m) so that
// the indices we hand back line up exactly with the ones FFmpeg's avfoundation demuxer assigns
// when opening a device by index. This is macOS-only (TARGET_OS_OSX), the iOS branches of
// FFmpeg's original are dropped.
NSArray<AVCaptureDevice*>* devicesWithMediaType(AVMediaType mediaType)
{
#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 101500
    NSMutableArray<AVCaptureDeviceType>* deviceTypes = nil;

    if (mediaType == AVMediaTypeVideo)
    {
        deviceTypes = [NSMutableArray arrayWithArray:@[AVCaptureDeviceTypeBuiltInWideAngleCamera]];
#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 130000
        [deviceTypes addObject:AVCaptureDeviceTypeDeskViewCamera];
#endif
#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 140000
        [deviceTypes addObject:AVCaptureDeviceTypeContinuityCamera];
        [deviceTypes addObject:AVCaptureDeviceTypeExternal];
#else
        [deviceTypes addObject:AVCaptureDeviceTypeExternalUnknown];
#endif
    }
    else if (mediaType == AVMediaTypeMuxed)
    {
#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 140000
        deviceTypes = [NSMutableArray arrayWithArray:@[AVCaptureDeviceTypeExternal]];
#else
        deviceTypes = [NSMutableArray arrayWithArray:@[AVCaptureDeviceTypeExternalUnknown]];
#endif
    }
    else
    {
        return nil;
    }

    AVCaptureDeviceDiscoverySession* session =
        [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:deviceTypes
                                                                mediaType:mediaType
                                                                 position:AVCaptureDevicePositionUnspecified];
    return session.devices;
#else
    return [AVCaptureDevice devicesWithMediaType:mediaType];
#endif
}
}  // namespace

// libavdevice's avfoundation input format does not implement the get_device_list callback
// (avdevice_list_input_sources() returns ENOSYS), so device enumeration on macOS goes through
// AVFoundation directly. The device path handed back is a plain numeric index (matching
// FFmpeg's own video_device_index scheme) rather than the device's name: names such as
// "USB Camera VID:1133 PID:2085" contain a colon, which collides with FFmpeg's avfoundation
// demuxer "video:audio" filename syntax and gets misparsed as an audio device selector.
std::vector<CameraDeviceEntry> listCameraDevices()
{
    std::vector<CameraDeviceEntry> result;

    @autoreleasepool
    {
        NSArray<AVCaptureDevice*>* devices = devicesWithMediaType(AVMediaTypeVideo);
        NSArray<AVCaptureDevice*>* devicesMuxed = devicesWithMediaType(AVMediaTypeMuxed);

        NSUInteger index = 0;
        for (AVCaptureDevice* device in devices)
            result.push_back({std::to_string(index++), [[device localizedName] UTF8String]});
        for (AVCaptureDevice* device in devicesMuxed)
            result.push_back({std::to_string(index++), [[device localizedName] UTF8String]});
    }

    return result;
}

END_NAMESPACE_VIDEO_DEVICE_MODULE
