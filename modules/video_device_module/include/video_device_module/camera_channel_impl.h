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

#include <opendaq/channel_impl.h>
#include <opendaq/data_packet_ptr.h>

#include <vector>

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE

DECLARE_OPENDAQ_INTERFACE(IVideoChannel, IBaseObject)
{
    virtual void generatePacket(IDataPacket* domainPacket, std::vector<CapturedFrame>& frames) = 0;
};

class CameraChannelImpl final : public ChannelImpl<IVideoChannel>
{
public:
    using Self = CameraChannelImpl;
    using Super = ChannelImpl<IVideoChannel>;

    explicit CameraChannelImpl(const ContextPtr& ctx,
                               const ComponentPtr& parent,
                               const StringPtr& localId,
                               const SignalPtr& domainSignal,
                               const SignalPtr& rootDomainSignal,
                               CameraDriver& driver);

    void generatePacket(IDataPacket* domainPacket, std::vector<CapturedFrame>& frames) override;

protected:
    void initSignals(const SignalPtr& domainSignal);
    DataDescriptorPtr createVideoSignalDescriptor();

private:
    CameraDriver& driver;
    SignalPtr rootDomainSignal;

    SignalConfigPtr rawVideoSignal;
    SignalConfigPtr videoSignal;
    SignalConfigPtr timeSignal;
};

END_NAMESPACE_VIDEO_DEVICE_MODULE
