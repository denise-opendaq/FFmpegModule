#include <video_device_module/camera_platform.h>

#include <cctype>
#include <unordered_map>

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

namespace
{
std::string sanitizeDeviceId(const std::string& name)
{
    std::string result;
    result.reserve(name.size());
    for (unsigned char ch : name)
        result += (std::isalnum(ch) || ch == '_' || ch == '-') ? static_cast<char>(ch) : '_';
    return result.empty() ? "device" : result;
}
}  // namespace

std::vector<std::string> assignUniqueDeviceIds(const std::vector<CameraDeviceEntry>& entries)
{
    std::unordered_map<std::string, int> seenCount;
    std::vector<std::string> ids;
    ids.reserve(entries.size());
    for (const auto& entry : entries)
    {
        const std::string base = sanitizeDeviceId(entry.name);
        const int count = ++seenCount[base];
        ids.push_back(count == 1 ? base : base + "_" + std::to_string(count));
    }
    return ids;
}

END_NAMESPACE_VIDEO_DEVICE_MODULE
