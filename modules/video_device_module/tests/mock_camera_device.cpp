#include "mock_camera_device.h"

#include <video_device_module/ffmpeg_av_packet_utils.h>

#include <opendaq/binary_data_packet_factory.h>
#include <opendaq/data_descriptor_factory.h>
#include <opendaq/deleter_factory.h>
#include <opendaq/device_domain_factory.h>
#include <opendaq/packet_factory.h>

#include <cstring>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

namespace
{
void sharedAvPacketDeleter(AVPacket* pkt)
{
    av_packet_free(&pkt);
}
}  // namespace

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE_TEST

MockCameraDeviceImpl::MockCameraDeviceImpl(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId)
    : Super(ctx, parent, localId, nullptr, "MockCamera")
{
    initComponentStatus();

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec)
    {
        setComponentStatusWithMessage(ComponentStatus::Error, "MJPEG encoder not found");
        return;
    }

    encodeCtx = avcodec_alloc_context3(codec);
    if (!encodeCtx)
    {
        setComponentStatusWithMessage(ComponentStatus::Error, "Failed to allocate MJPEG encoder context");
        return;
    }

    encodeCtx->width = static_cast<int>(frameWidth);
    encodeCtx->height = static_cast<int>(frameHeight);
    encodeCtx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    encodeCtx->time_base = {1, static_cast<int>(frameRateHz)};

    if (avcodec_open2(encodeCtx, codec, nullptr) < 0)
    {
        setComponentStatusWithMessage(ComponentStatus::Error, "Failed to open MJPEG encoder");
        return;
    }

    frame = av_frame_alloc();
    if (!frame)
    {
        setComponentStatusWithMessage(ComponentStatus::Error, "Failed to allocate video frame");
        return;
    }

    frame->format = AV_PIX_FMT_YUVJ420P;
    frame->width = encodeCtx->width;
    frame->height = encodeCtx->height;
    if (av_frame_get_buffer(frame, 32) < 0)
    {
        setComponentStatusWithMessage(ComponentStatus::Error, "Failed to allocate frame buffer");
        return;
    }

    initSignals();
    setComponentStatus(ComponentStatus::Ok);
}

MockCameraDeviceImpl::~MockCameraDeviceImpl()
{
    if (frame)
        av_frame_free(&frame);
    if (encodeCtx)
        avcodec_free_context(&encodeCtx);
}

void MockCameraDeviceImpl::initSignals()
{
    const auto unit = Unit("s", -1, "seconds", "time");
    deviceDomain = DeviceDomain(Ratio(1, 1'000'000'000), "1970-01-01T00:00:00Z", unit);
    setDeviceDomain(deviceDomain);

    timeSignal = createAndAddSignal("Time", createTimeSignalDescriptor(), false);
    videoSignal = createAndAddSignal("Video", createVideoSignalDescriptor());
    videoSignal.setDomainSignal(timeSignal);
}

DataDescriptorPtr MockCameraDeviceImpl::createTimeSignalDescriptor() const
{
    const auto frameRate = static_cast<Int>(frameRateHz + 0.5f);
    const UInt expectedDeltaTicks = static_cast<UInt>(1'000'000'000ULL / static_cast<uint64_t>(frameRate));
    const UInt minExpectedDelta = expectedDeltaTicks * 9 / 10;
    const UInt maxExpectedDelta = expectedDeltaTicks * 11 / 10;

    return DataDescriptorBuilder()
        .setSampleType(SampleType::UInt64)
        .setRule(ExplicitDomainDataRule(minExpectedDelta, maxExpectedDelta))
        .setTickResolution(deviceDomain.getTickResolution())
        .setOrigin(deviceDomain.getOrigin())
        .setUnit(deviceDomain.getUnit())
        .setName("Time")
        .build();
}

DataDescriptorPtr MockCameraDeviceImpl::createVideoSignalDescriptor() const
{
    const StringPtr formatName = "MJPEG";
    return DataDescriptorBuilder()
        .setSampleType(SampleType::Binary)
        .setName("Video")
        .setUnit(Unit(formatName, -1, formatName, "Video frame format"))
        .setRule(ExplicitDataRule())
        .setMetadata(Dict<IString, IString>({
            {"NativeCodecId", std::to_string(AV_CODEC_ID_MJPEG)},
            {"NativePixelFormat", std::to_string(AV_PIX_FMT_YUVJ420P)},
            {"FrameWidth", std::to_string(frameWidth)},
            {"FrameHeight", std::to_string(frameHeight)},
            {"FrameRateHz", std::to_string(frameRateHz)},
        }))
        .build();
}

bool MockCameraDeviceImpl::encodeNextFrame(std::shared_ptr<AVPacket>& out)
{
    if (!encodeCtx || !frame)
        return false;

    if (av_frame_make_writable(frame) < 0)
        return false;

    const int ySize = frame->linesize[0] * frame->height;
    const uint8_t shade = static_cast<uint8_t>((frameIndex * 17) % 200 + 32);
    std::memset(frame->data[0], shade, static_cast<size_t>(ySize));

    const int chromaHeight = frame->height / 2;
    const int uvSize = frame->linesize[1] * chromaHeight;
    std::memset(frame->data[1], 128 - static_cast<int>(shade % 64), static_cast<size_t>(uvSize));
    std::memset(frame->data[2], 128 + static_cast<int>(shade % 64), static_cast<size_t>(uvSize));

    frame->pts = static_cast<int64_t>(frameIndex);

    if (avcodec_send_frame(encodeCtx, frame) < 0)
        return false;

    auto packet = std::shared_ptr<AVPacket>(av_packet_alloc(), sharedAvPacketDeleter);
    if (!packet)
        return false;

    if (avcodec_receive_packet(encodeCtx, packet.get()) < 0)
        return false;

    ++frameIndex;
    out = std::move(packet);
    return true;
}

void MockCameraDeviceImpl::generateFrame(UInt timestamp)
{
    std::shared_ptr<AVPacket> packet;
    if (!encodeNextFrame(packet) || !packet || packet->size <= 0)
        return;

    auto timestamps = std::make_unique<uint64_t[]>(1);
    timestamps[0] = timestamp;

    const auto domainPacket = DataPacketWithExternalMemory(
        nullptr,
        timeSignal.getDescriptor(),
        1,
        timestamps.release(),
        Deleter([](void* ptr) { delete[] static_cast<uint64_t*>(ptr); }));

    timeSignal.sendPacket(domainPacket);

    auto avPacket = std::move(packet);
    videoSignal.sendPacket(daq::modules::video_device_module::binaryDataPacketFromAvPacket(
        domainPacket, videoSignal.getDescriptor(), std::move(avPacket)));
    ++samplesGenerated;
}

UInt MockCameraDeviceImpl::getSamplesGenerated() const
{
    return samplesGenerated;
}

END_NAMESPACE_VIDEO_DEVICE_MODULE_TEST
