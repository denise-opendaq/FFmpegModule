#include <video_device_module/camera_device_impl.h>
#include <video_device_module/camera_platform.h>
#include <video_device_module/video_device_module_impl.h>

#include <gtest/gtest.h>
#include <opendaq/input_port_factory.h>
#include <opendaq/module_impl.h>
#include <opendaq/opendaq.h>

#include <chrono>
#include <thread>

using namespace daq;
using namespace daq::modules::video_device_module;

namespace
{
ContextPtr createContext()
{
    const auto logger = Logger();
    return Context(Scheduler(logger), logger, TypeManager(), nullptr, nullptr);
}

ModulePtr createVideoDeviceModule(const ContextPtr& context)
{
    return createWithImplementation<IModule, VideoDeviceModule>(context);
}
}  // namespace

TEST(VideoDeviceModuleTest, GetAvailableDeviceTypesContainsCameraDevice)
{
    const auto module = createVideoDeviceModule(createContext());

    DictPtr<IString, IDeviceType> deviceTypes;
    ASSERT_NO_THROW(deviceTypes = module.getAvailableDeviceTypes());
    ASSERT_TRUE(deviceTypes.assigned());

    ASSERT_TRUE(deviceTypes.hasKey(CAMERA_DEVICE_TYPE_ID));
    const DeviceTypePtr cameraType = deviceTypes.get(CAMERA_DEVICE_TYPE_ID);
    ASSERT_EQ(cameraType.getConnectionStringPrefix(), CAMERA_DEVICE_TYPE_CONNECTION_STRING_PREFIX);
}

TEST(VideoDeviceModuleTest, GetAvailableDevicesDoesNotThrow)
{
    const auto module = createVideoDeviceModule(createContext());

    // Enumerates real cameras on the current OS (v4l2 on Linux, AVFoundation on macOS,
    // dshow on Windows). On machines without a camera this returns an empty, but valid, list.
    ListPtr<IDeviceInfo> devices;
    ASSERT_NO_THROW(devices = module.getAvailableDevices());
    ASSERT_TRUE(devices.assigned());

    for (const DeviceInfoPtr& info : devices)
    {
        const std::string connectionString = info.getConnectionString();
        ASSERT_EQ(connectionString.rfind(std::string(CAMERA_DEVICE_TYPE_CONNECTION_STRING_PREFIX) + "://device", 0), 0u);
        ASSERT_TRUE(info.getDeviceType().assigned());
        ASSERT_EQ(info.getDeviceType().getId(), CAMERA_DEVICE_TYPE_ID);
    }
}

TEST(VideoDeviceModuleTest, SelfClockGeneratesFramesWithoutRootDevice)
{
    const auto devices = listCameraDevices();
    if (devices.empty())
        GTEST_SKIP() << "No camera device available on this machine";

    // createContext() attaches no root device, so CameraDeviceImpl cannot find a parent
    // time signal and must fall back to self-clocking its own acquisition.
    const auto context = createContext();
    const auto device = createWithImplementation<IDevice, CameraDeviceImpl>(
        StringPtr(devices.front().path), CameraDeviceImpl::CreateDefaultDeviceConfig(), context, nullptr, "SelfClockCamera", "SelfClockCamera");

    ASSERT_EQ(device.getStatusContainer().getStatus("ComponentStatus"), ComponentStatus::Ok);

    SignalConfigPtr rawVideoSignal;
    for (const auto& ch : device.getChannels())
    {
        for (const auto& sig : ch.getSignals())
        {
            if (sig.getLocalId() == "Video_Physical")
                rawVideoSignal = sig;
        }
    }
    ASSERT_TRUE(rawVideoSignal.assigned());

    auto monitorPort = InputPort(context, nullptr, "monitor");
    monitorPort.connect(rawVideoSignal);

    bool receivedData = false;
    for (int i = 0; i < 50 && !receivedData; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        for (const auto& packet : monitorPort.getConnection().dequeueAll())
        {
            if (packet.getType() == PacketType::Data)
            {
                receivedData = true;
                break;
            }
        }
    }

    ASSERT_TRUE(receivedData);
}

TEST(VideoDeviceModuleTest, GetCameraPathRoundTripsConnectionString)
{
    const std::string devicePath = "/dev/video0";
    const std::string connectionString = fmt::format("{}://device{}", CAMERA_DEVICE_TYPE_CONNECTION_STRING_PREFIX, devicePath);

    ASSERT_EQ(CameraDeviceImpl::GetCameraPath(connectionString), devicePath);
}

TEST(VideoDeviceModuleTest, GetCameraPathRoundTripsIpConnectionString)
{
    const std::string streamUrl = "rtsp://192.168.1.50:554/stream1";
    const std::string connectionString = fmt::format("{}://ip/{}", CAMERA_DEVICE_TYPE_CONNECTION_STRING_PREFIX, streamUrl);

    const std::string path = CameraDeviceImpl::GetCameraPath(connectionString);
    ASSERT_EQ(path, streamUrl);
    ASSERT_TRUE(isNetworkCameraPath(path));
}

TEST(VideoDeviceModuleTest, CreateDeviceInfoForIpCameraDoesNotRequireEnumeration)
{
    const std::string streamUrl = "rtsp://192.168.1.50:554/stream1";

    const auto info = CameraDeviceImpl::CreateDeviceInfo(streamUrl);
    ASSERT_TRUE(info.assigned());
    ASSERT_EQ(info.getConnectionString(),
              fmt::format("{}://ip/{}", CAMERA_DEVICE_TYPE_CONNECTION_STRING_PREFIX, streamUrl));
    ASSERT_EQ(info.getName(), streamUrl);
}
// rtsp://stream.strba.sk:1935/strba/VYHLAD_JAZERO.stream