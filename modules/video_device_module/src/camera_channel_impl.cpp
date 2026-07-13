#include <video_device_module/camera_channel_impl.h>

#include <video_device_module/ffmpeg_av_packet_utils.h>

#include <opendaq/data_descriptor_factory.h>

extern "C"
{
#include <libavcodec/packet.h>
}

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE

CameraChannelImpl::CameraChannelImpl(const ContextPtr& ctx,
                                     const ComponentPtr& parent,
                                     const StringPtr& localId,
                                     const SignalPtr& domainSignal,
                                     const SignalPtr& rootDomainSignal,
                                     CameraDriver& driver)
    : Super(FunctionBlockType("CameraChannel", "Camera", ""), ctx, parent, localId, nullptr)
    , driver(driver)
    , rootDomainSignal(rootDomainSignal)
{
    objPtr.asPtr<IPropertyObjectInternal>(true).setLockingStrategy(LockingStrategy::InheritLock);

    initComponentStatus();

    if (!driver.isOpen())
        setComponentStatusWithMessage(ComponentStatus::Error, "Camera driver is not open");

    initSignals(domainSignal);
}

void CameraChannelImpl::generatePacket(IDataPacket* domainPacket, std::vector<CapturedFrame>& frames)
{
    if (!domainPacket || frames.empty())
        return;

    const auto valueDescriptor = rawVideoSignal.getDescriptor();
    const DataPacketPtr domainPacketPtr(domainPacket);

    for (auto& frame : frames)
    {
        if (!frame.packet || frame.packet->size <= 0)
            continue;

        auto packet = std::move(frame.packet);
        rawVideoSignal.sendPacket(binaryDataPacketFromAvPacket(domainPacketPtr, valueDescriptor, std::move(packet)));
    }
}

void CameraChannelImpl::initSignals(const SignalPtr& domainSignal)
{
    if (!domainSignal.assigned())
        setComponentStatusWithMessage(ComponentStatus::Error, "Input domain signal is not assigned");

    rawVideoSignal = createAndAddSignal("Video_Physical", createVideoSignalDescriptor());
    rawVideoSignal.getTags().asPtr<ITagsPrivate>().add("Video_Physical");
    rawVideoSignal.setDomainSignal(domainSignal);

    videoSignal = createAndAddSignal("Video");
    videoSignal.getTags().asPtr<ITagsPrivate>().add("Video");
    timeSignal = createAndAddSignal("Time", nullptr, false);
    videoSignal.setDomainSignal(timeSignal);
}

DataDescriptorPtr CameraChannelImpl::createVideoSignalDescriptor()
{
    const auto& stream = driver.getStreamInfo();
    const StringPtr formatName = stream.nativeFormat;
    return DataDescriptorBuilder()
        .setSampleType(SampleType::Binary)
        .setName("Video")
        .setUnit(Unit(formatName, -1, formatName, "Video frame format"))
        .setRule(ExplicitDataRule())
        .setMetadata(Dict<IString, IString>({
            {"NativeCodecId", std::to_string(stream.nativeCodecId)},
            {"NativePixelFormat", std::to_string(stream.nativePixelFormat)},
            {"FrameWidth", std::to_string(stream.width)},
            {"FrameHeight", std::to_string(stream.height)},
            {"FrameRateHz", std::to_string(stream.frameRateHz)},
        }))
        .build();
}

END_NAMESPACE_VIDEO_DEVICE_MODULE
