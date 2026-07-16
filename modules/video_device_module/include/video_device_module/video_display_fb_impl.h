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

#include <opendaq/function_block_impl.h>
#include <opendaq/data_packet_ptr.h>
#include <opendaq/event_packet_ptr.h>

#include <memory>

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE

class VideoDisplayFbImpl final : public FunctionBlock
{
public:
    using Self = VideoDisplayFbImpl;
    using Super = FunctionBlock;

    static constexpr auto Id = "VideoDisplayFbId";
    static constexpr auto Name = "VideoDisplay";

    VideoDisplayFbImpl(const ContextPtr& context,
                       const ComponentPtr& parent,
                       const StringPtr& localId);
    ~VideoDisplayFbImpl() override;

    static FunctionBlockTypePtr CreateType();

protected:
    void onConnected(const InputPortPtr& port) override;
    void onDisconnected(const InputPortPtr& port) override;
    void onPacketReceived(const InputPortPtr& port) override;

private:
    void handleDataPacket(const DataPacketPtr& packet);
    void handleEventPacket(const EventPacketPtr& packet);
    void startRender();

    struct Impl;
    std::unique_ptr<Impl> impl;
};

END_NAMESPACE_VIDEO_DEVICE_MODULE
