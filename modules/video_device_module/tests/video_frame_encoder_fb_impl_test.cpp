#include <video_device_module/video_frame_encoder_fb_impl.h>
#include <video_device_module/camera_device_impl.h>

#include <opendaq/binary_data_packet_factory.h>
#include <opendaq/reference_domain_info_factory.h>

#include <gtest/gtest.h>
#include <opendaq/function_block_impl.h>
#include <opendaq/input_port_factory.h>
#include <opendaq/opendaq.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
}

using namespace daq;
using namespace daq::modules::video_device_module;

namespace
{
constexpr int JPEG_FORMAT_INDEX = 0;
constexpr int PNG_FORMAT_INDEX = 1;

// A real, public RTSP stream used as a live fixture. All tests built around it skip (rather
// than fail) if it's unreachable, since it's an external dependency this module doesn't control.
constexpr auto IP_CAMERA_STREAM_URL = "rtsp://stream.strba.sk:1935/strba/VYHLAD_JAZERO.stream";

ContextPtr createContext()
{
    const auto logger = Logger();
    return Context(Scheduler(logger), logger, TypeManager(), nullptr, nullptr);
}

FunctionBlockPtr createVideoFrameEncoder(const ContextPtr& context)
{
    return createWithImplementation<IFunctionBlock, VideoFrameEncoderFbImpl>(context, nullptr, "VideoFrameEncoder");
}

DevicePtr createIpCameraDevice(const ContextPtr& context)
{
    return createWithImplementation<IDevice, CameraDeviceImpl>(
        StringPtr(IP_CAMERA_STREAM_URL), CameraDeviceImpl::CreateDefaultDeviceConfig(), context, nullptr, "IpCamera", "IpCamera");
}

// The real camera exposes its raw encoded video on the channel's "Video_Physical" signal
// (unlike the previous mock, which put a ready-made "Video" signal directly on the device).
SignalConfigPtr findChannelSignal(const DevicePtr& device, const std::string& localId)
{
    for (const auto& ch : device.getChannels())
    {
        for (const auto& sig : ch.getSignals())
        {
            if (sig.getLocalId() == localId)
                return sig;
        }
    }
    return nullptr;
}

SignalConfigPtr findFbSignal(const FunctionBlockPtr& fb, const std::string& localId)
{
    for (const auto& signal : fb.getSignals())
    {
        if (signal.getLocalId() == localId)
            return signal;
    }
    return nullptr;
}

bool isJpeg(const uint8_t* data, size_t size)
{
    return size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

bool isPng(const uint8_t* data, size_t size)
{
    return size >= 4 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G';
}

std::vector<DataPacketPtr> readDataPackets(const InputPortPtr& port)
{
    std::vector<DataPacketPtr> result;
    const auto packets = port.getConnection().dequeueAll();

    for (const auto& packet : packets)
    {
        if (packet.getType() != PacketType::Data)
            continue;

        const auto dataPacket = packet.asPtr<IDataPacket>(false);
        if (dataPacket.assigned())
            result.push_back(dataPacket);
    }

    return result;
}

// Frame arrival is genuinely asynchronous (the camera's self-clock thread pulls frames off the
// network on its own schedule), so tests poll with a timeout instead of driving generation
// deterministically.
std::vector<DataPacketPtr> waitForDataPackets(const InputPortPtr& port,
                                              size_t minCount,
                                              std::chrono::milliseconds timeout = std::chrono::seconds(15))
{
    std::vector<DataPacketPtr> result;
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (result.size() < minCount && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        for (auto& packet : readDataPackets(port))
            result.push_back(std::move(packet));
    }

    return result;
}

struct EncoderWithIpCamera
{
    ContextPtr context;
    FunctionBlockPtr encoder;
    SignalConfigPtr rawVideoSignal;
    InputPortPtr monitorPort;
    DevicePtr device;

    EncoderWithIpCamera() = default;
    EncoderWithIpCamera(EncoderWithIpCamera&&) = default;
    EncoderWithIpCamera& operator=(EncoderWithIpCamera&&) = default;

    // The device's self-clock thread keeps pushing frames through the scheduler independently
    // of the test's control flow. If it's still mid-flight when encoder's destructor runs (which
    // waits for its own scheduled callbacks to finish), teardown deadlocks against that still-
    // running producer. Stop the producer first, then drain the scheduler, *before* letting the
    // rest of the members destroy themselves in the normal (reverse-declaration) order.
    ~EncoderWithIpCamera()
    {
        device = nullptr;
        if (context.assigned())
            context.getScheduler().waitAll();
    }

    static EncoderWithIpCamera create()
    {
        EncoderWithIpCamera pipeline;
        pipeline.context = createContext();
        pipeline.device = createIpCameraDevice(pipeline.context);

        if (pipeline.device.getStatusContainer().getStatus("ComponentStatus") != ComponentStatus::Ok)
            return pipeline;

        pipeline.rawVideoSignal = findChannelSignal(pipeline.device, "Video_Physical");
        if (!pipeline.rawVideoSignal.assigned())
            return pipeline;

        pipeline.encoder = createVideoFrameEncoder(pipeline.context);
        pipeline.encoder.getInputPorts()[0].connect(pipeline.rawVideoSignal);

        pipeline.monitorPort = InputPort(pipeline.context, nullptr, "monitor");
        pipeline.monitorPort.connect(findFbSignal(pipeline.encoder, "value"));

        // Wait for the encoder to receive the source descriptor and set up its decode/encode
        // pipeline; unlike the mock this isn't guaranteed by a single waitAll() call.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (pipeline.encoder.getStatusContainer().getStatus("ComponentStatus") != ComponentStatus::Ok &&
               std::chrono::steady_clock::now() < deadline)
        {
            pipeline.context.getScheduler().waitAll();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        return pipeline;
    }

    bool valid() const
    {
        return rawVideoSignal.assigned() && monitorPort.assigned() &&
               encoder.assigned() && encoder.getStatusContainer().getStatus("ComponentStatus") == ComponentStatus::Ok;
    }
};
}  // namespace

TEST(VideoFrameEncoderFbImplTest, CreateType)
{
    const auto type = VideoFrameEncoderFbImpl::CreateType();
    ASSERT_TRUE(type.assigned());
    ASSERT_EQ(type.getId(), VideoFrameEncoderFbImpl::Id);
    ASSERT_EQ(type.getName(), VideoFrameEncoderFbImpl::Name);
}

TEST(VideoFrameEncoderFbImplTest, SupportedFrameFormats)
{
    const auto formats = VideoFrameEncoderFbImpl::GetSupportedFrameFormats();
    ASSERT_EQ(formats.getCount(), 6u);
    ASSERT_EQ(formats[0], "JPEG");
    ASSERT_EQ(formats[1], "PNG");
}

TEST(VideoFrameEncoderFbImplTest, ConnectIpCameraToEncoder)
{
    auto pipeline = EncoderWithIpCamera::create();
    if (!pipeline.valid())
        GTEST_SKIP() << "IP camera stream not reachable: " << IP_CAMERA_STREAM_URL;

    const auto outputValue = findFbSignal(pipeline.encoder, "value");
    ASSERT_TRUE(outputValue.assigned());
    ASSERT_EQ(outputValue.getDescriptor().getSampleType(), SampleType::Binary);

    // The stream is H.264, which never matches the encoder's JPEG/PNG/... output formats, so it
    // always takes the transcode (decode+encode) path rather than passthrough; transcoded output
    // descriptors carry the *output* format as their unit, not a "NativeCodecId" of the source.
    const auto unit = outputValue.getDescriptor().getUnit();
    ASSERT_TRUE(unit.assigned());
    ASSERT_EQ(unit.getSymbol(), "JPEG");
}

TEST(VideoFrameEncoderFbImplTest, TranscodesToJpeg)
{
    auto pipeline = EncoderWithIpCamera::create();
    if (!pipeline.valid())
        GTEST_SKIP() << "IP camera stream not reachable: " << IP_CAMERA_STREAM_URL;

    pipeline.encoder.setPropertyValue("FrameFormat", JPEG_FORMAT_INDEX);
    const auto packets = waitForDataPackets(pipeline.monitorPort, 1);

    ASSERT_GE(packets.size(), 1u);
    ASSERT_GT(packets[0].getRawDataSize(), 0u);
    ASSERT_TRUE(isJpeg(reinterpret_cast<const uint8_t*>(packets[0].getRawData()), packets[0].getRawDataSize()));
}

TEST(VideoFrameEncoderFbImplTest, TranscodesToPng)
{
    auto pipeline = EncoderWithIpCamera::create();
    if (!pipeline.valid())
        GTEST_SKIP() << "IP camera stream not reachable: " << IP_CAMERA_STREAM_URL;

    pipeline.encoder.setPropertyValue("FrameFormat", PNG_FORMAT_INDEX);
    const auto packets = waitForDataPackets(pipeline.monitorPort, 1);

    ASSERT_GE(packets.size(), 1u);
    ASSERT_GT(packets[0].getRawDataSize(), 0u);
    ASSERT_TRUE(isPng(reinterpret_cast<const uint8_t*>(packets[0].getRawData()), packets[0].getRawDataSize()));

    const auto unit = packets[0].getDataDescriptor().getUnit();
    ASSERT_TRUE(unit.assigned());
    ASSERT_EQ(unit.getSymbol(), "PNG");
}

TEST(VideoFrameEncoderFbImplTest, TranscodesMultipleFramesOverTime)
{
    auto pipeline = EncoderWithIpCamera::create();
    if (!pipeline.valid())
        GTEST_SKIP() << "IP camera stream not reachable: " << IP_CAMERA_STREAM_URL;

    pipeline.encoder.setPropertyValue("FrameFormat", JPEG_FORMAT_INDEX);
    const auto packets = waitForDataPackets(pipeline.monitorPort, 2, std::chrono::seconds(30));

    ASSERT_GE(packets.size(), 2u);
    for (const auto& packet : packets)
    {
        ASSERT_GT(packet.getRawDataSize(), 0u);
        ASSERT_TRUE(isJpeg(reinterpret_cast<const uint8_t*>(packet.getRawData()), packet.getRawDataSize()));
    }
}

TEST(VideoFrameEncoderFbImplTest, RejectsInputWithoutDomainSignal)
{
    const auto context = createContext();
    const auto encoder = createVideoFrameEncoder(context);
    const auto orphanSignal = Signal(context, nullptr, "orphan");
    orphanSignal.setDescriptor(DataDescriptorBuilder()
                                   .setSampleType(SampleType::Binary)
                                   .setName("Video")
                                   .setRule(ExplicitDataRule())
                                   .build());

    encoder.getInputPorts()[0].connect(orphanSignal);

    ASSERT_EQ(encoder.getStatusContainer().getStatus("ComponentStatus"), ComponentStatus::Error);
}
