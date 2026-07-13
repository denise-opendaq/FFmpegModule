/*
 * Copyright 2022-2026 openDAQ d.o.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <video_device_module/common.h>
#include <video_device_module/camera_driver.h>
#include <video_device_module/camera_channel_impl.h>
#include <video_device_module/time_accumulator.h>

#include <opendaq/device_impl.h>
#include <opendaq/event_packet_ptr.h>
#include <opendaq/input_port_config_ptr.h>
#include <opendaq/packet_reader_ptr.h>

#include <condition_variable>
#include <thread>

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE


static constexpr auto CAMERA_DEVICE_TYPE_ID = "CameraDevice";
static constexpr auto CAMERA_DEVICE_TYPE_NAME = "Camera Device";
static constexpr auto CAMERA_DEVICE_TYPE_DESCRIPTION = "";
static constexpr auto CAMERA_DEVICE_TYPE_CONNECTION_STRING_PREFIX = "camera";

class CameraDeviceImpl final : public Device
{
public:
    using Self = CameraDeviceImpl;
    using Super = Device;

    explicit CameraDeviceImpl(const StringPtr& cameraPath,
                              const PropertyObjectPtr& config,
                              const ContextPtr& ctx,
                              const ComponentPtr& parent,
                              const StringPtr& localId,
                              const StringPtr& name);
    ~CameraDeviceImpl() override;

    uint64_t onGetTicksSinceOrigin() override;

    static PropertyObjectPtr CreateDefaultDeviceConfig();
    static DeviceTypePtr CreateType();
    static DeviceInfoPtr CreateDeviceInfo(const std::string& cameraPath);
    static std::string GetCameraPath(const std::string& connectionString);

    DeviceInfoPtr onGetInfo() override;

protected:
    void initProperties();
    void initSignals();
    void initChannels();

    void onPacketReceived();
    void handleEventPacket(const EventPacketPtr& packet);
    void handleDataPacket(const DataPacketPtr& packet);
    void generatePacket(SizeT sampleCount);

    // Fallback trigger used when no parent device time signal is found: reads and
    // publishes frames on its own timer instead of being clocked by the root domain signal.
    void startSelfClock();
    void stopSelfClock();
    void selfClockLoop();

    void activeChanged() override;
    void onTimeOffsetChanged(Int offset);

private:
    DataDescriptorPtr createTimeSignalDescriptor();
    RatioPtr getRatio() const;

    std::string cameraPath;
    CameraDriver driver;

    Int sourceDelta{0};
    InputPortConfigPtr deviceTimeInputPort;
    SignalConfigPtr timeSignal;
    SignalPtr rootDomainSignal;
    PacketReaderPtr reader;
    TimeAccumulator timeAccumulator;

    ObjectPtr<IVideoChannel> channel;
    Int timeOffsetNs{0};
    DeviceDomainPtr deviceDomain;

    bool needsSelfClock{false};
    std::thread selfClockThread;
    std::condition_variable selfClockCv;
    bool stopSelfClockRequested{false};
};

END_NAMESPACE_VIDEO_DEVICE_MODULE
