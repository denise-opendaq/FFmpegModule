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

#include <opendaq/binary_data_packet_factory.h>
#include <opendaq/deleter_factory.h>

#include <memory>
#include <mutex>
#include <unordered_map>

extern "C"
{
#include <libavcodec/avcodec.h>
}

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE

using AvPacketSharedPtr = std::shared_ptr<AVPacket>;

inline void avPacketSharedDeleter(AVPacket* pkt)
{
    av_packet_free(&pkt);
}

inline AvPacketSharedPtr makeAvPacketShared(AVPacket* pkt = nullptr)
{
    if (!pkt)
        pkt = av_packet_alloc();
    return AvPacketSharedPtr(pkt, avPacketSharedDeleter);
}

namespace detail
{
class AvPacketBindingRegistry
{
public:
    static AvPacketBindingRegistry& instance()
    {
        static AvPacketBindingRegistry registry;
        return registry;
    }

    void registerBinding(const void* data, const AvPacketSharedPtr& packet)
    {
        if (!data || !packet)
            return;

        std::lock_guard<std::mutex> lock(mutex);
        entries[data] = packet;
    }

    void unregisterBinding(const void* data)
    {
        if (!data)
            return;

        std::lock_guard<std::mutex> lock(mutex);
        entries.erase(data);
    }

    AvPacketSharedPtr lookup(const void* data)
    {
        if (!data)
            return nullptr;

        std::lock_guard<std::mutex> lock(mutex);
        const auto it = entries.find(data);
        if (it == entries.end())
            return nullptr;

        return it->second.lock();
    }

private:
    std::mutex mutex;
    std::unordered_map<const void*, std::weak_ptr<AVPacket>> entries;
};
}  // namespace detail

// Wraps FFmpeg-owned packet memory in an openDAQ binary packet without copying payload bytes.
inline DataPacketPtr binaryDataPacketFromAvPacket(const DataPacketPtr& domainPacket,
                                                 const DataDescriptorPtr& descriptor,
                                                 AvPacketSharedPtr packet)
{
    if (!packet || packet->size <= 0 || !packet->data)
        return nullptr;

    const void* dataKey = packet->data;
    detail::AvPacketBindingRegistry::instance().registerBinding(dataKey, packet);

    return BinaryDataPacketWithExternalMemory(
        domainPacket,
        descriptor,
        static_cast<UInt>(packet->size),
        packet->data,
        Deleter([packet = std::move(packet), dataKey](void*) {
            detail::AvPacketBindingRegistry::instance().unregisterBinding(dataKey);
        }));
}

// Binds a decoder input AVPacket to FFmpeg/openDAQ memory without copying compressed bytes.
inline bool bindDecoderInputFromDataPacket(AVPacket* dst, const DataPacketPtr& holder, int size)
{
    if (!dst || !holder.assigned() || size <= 0)
        return false;

    av_packet_unref(dst);

    if (const auto source = detail::AvPacketBindingRegistry::instance().lookup(holder.getRawData()))
    {
        if (av_packet_ref(dst, source.get()) == 0)
            return true;
    }

    dst->data = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(holder.getRawData()));
    dst->size = size;
    return true;
}

END_NAMESPACE_VIDEO_DEVICE_MODULE
