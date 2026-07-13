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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct AVPacket;
struct AVFormatContext;
struct AVCodecParameters;

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE

struct CameraStreamInfo
{
    SizeT width{0};
    SizeT height{0};
    Float frameRateHz{0.0f};
    std::string nativeFormat;
    int nativeCodecId{0};
    int nativePixelFormat{0};
};

struct CapturedFrame
{
    std::shared_ptr<AVPacket> packet;
    uint64_t timestampNs{0};
};

class CameraDriver
{
public:
    CameraDriver();
    ~CameraDriver();

    CameraDriver(const CameraDriver&) = delete;
    CameraDriver& operator=(const CameraDriver&) = delete;

    bool open(const std::string& devicePath);
    void close();

    bool isOpen() const
    {
        return fmtCtx != nullptr;
    }

    const CameraStreamInfo& getStreamInfo() const
    {
        return streamInfo;
    }

    const AVCodecParameters* getCodecParameters() const;

    size_t readFrames(size_t maxCount, std::vector<CapturedFrame>& out);
    void flushBuffer(size_t maxFrames);

private:
    using AVFormatContextPtr = std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)>;
    using AVPacketPtr = std::unique_ptr<AVPacket, void (*)(AVPacket*)>;

    static void avformat_free_context_deleter(AVFormatContext* ctx);
    static void av_packet_free_deleter(AVPacket* pkt);
    static void shared_av_packet_deleter(AVPacket* pkt);

    static std::string formatNativeCodecName(int codecId, int pixelFormat);
    static uint64_t packetPtsToNs(int64_t pts, int num, int den);

    AVFormatContextPtr fmtCtx{nullptr, avformat_free_context_deleter};
    AVPacketPtr readPacket{nullptr, av_packet_free_deleter};
    CameraStreamInfo streamInfo;
    int streamIndex{-1};
};

END_NAMESPACE_VIDEO_DEVICE_MODULE
