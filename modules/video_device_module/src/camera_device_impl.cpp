#include <video_device_module/camera_device_impl.h>
#include <video_device_module/camera_platform.h>

#include <opendaq/data_descriptor_factory.h>
#include <opendaq/device_domain_factory.h>
#include <opendaq/device_type_factory.h>
#include <opendaq/deleter_factory.h>
#include <opendaq/event_packet_params.h>
#include <opendaq/input_port_factory.h>
#include <opendaq/packet_factory.h>
#include <opendaq/reader_factory.h>
#include <opendaq/reference_domain_info_factory.h>
#include <opendaq/signal_factory.h>

#include <chrono>

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE

CameraDeviceImpl::CameraDeviceImpl(const StringPtr& cameraPath,
                                   const PropertyObjectPtr& config,
                                   const ContextPtr& ctx,
                                   const ComponentPtr& parent,
                                   const StringPtr& localId,
                                   const StringPtr& name)
    : Super(ctx, parent, localId, nullptr, name)
    , cameraPath(cameraPath)
    , timeOffsetNs(0)
{
    initComponentStatus();
    if (!driver.open(this->cameraPath))
    {
        setComponentStatusWithMessage(ComponentStatus::Error, "Failed to open camera driver");
        return;
    }

    initProperties();
    initSignals();
    initChannels();

    if (needsSelfClock)
        startSelfClock();
}

CameraDeviceImpl::~CameraDeviceImpl()
{
    stopSelfClock();
}

PropertyObjectPtr CameraDeviceImpl::CreateDefaultDeviceConfig()
{
    return PropertyObject();
}

DeviceTypePtr CameraDeviceImpl::CreateType()
{
    return DeviceType(CAMERA_DEVICE_TYPE_ID,
                      CAMERA_DEVICE_TYPE_NAME,
                      CAMERA_DEVICE_TYPE_DESCRIPTION,
                      CAMERA_DEVICE_TYPE_CONNECTION_STRING_PREFIX,
                      CreateDefaultDeviceConfig());
}

DeviceInfoPtr CameraDeviceImpl::CreateDeviceInfo(const std::string& cameraPath)
{
    if (isNetworkCameraPath(cameraPath))
    {
        const StringPtr connectionString = fmt::format("{}://{}", CAMERA_DEVICE_TYPE_CONNECTION_STRING_PREFIX, cameraPath);

        auto deviceInfo = DeviceInfo(connectionString, cameraPath);
        deviceInfo.setModel(cameraPath);
        deviceInfo.setDeviceType(CameraDeviceImpl::CreateType());
        return deviceInfo;
    }

    for (const auto& entry : listCameraDevices())
    {
        if (entry.path != cameraPath)
            continue;

        const StringPtr connectionString = fmt::format("{}://device{}", CAMERA_DEVICE_TYPE_CONNECTION_STRING_PREFIX, cameraPath);

        auto deviceInfo = DeviceInfo(connectionString, entry.name);
        deviceInfo.setModel(entry.name);
        deviceInfo.setDeviceType(CameraDeviceImpl::CreateType());
        return deviceInfo;
    }

    return nullptr;
}

std::string CameraDeviceImpl::GetCameraPath(const std::string& connectionString)
{
    const std::string typePrefix = fmt::format("{}://", CAMERA_DEVICE_TYPE_CONNECTION_STRING_PREFIX);
    if (connectionString.find(typePrefix) != 0)
        DAQ_THROW_EXCEPTION(InvalidParameterException, "Invalid connection string prefix for connection string '{}'", connectionString);

    const std::string remainder = connectionString.substr(typePrefix.length());

    // Local devices use an explicit "device" marker (e.g. "camera://device/dev/video0");
    // anything else is already a full stream URL (e.g. "camera://rtsp://host/stream").
    const std::string deviceMarker = "device";
    if (remainder.rfind(deviceMarker, 0) == 0)
        return remainder.substr(deviceMarker.length());

    return remainder;
}

DeviceInfoPtr CameraDeviceImpl::onGetInfo()
{
    auto info = CreateDeviceInfo(cameraPath);
    if (!info.assigned() || !driver.isOpen())
        return info;

    const auto& stream = driver.getStreamInfo();
    info.addProperty(IntPropertyBuilder("FrameWidth", stream.width).setReadOnly(true).build());
    info.addProperty(IntPropertyBuilder("FrameHeight", stream.height).setReadOnly(true).build());
    info.addProperty(FloatPropertyBuilder("FrameRate", stream.frameRateHz).setReadOnly(true).build());
    info.addProperty(StringPropertyBuilder("NativeFormat", stream.nativeFormat).setReadOnly(true).build());
    return info;
}

