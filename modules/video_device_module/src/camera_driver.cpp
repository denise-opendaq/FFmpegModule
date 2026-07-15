#include <video_device_module/camera_driver.h>
#include <video_device_module/camera_platform.h>

#include <coretypes/ratio_factory.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <string_view>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

namespace
{
// libavformat/libavdevice error codes for a failed device open are frequently a generic
// "unknown error"; the actionable detail (e.g. the HRESULT-derived reason dshow logs when it
// can't build its capture graph) only appears in the backend's own av_log() line. Capture the
// last warning/error line here so CameraDriver::open() can surface it.
thread_local std::string g_lastAvLogLine;

void avLogCaptureCallback(void* avcl, int level, const char* fmt, va_list args)
{
    if (level > AV_LOG_WARNING)
        return;

    char buffer[512];
    vsnprintf(buffer, sizeof(buffer), fmt, args);

    std::string_view msg(buffer);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
        msg.remove_suffix(1);

    if (!msg.empty())
        g_lastAvLogLine.assign(msg);
}

std::string describeAvError(int errnum)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, buffer, sizeof(buffer));

    if (!g_lastAvLogLine.empty())
    {
        std::string combined = g_lastAvLogLine + " (" + buffer + ")";
        g_lastAvLogLine.clear();
        return combined;
    }

    return buffer;
}
}  // namespace

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE

namespace
{
constexpr double kMaxReasonableFrameRateHz = 240.0;

bool isReasonableFrameRate(const AVRational& rate)
{
    if (rate.num <= 0 || rate.den <= 0)
        return false;

    const double fps = av_q2d(rate);
    return fps > 0.0 && fps <= kMaxReasonableFrameRateHz;
}

Float resolveFrameRateHz(AVFormatContext* fmtCtx, AVStream* stream)
{
    if (isReasonableFrameRate(stream->avg_frame_rate))
        return static_cast<Float>(av_q2d(stream->avg_frame_rate));

    if (isReasonableFrameRate(stream->r_frame_rate))
        return static_cast<Float>(av_q2d(stream->r_frame_rate));

    const AVRational guessed = av_guess_frame_rate(fmtCtx, stream, nullptr);
    if (isReasonableFrameRate(guessed))
        return static_cast<Float>(av_q2d(guessed));

    // avfoundation reports stream time_base in microseconds (1/1'000'000). When avg/r_frame_rate
    // are unset, av_guess_frame_rate() inverts that time_base and returns ~1e6 fps.
    if (std::string_view(cameraInputFormatName()) == "avfoundation")
        return 30.0f;

    return 0.0f;
}
}  // namespace

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

int CameraDriver::interruptCallback(void* opaque)
{
    return static_cast<CameraDriver*>(opaque)->interruptRequested.load(std::memory_order_relaxed) ? 1 : 0;
}

void CameraDriver::requestInterrupt()
{
    interruptRequested.store(true, std::memory_order_relaxed);
}

bool CameraDriver::open(const std::string& devicePath)
{
    close();
    interruptRequested.store(false, std::memory_order_relaxed);
    lastError.clear();
    g_lastAvLogLine.clear();

    // Raise the log level enough for the backend (e.g. dshow) to actually emit its own
    // diagnostic line on failure, and capture it instead of printing to stderr: the error
    // code alone from avformat_open_input() is frequently just a generic "unknown error".
    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(&avLogCaptureCallback);

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
        {
            lastError = std::string(cameraInputFormatName()) + " input format not found";
            return false;
        }
    }

    AVFormatContext* rawCtx = avformat_alloc_context();
    if (!rawCtx)
    {
        lastError = "avformat_alloc_context failed";
        return false;
    }

    rawCtx->flags |= AVFMT_FLAG_NONBLOCK;
    rawCtx->interrupt_callback.callback = &CameraDriver::interruptCallback;
    rawCtx->interrupt_callback.opaque = this;

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

    // dshow's demuxer parses its filename argument for "video=" / "audio=" tokens rather than
    // taking the device name directly, so the enumerated device name (e.g. "@device_pnp_...")
    // has to be wrapped in that syntax; v4l2/avfoundation take the device path as-is.
    std::string openTarget = devicePath;
    if (!isNetworkStream && std::string_view(cameraInputFormatName()) == "dshow")
        openTarget = "video=" + devicePath;

    const int openResult = avformat_open_input(&rawCtx, openTarget.c_str(), inputFmt, &options);
    av_dict_free(&options);

    if (openResult < 0)
    {
        lastError = "avformat_open_input failed: " + describeAvError(openResult);
        avformat_free_context(rawCtx);
        return false;
    }

    fmtCtx.reset(rawCtx);

    if (const int ret = avformat_find_stream_info(fmtCtx.get(), nullptr); ret < 0)
    {
        lastError = "avformat_find_stream_info failed: " + describeAvError(ret);
        close();
        return false;
    }

    streamIndex = av_find_best_stream(fmtCtx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (streamIndex < 0)
    {
        lastError = "no video stream found: " + describeAvError(streamIndex);
        close();
        return false;
    }

    AVStream* stream = fmtCtx->streams[streamIndex];
    AVCodecParameters* codecpar = stream->codecpar;

    if (codecpar->width == 0 || codecpar->height == 0)
    {
        lastError = "video stream reported zero width/height";
        close();
        return false;
    }

    const Float frameRateHz = resolveFrameRateHz(fmtCtx.get(), stream);
    if (frameRateHz <= 0.0f)
    {
        lastError = "could not determine a valid frame rate";
        close();
        return false;
    }

    streamInfo.width = static_cast<SizeT>(codecpar->width);
    streamInfo.height = static_cast<SizeT>(codecpar->height);
    streamInfo.frameRateHz = frameRateHz;
    streamInfo.nativeCodecId = codecpar->codec_id;
    streamInfo.nativePixelFormat = codecpar->format;
    streamInfo.nativeFormat = formatNativeCodecName(codecpar->codec_id, codecpar->format);

    streamTimeRatio = Ratio(stream->time_base.num, stream->time_base.den);

    readPacket.reset(av_packet_alloc());
    if (!readPacket)
    {
        lastError = "av_packet_alloc failed";
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
    streamTimeRatio = nullptr;
    streamInfo = {};
}

const AVCodecParameters* CameraDriver::getCodecParameters() const
{
    if (!fmtCtx || streamIndex < 0)
        return nullptr;
    return fmtCtx->streams[streamIndex]->codecpar;
}

RatioPtr CameraDriver::getStreamTimeRatio() const
{
    return streamTimeRatio;
}

size_t CameraDriver::readFrames(size_t maxCount, std::vector<std::shared_ptr<AVPacket>>& out)
{
    if (!fmtCtx || !readPacket || maxCount == 0)
        return 0;

    AVStream* stream = fmtCtx->streams[streamIndex];

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

        auto framePacket = std::shared_ptr<AVPacket>(av_packet_alloc(), shared_av_packet_deleter);
        if (!framePacket)
        {
            av_packet_unref(readPacket.get());
            continue;
        }

        av_packet_move_ref(framePacket.get(), readPacket.get());
        out.push_back(std::move(framePacket));
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
