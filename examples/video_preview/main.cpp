#include <opendaq/opendaq.h>

#include <chrono>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

using namespace daq;

namespace
{
constexpr const char* VIDEO_FRAME_ENCODER_FB_ID = "VideoFrameEncoderFbId";
constexpr const char* VIDEO_DISPLAY_FB_ID = "VideoDisplayFbId";
constexpr int JPEG_FORMAT_INDEX = 0;

SignalPtr findChannelSignal(const DevicePtr& device, const std::string& localId)
{
    auto signals = device.getSignalsRecursive(search::LocalId(localId));
    if (signals.assigned() && signals.getCount() > 0)
        return signals[0];
    return {};
}

SignalPtr findFbSignal(const FunctionBlockPtr& fb, const std::string& localId)
{
    auto signals = fb.getSignals(search::LocalId(localId));
    if (signals.assigned() && signals.getCount() > 0)
        return signals[0];
    return {};
}

}  // namespace

int main(int /*argc*/, const char* /*argv*/[])
{
    const InstancePtr instance = InstanceBuilder().setModulePath(MODULE_PATH).setUsingSchedulerMainLoop(true).build();

    const ListPtr<IDeviceInfo> devices = instance.getAvailableDevices();
    if (!devices.assigned() || devices.getCount() == 0)
    {
        std::cout << "No camera devices found.\n";
        return 1;
    }

    std::cout << "Available devices:\n";
    for (SizeT i = 0; i < devices.getCount(); ++i)
    {
        const DeviceInfoPtr info = devices[i];
        std::cout << "  [" << i << "] " << info.getName() << " (" << info.getConnectionString() << ")\n";
    }

    std::cout << "\nSelect device index: ";
    std::size_t selectedIndex = 0;
    if (!(std::cin >> selectedIndex) || selectedIndex >= devices.getCount())
    {
        std::cout << "Invalid selection.\n";
        return 1;
    }

    const DevicePtr device = instance.addDevice(devices[selectedIndex].getConnectionString());
    if (!device.assigned())
    {
        std::cerr << "addDevice failed.\n";
        return 1;
    }

    for (const auto& property : device.getAllProperties())
        std::cout << "  " << property.getName() << " = " << property.getValue() << "\n";

    const SignalPtr rawVideoSignal = findChannelSignal(device, "Video_Physical");
    if (!rawVideoSignal.assigned())
    {
        std::cerr << "Video_Physical signal not found on the selected device.\n";
        return 1;
    }

    FunctionBlockPtr encoder = instance.addFunctionBlock(VIDEO_FRAME_ENCODER_FB_ID);
    encoder.getInputPorts()[0].connect(rawVideoSignal);

    const SignalPtr jpegSignal = findFbSignal(encoder, "value");
    if (!jpegSignal.assigned())
    {
        std::cerr << "Encoder output signal 'value' not found.\n";
        return 1;
    }

    FunctionBlockPtr display = instance.addFunctionBlock(VIDEO_DISPLAY_FB_ID);
    display.getInputPorts()[0].connect(jpegSignal);

    std::cout << "Video preview is running. Close the window to exit.\n";
    instance.getContext().getScheduler().runMainLoop();
    return 0;
}
