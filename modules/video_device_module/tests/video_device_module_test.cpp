#include <video_device_module/camera_device_impl.h>
#include <video_device_module/module_dll.h>

#include <gtest/gtest.h>
#include <opendaq/input_port_factory.h>
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

// Goes through the module's exported C entry point rather than instantiating VideoDeviceModule
// directly: internal implementation classes aren't exported from the module's shared library
// (visibility is hidden by default there; only the plugin's C ABI entry points are), so tests
// only ever reach the module through its public IModule/IDevice/IFunctionBlock surface.
ModulePtr CreateModule(const ContextPtr& context)
{
    ModulePtr module;
    createVideoDeviceModule(&module, context);
    return module;
}

bool isLocalDeviceConnectionString(const std::string& connectionString)
{
    static const std::string prefix = std::string(CAMERA_DEVICE_TYPE_CONNECTION_STRING_PREFIX) + "://device";
    return connectionString.rfind(prefix, 0) == 0;
}
}  // namespace

TEST(VideoDeviceModuleTest, GetAvailableDeviceTypesContainsCameraDevice)
{
    const auto module = CreateModule(createContext());

    DictPtr<IString, IDeviceType> deviceTypes;
    ASSERT_NO_THROW(deviceTypes = module.getAvailableDeviceTypes());
    ASSERT_TRUE(deviceTypes.assigned());

    ASSERT_TRUE(deviceTypes.hasKey(CAMERA_DEVICE_TYPE_ID));
    const DeviceTypePtr cameraType = deviceTypes.get(CAMERA_DEVICE_TYPE_ID);
    ASSERT_EQ(cameraType.getConnectionStringPrefix(), CAMERA_DEVICE_TYPE_CONNECTION_STRING_PREFIX);
}

TEST(VideoDeviceModuleTest, GetAvailableDevicesDoesNotThrow)
{
    const auto module = CreateModule(createContext());

    // Enumerates real cameras on the current OS (v4l2 on Linux, AVFoundation on macOS,
    // dshow on Windows), plus a fixed example IP camera entry. On machines without a local
    // camera this still returns that example, never an empty list.
    ListPtr<IDeviceInfo> devices;
    ASSERT_NO_THROW(devices = module.getAvailableDevices());
    ASSERT_TRUE(devices.assigned());

    for (const DeviceInfoPtr& info : devices)
    {
        ASSERT_TRUE(info.getDeviceType().assigned());
        ASSERT_EQ(info.getDeviceType().getId(), CAMERA_DEVICE_TYPE_ID);
        std::cout << "Found camera device: " << info.getName() << " (" << info.getConnectionString() << ")" << std::endl;

        const std::string connectionString = info.getConnectionString();
        ASSERT_EQ(connectionString.rfind(std::string(CAMERA_DEVICE_TYPE_CONNECTION_STRING_PREFIX) + "://", 0), 0u);
    }
}

TEST(VideoDeviceModuleTest, GetAvailableDevicesIncludesIpCameraExample)
{
    // Network cameras aren't locally enumerable, so the module always lists a fixed public RTSP
    // stream as an example network camera path (see VideoDeviceModule::onGetAvailableDevices).
    const std::string streamUrl = "rtsp://stream.strba.sk:1935/strba/VYHLAD_JAZERO.stream";
    const std::string expectedConnectionString = fmt::format("{}://{}", CAMERA_DEVICE_TYPE_CONNECTION_STRING_PREFIX, streamUrl);

    const auto module = CreateModule(createContext());

    ListPtr<IDeviceInfo> devices;
    ASSERT_NO_THROW(devices = module.getAvailableDevices());

    DeviceInfoPtr ipCameraInfo;
    for (const DeviceInfoPtr& info : devices)
    {
        if (info.getConnectionString() == expectedConnectionString)
        {
            ipCameraInfo = info;
            break;
        }
    }

    ASSERT_TRUE(ipCameraInfo.assigned());
    ASSERT_EQ(ipCameraInfo.getName(), streamUrl);
}

TEST(VideoDeviceModuleTest, SelfClockGeneratesFramesWithoutRootDevice)
{
    const auto context = createContext();
    const auto module = CreateModule(context);

    DeviceInfoPtr localDeviceInfo;
    for (const DeviceInfoPtr& info : module.getAvailableDevices())
    {
        if (isLocalDeviceConnectionString(info.getConnectionString()))
        {
            localDeviceInfo = info;
            break;
        }
    }
    if (!localDeviceInfo.assigned())
        GTEST_SKIP() << "No camera device available on this machine";

    // createContext() attaches no root device, so the device cannot find a parent time signal
    // and must fall back to self-clocking its own acquisition.
    const auto device = module.createDevice(localDeviceInfo.getConnectionString(), nullptr, PropertyObject());
    ASSERT_EQ(device.getStatusContainer().getStatus("ComponentStatus"), ComponentStatus::Ok) << "Local camera device could not be opened: " << localDeviceInfo.getConnectionString();

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

TEST(VideoDeviceModuleTest, IpCameraStreamProducesFrames)
{
    // A real public RTSP stream, used here purely as connectivity fixture. Skips rather than
    // fails if it's unreachable (no network in this environment, or the stream went down)
    // since this is an external dependency, not something this module controls.
    const std::string streamUrl = "rtsp://stream.strba.sk:1935/strba/VYHLAD_JAZERO.stream";
    const std::string connectionString = fmt::format("{}://{}", CAMERA_DEVICE_TYPE_CONNECTION_STRING_PREFIX, streamUrl);

    const auto context = createContext();
    const auto module = CreateModule(context);
    const auto device = module.createDevice(connectionString, nullptr, PropertyObject());

    if (device.getStatusContainer().getStatus("ComponentStatus") != ComponentStatus::Ok)
        GTEST_SKIP() << "IP camera stream not reachable: " << streamUrl;

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
    for (int i = 0; i < 100 && !receivedData; ++i)
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
