#include "mock_camera_device.h"

#include <gtest/gtest.h>
#include <opendaq/input_port_factory.h>
#include <opendaq/opendaq.h>

#include <cstring>
#include <string>

extern "C"
{
#include <libavcodec/avcodec.h>
}

using namespace daq;
using namespace daq::modules::video_device_module_test;

namespace
{
ContextPtr createContext()
{
    const auto logger = Logger();
    return Context(Scheduler(logger), logger, TypeManager(), nullptr, nullptr);
}

DevicePtr createMockCameraDevice(const ContextPtr& context)
{
    return createWithImplementation<IDevice, MockCameraDeviceImpl>(context, nullptr, "MockCamera");
}

SignalConfigPtr findSignal(const DevicePtr& device, const std::string& localId)
{
    for (const auto& signal : device.getSignals())
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
}  // namespace

TEST(MockCameraDeviceTest, CreatesWithTimeAndVideoSignals)
{
    const auto context = createContext();
    const auto device = createMockCameraDevice(context);
    const auto mock = device.asPtr<IMockCamera>(true);

    ASSERT_TRUE(mock.assigned());
    ASSERT_EQ(device.getStatusContainer().getStatus("ComponentStatus"), ComponentStatus::Ok);

    const auto videoSignal = findSignal(device, "Video");
    ASSERT_TRUE(videoSignal.assigned());

    const auto timeSignal = videoSignal.getDomainSignal();
    ASSERT_TRUE(timeSignal.assigned());
    ASSERT_EQ(timeSignal.getLocalId(), "Time");
    ASSERT_EQ(videoSignal.getDomainSignal(), timeSignal);
    ASSERT_EQ(mock->getSamplesGenerated(), 0u);
}

TEST(MockCameraDeviceTest, GenerateFrameProducesMjpegPacket)
{
    const auto context = createContext();
    const auto device = createMockCameraDevice(context);
    const auto mock = device.asPtr<IMockCamera>(true);

    auto monitorPort = InputPort(context, nullptr, "monitor");
    monitorPort.connect(findSignal(device, "Video"));

    mock->generateFrame(1'000'000ULL);
    context.getScheduler().waitAll();

    ListPtr<IPacket> packets;
    ASSERT_NO_THROW(packets = monitorPort.getConnection().dequeueAll());

    DataPacketPtr dataPacket;
    for (const auto& packet : packets)
    {
        if (packet.getType() == PacketType::Data)
        {
            dataPacket = packet.asPtr<IDataPacket>(false);
            if (dataPacket.assigned())
                break;
        }
    }

    ASSERT_TRUE(dataPacket.assigned());
    ASSERT_EQ(dataPacket.getSampleCount(), 1u);
    ASSERT_GT(dataPacket.getRawDataSize(), 0u);
    ASSERT_TRUE(isJpeg(reinterpret_cast<const uint8_t*>(dataPacket.getRawData()), dataPacket.getRawDataSize()));
    ASSERT_EQ(mock->getSamplesGenerated(), 1u);
}

TEST(MockCameraDeviceTest, VideoDescriptorHasCodecMetadata)
{
    const auto context = createContext();
    const auto device = createMockCameraDevice(context);
    const auto videoSignal = findSignal(device, "Video");
    ASSERT_TRUE(videoSignal.assigned());

    const auto descriptor = videoSignal.getDescriptor();
    const auto metadata = descriptor.getMetadata();
    ASSERT_TRUE(metadata.assigned());

    StringPtr codecId;
    ASSERT_TRUE(metadata.tryGet("NativeCodecId", codecId));
    ASSERT_EQ(std::stoi(codecId.toStdString()), AV_CODEC_ID_MJPEG);
}

TEST(MockCameraDeviceTest, GenerateFrameIncrementsSampleCounter)
{
    const auto context = createContext();
    const auto device = createMockCameraDevice(context);
    const auto mock = device.asPtr<IMockCamera>(true);

    mock->generateFrame(0);
    mock->generateFrame(33'333'333ULL);
    mock->generateFrame(66'666'666ULL);
    context.getScheduler().waitAll();

    ASSERT_EQ(mock->getSamplesGenerated(), 3u);
}