RatioPtr CameraDeviceImpl::getRatio() const
{
    return Ratio(1, 1'000'000'000);
}

void CameraDeviceImpl::initProperties()
{
    const auto& stream = driver.getStreamInfo();
    const Int frameRateHz = static_cast<Int>(stream.frameRateHz + 0.5f);
    timeAccumulator.setFrequency(Ratio(1, frameRateHz).simplify());

    const auto timeOffsetProp = IntPropertyBuilder("TimeOffset", timeOffsetNs).setUnit(Unit("ns")).build();
    objPtr.addProperty(timeOffsetProp);
    objPtr.getOnPropertyValueWrite("TimeOffset") +=
        [this](PropertyObjectPtr& /*obj*/, PropertyValueEventArgsPtr& args) { onTimeOffsetChanged(args.getValue()); };
}

void CameraDeviceImpl::initSignals()
{
    const auto unit = Unit("s", -1, "seconds", "time");

    const auto referenceDomainInfo = ReferenceDomainInfoBuilder().setReferenceDomainId(localId)
                                                                 .setReferenceDomainOffset(0)
                                                                 .build();

    deviceDomain = DeviceDomain(getRatio(), "1970-01-01T00:00:00", unit, referenceDomainInfo);
    setDeviceDomain(deviceDomain);

    timeSignal = createAndAddSignal("Time", createTimeSignalDescriptor(), false);
    timeSignal.getTags().asPtr<ITagsPrivate>().add("DeviceDomain");

    DevicePtr rootDevice = context.getRootDevice();
    if (rootDevice.assigned())
    {
        for (const auto& signal : rootDevice.getSignals(search::Any()))
        {
            if (const auto descriptor = signal.getDescriptor(); descriptor.assigned())
            {
                const auto signalUnit = descriptor.getUnit();
                if (signalUnit.assigned() && signalUnit.getSymbol() == "s" && signalUnit.getQuantity() == "time")
                {
                    rootDomainSignal = signal;

                    deviceTimeInputPort = InputPort(context, nullptr, "SourceTime");
                    deviceTimeInputPort.setNotificationMethod(PacketReadyNotification::Scheduler);
                    deviceTimeInputPort.connect(signal);

                    reader = PacketReaderFromPort(deviceTimeInputPort);
                    reader.setOnDataAvailable([&] { onPacketReceived(); });

                    deviceTimeInputPort.setActive(true);
                    setComponentStatus(ComponentStatus::Ok);
                    return;
                }
            }
        }
    }

    LOG_I("No parent time signal found, self-clocking camera acquisition instead");
    setComponentStatus(ComponentStatus::Ok);
    needsSelfClock = true;
}

void CameraDeviceImpl::initChannels()
{
    const auto ch = createAndAddChannel<CameraChannelImpl>(ioFolder, "Camera", timeSignal, rootDomainSignal, driver);
    ch.getTags().asPtr<ITagsPrivate>().add("Camera");
    channel = ch;
}

DataDescriptorPtr CameraDeviceImpl::createTimeSignalDescriptor()
{
    const auto frameRateHz = static_cast<Int>(driver.getStreamInfo().frameRateHz + 0.5f);
    const UInt expectedDeltaTicks =
        frameRateHz > 0 ? static_cast<UInt>(1'000'000'000ULL / static_cast<uint64_t>(frameRateHz)) : 0;
    const UInt minExpectedDelta = expectedDeltaTicks > 0 ? expectedDeltaTicks * 9 / 10 : 0;
    const UInt maxExpectedDelta = expectedDeltaTicks > 0 ? expectedDeltaTicks * 11 / 10 : 0;

    return DataDescriptorBuilder()
        .setSampleType(SampleType::UInt64)
        .setRule(ExplicitDomainDataRule(minExpectedDelta, maxExpectedDelta))
        .setTickResolution(deviceDomain.getTickResolution())
        .setOrigin(deviceDomain.getOrigin())
        .setUnit(deviceDomain.getUnit())
        .setMetadata(Dict<IString, IString>({{"SampleRate", std::to_string(frameRateHz)}}))
        .setName("Time")
        .build();
}

void CameraDeviceImpl::onPacketReceived()
{
    auto lock = getAcquisitionLock2();
    const auto packets = reader.readAll();

    for (const auto& packet : packets)
    {
        switch (packet.getType())
        {
            case PacketType::None:
                LOG_W("Packet type None");
                break;
            case PacketType::Data:
                handleDataPacket(packet.asPtr<IDataPacket>(true));
                break;
            case PacketType::Event:
                handleEventPacket(packet.asPtr<IEventPacket>(true));
                break;
        }
    }
}

void CameraDeviceImpl::handleEventPacket(const EventPacketPtr& packet)
{
    const auto params = packet.getParameters();
    if (!params.hasKey(event_packet_param::DATA_DESCRIPTOR))
        return;

    const DataDescriptorPtr sourceDescriptor = params.get(event_packet_param::DATA_DESCRIPTOR);
    timeAccumulator.setRatio(sourceDescriptor.getTickResolution());
    timeAccumulator.setStartTime(0);

    const auto domainRule = sourceDescriptor.getRule();
    if (!domainRule.assigned() || domainRule.getType() != DataRuleType::Linear)
    {
        setComponentStatusWithMessage(ComponentStatus::Error, "Source time signal rule must be linear");
        return;
    }

    sourceDelta = domainRule.getParameters().get("delta");

    if (!timeSignal.getDescriptor().assigned())
        timeSignal.setDescriptor(createTimeSignalDescriptor());

    deviceTimeInputPort.setActive(true);
    setComponentStatus(ComponentStatus::Ok);
}

void CameraDeviceImpl::handleDataPacket(const DataPacketPtr& packet)
{
    if (packet.getSampleCount() == 0)
    {
        LOG_D("Received empty data packet");
        return;
    }

    UInt offset = packet.getOffset();

    if (timeAccumulator.getSourceTime() == 0)
        timeAccumulator.setStartTime(offset);

    offset += sourceDelta * (packet.getSampleCount() - 1);

    const auto sampleCount = timeAccumulator.update(offset);
    if (sampleCount)
        generatePacket(sampleCount);
}

void CameraDeviceImpl::generatePacket(SizeT sampleCount)
{
    std::vector<CapturedFrame> frames;
    frames.reserve(sampleCount);

    const size_t framesRead = driver.readFrames(sampleCount, frames);
    if (framesRead == 0)
    {
        LOG_W("No frames read from camera for {}", sampleCount);
        return;
    }

    const uint64_t offsetNs = static_cast<uint64_t>(timeOffsetNs);
    auto timestamps = std::make_unique<uint64_t[]>(framesRead);
    for (size_t i = 0; i < framesRead; ++i)
    {
        timestamps[i] = frames[i].timestampNs + offsetNs;
        frames[i].timestampNs = timestamps[i];
    }

    auto domainPacket = DataPacketWithExternalMemory(
        nullptr,
        timeSignal.getDescriptor(),
        framesRead,
        timestamps.release(),
        Deleter([](void* ptr) { delete[] static_cast<uint64_t*>(ptr); }));

    timeSignal.sendPacket(domainPacket);
    channel->generatePacket(domainPacket, frames);
}

void CameraDeviceImpl::startSelfClock()
{
    selfClockThread = std::thread(&CameraDeviceImpl::selfClockLoop, this);
}

void CameraDeviceImpl::stopSelfClock()
{
    if (!selfClockThread.joinable())
        return;

    {
        auto lock = getUniqueLock();
        stopSelfClockRequested = true;
    }
    selfClockCv.notify_one();
    // Network sources (RTSP/...) can block for a while inside generatePacket()'s frame read;
    // interrupt it so join() below doesn't wait on however long that read would otherwise take.
    driver.requestInterrupt();
    selfClockThread.join();
}

void CameraDeviceImpl::selfClockLoop()
{
    const auto frameRateHz = driver.getStreamInfo().frameRateHz;
    const auto framePeriod =
        frameRateHz > 0.0f
            ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<Float>(1.0f / frameRateHz))
            : std::chrono::seconds(1);

    auto nextTick = std::chrono::steady_clock::now();

    while (true)
    {
        {
            // Only hold the device lock while waiting/checking the stop flag: generatePacket()
            // below can block on network I/O, and must not do so while holding this lock, or
            // stopSelfClock() (called from the destructor, on another thread) would deadlock
            // trying to acquire the same lock just to request the stop.
            auto lock = getUniqueLock();
            nextTick += framePeriod;
            selfClockCv.wait_until(lock, nextTick, [this] { return stopSelfClockRequested; });
            if (stopSelfClockRequested)
                break;
        }

        generatePacket(1);
    }
}

uint64_t CameraDeviceImpl::onGetTicksSinceOrigin()
{
    if (const auto lastValue = timeSignal.getLastValue(); lastValue.assigned())
        return lastValue;
    return 0;
}

void CameraDeviceImpl::onTimeOffsetChanged(Int offset)
{
    auto lock = getRecursiveConfigLock2();
    timeOffsetNs = offset;
}

void CameraDeviceImpl::activeChanged()
{
    if (active)
        driver.flushBuffer(300);
}

END_NAMESPACE_VIDEO_DEVICE_MODULE
