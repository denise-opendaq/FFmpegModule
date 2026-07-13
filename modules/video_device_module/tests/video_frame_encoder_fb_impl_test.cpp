#include <video_device_module/video_frame_encoder_fb_impl.h>

#include "mock_camera_device.h"

#include <opendaq/binary_data_packet_factory.h>
#include <opendaq/reference_domain_info_factory.h>

#include <gtest/gtest.h>
#include <opendaq/function_block_impl.h>
#include <opendaq/input_port_factory.h>
#include <opendaq/opendaq.h>

#include <cstring>
#include <string>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
}

using namespace daq;
using namespace daq::modules::video_device_module;
using namespace daq::modules::video_device_module_test;

namespace
{
constexpr int JPEG_FORMAT_INDEX = 0;
constexpr int PNG_FORMAT_INDEX = 1;

ContextPtr createContext()
{
    const auto logger = Logger();
    return Context(Scheduler(logger), logger, TypeManager(), nullptr, nullptr);
}

FunctionBlockPtr createVideoFrameEncoder(const ContextPtr& context)
{
    return createWithImplementation<IFunctionBlock, VideoFrameEncoderFbImpl>(context, nullptr, "VideoFrameEncoder");
}

DevicePtr createMockCameraDevice(const ContextPtr& context)
{
    return createWithImplementation<IDevice, MockCameraDeviceImpl>(context, nullptr, "MockCamera");
}

SignalConfigPtr findDeviceSignal(const DevicePtr& device, const std::string& localId)
{
    for (const auto& signal : device.getSignals())
    {
        if (signal.getLocalId() == localId)
            return signal;
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

struct EncoderWithMockCamera
{
    ContextPtr context;
    DevicePtr device;
    FunctionBlockPtr encoder;
    ObjectPtr<IMockCamera> mock;
    SignalConfigPtr videoSignal;
    InputPortPtr monitorPort;

    static EncoderWithMockCamera create()
    {
        EncoderWithMockCamera pipeline;
        pipeline.context = createContext();
        pipeline.device = createMockCameraDevice(pipeline.context);
        pipeline.encoder = createVideoFrameEncoder(pipeline.context);
        pipeline.mock = pipeline.device.asPtr<IMockCamera>(true);
        pipeline.videoSignal = findDeviceSignal(pipeline.device, "Video");

        if (!pipeline.mock.assigned() || !pipeline.videoSignal.assigned())
            return pipeline;

        pipeline.encoder.getInputPorts()[0].connect(pipeline.videoSignal);
        pipeline.context.getScheduler().waitAll();

        pipeline.monitorPort = InputPort(pipeline.context, nullptr, "monitor");
        pipeline.monitorPort.connect(findFbSignal(pipeline.encoder, "value"));
        return pipeline;
    }

    bool valid() const
    {
        return mock.assigned() && videoSignal.assigned() && monitorPort.assigned();
    }

    std::vector<DataPacketPtr> generateAndRead(UInt timestamp, int frameFormat = JPEG_FORMAT_INDEX)
    {
        encoder.setPropertyValue("FrameFormat", frameFormat);
        mock->generateFrame(timestamp);
        context.getScheduler().waitAll();
        return readDataPackets(monitorPort);
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

TEST(VideoFrameEncoderFbImplTest, ConnectMockCameraToEncoder)
{
    auto pipeline = EncoderWithMockCamera::create();
    ASSERT_TRUE(pipeline.valid());
    ASSERT_EQ(pipeline.encoder.getStatusContainer().getStatus("ComponentStatus"), ComponentStatus::Ok);

    const auto outputValue = findFbSignal(pipeline.encoder, "value");
    ASSERT_TRUE(outputValue.assigned());
    ASSERT_EQ(outputValue.getDescriptor().getSampleType(), SampleType::Binary);

    const auto metadata = outputValue.getDescriptor().getMetadata();
    ASSERT_TRUE(metadata.assigned());
    StringPtr codecId;
    ASSERT_TRUE(metadata.tryGet("NativeCodecId", codecId));
    ASSERT_EQ(std::stoi(codecId.toStdString()), AV_CODEC_ID_MJPEG);
}

TEST(VideoFrameEncoderFbImplTest, PassthroughMjpegWhenOutputFormatIsJpeg)
{
    auto pipeline = EncoderWithMockCamera::create();
    ASSERT_TRUE(pipeline.valid());
    const auto packets = pipeline.generateAndRead(1'000'000ULL, JPEG_FORMAT_INDEX);

    ASSERT_EQ(packets.size(), 1u);
    ASSERT_EQ(packets[0].getSampleCount(), 1u);
    ASSERT_GT(packets[0].getRawDataSize(), 0u);
    ASSERT_TRUE(isJpeg(reinterpret_cast<const uint8_t*>(packets[0].getRawData()), packets[0].getRawDataSize()));
    ASSERT_EQ(pipeline.mock->getSamplesGenerated(), 1u);
}

TEST(VideoFrameEncoderFbImplTest, TranscodesMjpegToPng)
{
    auto pipeline = EncoderWithMockCamera::create();
    ASSERT_TRUE(pipeline.valid());
    const auto packets = pipeline.generateAndRead(2'000'000ULL, PNG_FORMAT_INDEX);

    ASSERT_EQ(packets.size(), 1u);
    ASSERT_EQ(packets[0].getSampleCount(), 1u);
    ASSERT_GT(packets[0].getRawDataSize(), 0u);
    ASSERT_TRUE(isPng(reinterpret_cast<const uint8_t*>(packets[0].getRawData()), packets[0].getRawDataSize()));

    const auto unit = packets[0].getDataDescriptor().getUnit();
    ASSERT_TRUE(unit.assigned());
    ASSERT_EQ(unit.getSymbol(), "PNG");
}

TEST(VideoFrameEncoderFbImplTest, ForwardsMultipleFramesInPassthroughMode)
{
    auto pipeline = EncoderWithMockCamera::create();
    ASSERT_TRUE(pipeline.valid());

    const auto first = pipeline.generateAndRead(0, JPEG_FORMAT_INDEX);
    const auto second = pipeline.generateAndRead(33'333'333ULL, JPEG_FORMAT_INDEX);

    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(second.size(), 1u);
    ASSERT_TRUE(isJpeg(reinterpret_cast<const uint8_t*>(first[0].getRawData()), first[0].getRawDataSize()));
    ASSERT_TRUE(isJpeg(reinterpret_cast<const uint8_t*>(second[0].getRawData()), second[0].getRawDataSize()));
    ASSERT_EQ(pipeline.mock->getSamplesGenerated(), 2u);
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

