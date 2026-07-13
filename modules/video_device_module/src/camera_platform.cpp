#include <video_device_module/camera_platform.h>

#if !defined(__APPLE__)
extern "C"
{
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
}
#endif

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE

const char* cameraInputFormatName()
{
#if defined(__linux__)
    return "v4l2";
#elif defined(__APPLE__)
    return "avfoundation";
#elif defined(_WIN32)
    return "dshow";
#else
#error "Unsupported platform: no libavdevice camera input format known"
#endif
}

#if !defined(__APPLE__)
// v4l2 (Linux) and dshow (Windows) both implement libavdevice's programmatic
// device-listing callback, so a single implementation covers both.
std::vector<CameraDeviceEntry> listCameraDevices()
{
    std::vector<CameraDeviceEntry> result;

    const AVInputFormat* inputFmt = av_find_input_format(cameraInputFormatName());
    if (!inputFmt)
        return result;

    AVDeviceInfoList* deviceList = nullptr;
    if (avdevice_list_input_sources(inputFmt, nullptr, nullptr, &deviceList) < 0 || !deviceList)
        return result;

    for (int i = 0; i < deviceList->nb_devices; ++i)
    {
        const AVDeviceInfo* dev = deviceList->devices[i];
        result.push_back({dev->device_name, dev->device_description ? dev->device_description : dev->device_name});
    }

    avdevice_free_list_devices(&deviceList);
    return result;
}
#endif

bool isNetworkCameraPath(const std::string& path)
{
    return path.find("://") != std::string::npos;
}

END_NAMESPACE_VIDEO_DEVICE_MODULE
