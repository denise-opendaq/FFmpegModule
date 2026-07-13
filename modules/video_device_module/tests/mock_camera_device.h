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

#include "common.h"

#include <opendaq/device_impl.h>

#include <opendaq/signal_config_ptr.h>

#include <memory>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE_TEST

DECLARE_OPENDAQ_INTERFACE(IMockCamera, IBaseObject)
{
    virtual void generateFrame(UInt timestamp) = 0;
    virtual UInt getSamplesGenerated() const = 0;
};

class MockCameraDeviceImpl final : public DeviceBase<IDevice, IMockCamera>
{
public:
    using Self = MockCameraDeviceImpl;
    using Super = DeviceBase<IDevice, IMockCamera>;

    MockCameraDeviceImpl(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId);
    ~MockCameraDeviceImpl() override;

    void generateFrame(UInt timestamp) override;
    UInt getSamplesGenerated() const override;

    SignalConfigPtr getTimeSignal() const
    {
        return timeSignal;
    }

    SignalConfigPtr getVideoSignal() const
    {
        return videoSignal;
    }

protected:
    void initSignals();

    DataDescriptorPtr createTimeSignalDescriptor() const;
    DataDescriptorPtr createVideoSignalDescriptor() const;

    bool encodeNextFrame(std::shared_ptr<AVPacket>& out);

private:
    SignalConfigPtr timeSignal;
    SignalConfigPtr videoSignal;
    DeviceDomainPtr deviceDomain;
    UInt samplesGenerated{0};

    static constexpr SizeT frameWidth = 64;
    static constexpr SizeT frameHeight = 48;
    static constexpr Float frameRateHz = 30.0f;

    AVCodecContext* encodeCtx{nullptr};
    AVFrame* frame{nullptr};
    uint64_t frameIndex{0};
};

END_NAMESPACE_VIDEO_DEVICE_MODULE_TEST
