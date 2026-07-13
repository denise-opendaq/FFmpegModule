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
#include <opendaq/event_packet_ptr.h>
#include <opendaq/data_packet_ptr.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE

class VideoFrameEncoderFbImpl final : public FunctionBlock
{
public:
    using Self = VideoFrameEncoderFbImpl;
    using Super = FunctionBlock;

    static constexpr auto Id = "VideoFrameEncoderFbId";
    static constexpr auto Name = "VideoFrameEncoder";

    VideoFrameEncoderFbImpl(const ContextPtr& context,
                            const ComponentPtr& parent,
                            const StringPtr& localId);

    static FunctionBlockTypePtr CreateType();
    static ListPtr<IString> GetSupportedFrameFormats();

protected:
    void initProperties();
    void initInputPorts();
    void initSignals();

    void onConnected(const InputPortPtr& port) override;
    void onDisconnected(const InputPortPtr& port) override;
    void onPacketReceived(const InputPortPtr& port) override;

    void handleDataPacket(const DataPacketPtr& packet);
    void handleEventPacket(const EventPacketPtr& packet);

    void updateFrameFormat(Int format);
    void updateFrameQuality(Int quality);
    void resetPipeline();
    bool shouldPassthrough() const;
    void processPassthrough(const DataPacketPtr& packet);
    void processTranscode(const DataPacketPtr& packet);

    void initDecoder();
    void initEncoder();
    void applyFrameQuality(AVDictionary** options);
    void applyInputDescriptor(const DataDescriptorPtr& dataDescriptor, const DataDescriptorPtr& domainDescriptor);
    bool prepareConvertedFrame(AVFrame* srcFrame, AVFrame*& outFrame);
    bool refillInputPacket(const DataPacketPtr& packet, int inputSize);

private:
    using AVFramePtr = std::unique_ptr<AVFrame, void (*)(AVFrame*)>;
    using AVCodecContextPtr = std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)>;
    using AVPacketPtr = std::unique_ptr<AVPacket, void (*)(AVPacket*)>;
    using SwsContextPtr = std::unique_ptr<SwsContext, void (*)(SwsContext*)>;

    static void av_frame_free_deleter(AVFrame* frame);
    static void avcodec_free_context_deleter(AVCodecContext* ctx);
    static void av_packet_free_deleter(AVPacket* pkt);
    static void sws_freeContext_deleter(SwsContext* ctx);

    InputPortPtr inputPort;
    SignalConfigPtr timeSignal;
    SignalConfigPtr valueSignal;

    Int nativeCodecId{0};
    Int nativePixelFormat{0};
    SizeT frameWidth{0};
    SizeT frameHeight{0};
    Float frameRate{0.0f};
    std::string nativeFormat;

    AVCodecID encodeCodecId{AV_CODEC_ID_MJPEG};
    AVPixelFormat swsFormat{AV_PIX_FMT_YUVJ420P};
    Int encodeQuality{0};
    std::string encodeFrameFormatName{"JPEG"};

    AVCodecContextPtr decodecCtx{nullptr, avcodec_free_context_deleter};
    AVCodecContextPtr encodecCtx{nullptr, avcodec_free_context_deleter};
    SwsContextPtr swsCtx{nullptr, sws_freeContext_deleter};
    AVFramePtr rawFrame{nullptr, av_frame_free_deleter};
    AVFramePtr swcFrame{nullptr, av_frame_free_deleter};
    AVPacketPtr encodedPacket{nullptr, av_packet_free_deleter};
    AVPacketPtr inputPacket{nullptr, av_packet_free_deleter};
    DataPacketPtr inputPacketHolder;

    int swsSrcWidth{0};
    int swsSrcHeight{0};
    AVPixelFormat swsSrcFormat{AV_PIX_FMT_NONE};

    bool pipelineReady{false};
};

END_NAMESPACE_VIDEO_DEVICE_MODULE
