#include <video_device_module/version.h>
#include <video_device_module/video_device_module_impl.h>
#include <video_device_module/camera_device_impl.h>
#include <video_device_module/camera_platform.h>
#include <video_device_module/video_frame_encoder_fb_impl.h>
#if defined(VIDEO_DEVICE_MODULE_ENABLE_VIDEO_DISPLAY)
#include <video_device_module/video_display_fb_impl.h>
#endif
#include <coretypes/version_info_factory.h>
#include <opendaq/custom_log.h>

extern "C"
{
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
}

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE

VideoDeviceModule::VideoDeviceModule(const ContextPtr& context)
    : Module(VIDEO_DEVICE_MODULE_ID,
            daq::VersionInfo(VIDEO_DEVICE_MODULE_MAJOR_VERSION, VIDEO_DEVICE_MODULE_MINOR_VERSION, VIDEO_DEVICE_MODULE_PATCH_VERSION),
            context,
            VIDEO_DEVICE_MODULE_ID)
{
    av_log_set_level(AV_LOG_FATAL);
    avdevice_register_all();
}

ListPtr<IDeviceInfo> VideoDeviceModule::onGetAvailableDevices()
{
    auto result = List<IDeviceInfo>();

    for (const auto& entry : listCameraDevices())
    {
        auto deviceInfo = CameraDeviceImpl::CreateDeviceInfo(entry.path);
        if (deviceInfo.assigned())
            result.pushBack(deviceInfo);
    }

    // Network cameras aren't locally enumerable like v4l2/dshow devices, so list a
    // public RTSP stream here as an example of a valid network camera path.
    auto exampleDeviceInfo = CameraDeviceImpl::CreateDeviceInfo("rtsp://stream.strba.sk:1935/strba/VYHLAD_JAZERO.stream");
    if (exampleDeviceInfo.assigned())
        result.pushBack(exampleDeviceInfo);

    return result;
}

DictPtr<IString, IDeviceType> VideoDeviceModule::onGetAvailableDeviceTypes()
{
    auto result = Dict<IString, IDeviceType>();

    auto deviceType = CameraDeviceImpl::CreateType();
    result.set(deviceType.getId(), deviceType);

    return result; 
}

DevicePtr VideoDeviceModule::onCreateDevice(const StringPtr& connectionString,
                                            const ComponentPtr& parent,
                                            const PropertyObjectPtr& config)
{
    std::string devicePath = CameraDeviceImpl::GetCameraPath(connectionString);

    if (!isNetworkCameraPath(devicePath))
    {
        const AVInputFormat* input_fmt = av_find_input_format(cameraInputFormatName());
        if (!input_fmt)
            DAQ_THROW_EXCEPTION(NotFoundException, "{} input format not found", cameraInputFormatName());
    }

    std::string localId = devicePath;
    for (auto& ch : localId)
    {
        if (ch == '/' || ch == ':')
            ch = '_';
    }

    return createWithImplementation<IDevice, CameraDeviceImpl>(devicePath, config, context, parent, localId, localId);
}

DictPtr<IString, IFunctionBlockType> VideoDeviceModule::onGetAvailableFunctionBlockTypes()
{
    auto types = Dict<IString, IFunctionBlockType>();
    const auto encoderType = VideoFrameEncoderFbImpl::CreateType();
    types.set(encoderType.getId(), encoderType);
#if defined(VIDEO_DEVICE_MODULE_ENABLE_VIDEO_DISPLAY)
    const auto displayType = VideoDisplayFbImpl::CreateType();
    types.set(displayType.getId(), displayType);
#endif
    return types;
}

FunctionBlockPtr VideoDeviceModule::onCreateFunctionBlock(const StringPtr& id,
                                                          const ComponentPtr& parent,
                                                          const StringPtr& localId,
                                                          const PropertyObjectPtr& config)
{
    if (id == VideoFrameEncoderFbImpl::Id)
        return createWithImplementation<IFunctionBlock, VideoFrameEncoderFbImpl>(context, parent, localId);
#if defined(VIDEO_DEVICE_MODULE_ENABLE_VIDEO_DISPLAY)
    if (id == VideoDisplayFbImpl::Id)
        return createWithImplementation<IFunctionBlock, VideoDisplayFbImpl>(context, parent, localId);
#endif

    LOG_W("Function block \"{}\" not found", id);
    DAQ_THROW_EXCEPTION(NotFoundException, "Function block not found");
}

END_NAMESPACE_VIDEO_DEVICE_MODULE
