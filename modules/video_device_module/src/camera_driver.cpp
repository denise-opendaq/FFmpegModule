#include <video_device_module/camera_driver.h>
#include <video_device_module/camera_platform.h>

#include <cctype>
#include <string_view>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE

CameraDriver::CameraDriver() = default;

CameraDriver::~CameraDriver()
{
    close();
}

void CameraDriver::avformat_free_context_deleter(AVFormatContext* ctx)
{
    avformat_close_input(&ctx);
}

void CameraDriver::av_packet_free_deleter(AVPacket* pkt)
{
    av_packet_free(&pkt);
}

void CameraDriver::shared_av_packet_deleter(AVPacket* pkt)
{
    av_packet_free(&pkt);
}

std::string CameraDriver::formatNativeCodecName(int codecId, int pixelFormat)
{
    const AVCodec* codec = avcodec_find_decoder(static_cast<AVCodecID>(codecId));
    if (codec && codec->name)
    {
        std::string name = codec->name;
        if (codecId == AV_CODEC_ID_RAWVIDEO)
        {
            const char* pixName = av_get_pix_fmt_name(static_cast<AVPixelFormat>(pixelFormat));
            if (pixName)
                name = pixName;
        }
        for (auto& ch : name)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return name;
    }
    return "BINARY";
}

uint64_t CameraDriver::packetPtsToNs(int64_t pts, int num, int den)
{
    if (pts == AV_NOPTS_VALUE || den == 0)
        return 0;

    const auto seconds = av_q2d(AVRational{num, den}) * static_cast<double>(pts);
    return static_cast<uint64_t>(seconds * 1'000'000'000.0 + 0.5);
}

bool CameraDriver::open(const std::string& devicePath)
{
    close();

    const bool isNetworkStream = isNetworkCameraPath(devicePath);

    // Network streams (rtsp://, http://, ...) are handled entirely by libavformat's own
    // protocol/format probing; local capture devices need libavdevice's platform backend
    // (v4l2/avfoundation/dshow) named explicitly.
    const AVInputFormat* inputFmt = nullptr;
    if (!isNetworkStream)
    {
        avdevice_register_all();
        inputFmt = av_find_input_format(cameraInputFormatName());
        if (!inputFmt)
            return false;
    }

    AVFormatContext* rawCtx = avformat_alloc_context();
    if (!rawCtx)
        return false;

    rawCtx->flags |= AVFMT_FLAG_NONBLOCK;

    AVDictionary* options = nullptr;
    if (isNetworkStream)
    {
        avformat_network_init();

        // UDP (ffmpeg's rtsp default) is frequently blocked by NAT/firewalls; TCP is the
        // reliable default for a demuxer that isn't going to retry/reconnect on its own.
        if (devicePath.rfind("rtsp://", 0) == 0)
            av_dict_set(&options, "rtsp_transport", "tcp", 0);
    }
    else if (std::string_view(cameraInputFormatName()) == "avfoundation")
    {
        // libavdevice's avfoundation backend defaults to NTSC's 29.97 fps, which many cameras
        // (webcams in particular) only expose as an integer-fps mode and reject outright.
        av_dict_set(&options, "framerate", "30", 0);
    }

    const int openResult = avformat_open_input(&rawCtx, devicePath.c_str(), inputFmt, &options);
    av_dict_free(&options);

    if (openResult < 0)
    {
        avformat_free_context(rawCtx);
        return false;
    }

    fmtCtx.reset(rawCtx);

    if (avformat_find_stream_info(fmtCtx.get(), nullptr) < 0)
    {
        close();
        return false;
    }

    streamIndex = av_find_best_stream(fmtCtx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (streamIndex < 0)
    {
        close();
        return false;
    }

    AVStream* stream = fmtCtx->streams[streamIndex];
    AVCodecParameters* codecpar = stream->codecpar;

    if (codecpar->width == 0 || codecpar->height == 0)
    {
        close();
        return false;
    }

    const AVRational framerate = av_guess_frame_rate(fmtCtx.get(), stream, nullptr);
    const Float frameRateHz = av_q2d(framerate);
    if (frameRateHz <= 0.0f)
    {
        close();
        return false;
    }

    streamInfo.width = static_cast<SizeT>(codecpar->width);
    streamInfo.height = static_cast<SizeT>(codecpar->height);
    streamInfo.frameRateHz = frameRateHz;
    streamInfo.nativeCodecId = codecpar->codec_id;
    streamInfo.nativePixelFormat = codecpar->format;
    streamInfo.nativeFormat = formatNativeCodecName(codecpar->codec_id, codecpar->format);

    readPacket.reset(av_packet_alloc());
    if (!readPacket)
    {
        close();
        return false;
    }

    return true;
}

void CameraDriver::close()
{
    readPacket.reset();
    fmtCtx.reset();
    streamIndex = -1;
    streamInfo = {};
}

const AVCodecParameters* CameraDriver::getCodecParameters() const
{
    if (!fmtCtx || streamIndex < 0)
        return nullptr;
    return fmtCtx->streams[streamIndex]->codecpar;
}

size_t CameraDriver::readFrames(size_t maxCount, std::vector<CapturedFrame>& out)
{
    if (!fmtCtx || !readPacket || maxCount == 0)
        return 0;

    size_t read = 0;
    while (read < maxCount)
    {
        const int ret = av_read_frame(fmtCtx.get(), readPacket.get());
        if (ret == AVERROR(EAGAIN))
            break;
        if (ret < 0)
            break;

        if (readPacket->stream_index != streamIndex)
        {
            av_packet_unref(readPacket.get());
            continue;
        }

        if (readPacket->size <= 0)
        {
            av_packet_unref(readPacket.get());
            continue;
        }

        CapturedFrame frame;
        auto framePacket = std::shared_ptr<AVPacket>(av_packet_alloc(), shared_av_packet_deleter);
        if (!framePacket)
        {
            av_packet_unref(readPacket.get());
            continue;
        }

        av_packet_move_ref(framePacket.get(), readPacket.get());
        frame.packet = std::move(framePacket);

        const AVRational timeBase = fmtCtx->streams[streamIndex]->time_base;
        frame.timestampNs = packetPtsToNs(frame.packet->pts, timeBase.num, timeBase.den);

        out.push_back(std::move(frame));
        ++read;
    }

    return read;
}

void CameraDriver::flushBuffer(size_t maxFrames)
{
    if (!fmtCtx || !readPacket)
        return;

    size_t flushed = 0;
    while (flushed < maxFrames)
    {
        const int ret = av_read_frame(fmtCtx.get(), readPacket.get());
        if (ret == AVERROR(EAGAIN))
            break;
        if (ret < 0)
            break;

        if (readPacket->stream_index == streamIndex)
            ++flushed;

        av_packet_unref(readPacket.get());
    }
}

END_NAMESPACE_VIDEO_DEVICE_MODULE
