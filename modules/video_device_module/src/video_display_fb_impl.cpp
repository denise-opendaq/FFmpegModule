#include <video_device_module/video_display_fb_impl.h>
#include <video_device_module/arial.ttf.h>

#include <video_device_module/ffmpeg_av_packet_utils.h>

#include <coreobjects/callable_info_factory.h>
#include <opendaq/event_packet_ids.h>
#include <opendaq/event_packet_utils.h>
#include <opendaq/time_reader.h>
#include <opendaq/work_factory.h>

#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE

namespace
{
std::string toLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool isCompressedImageFormat(const std::string& format)
{
    const std::string lower = toLower(format);
    return lower == "jpeg" || lower == "jpg" || lower == "png" || lower == "bmp" || lower == "targa" || lower == "hdr" ||
           lower == "gif";
}

Int metadataInt(const DataDescriptorPtr& descriptor, const char* key, Int defaultValue = 0)
{
    const auto metadata = descriptor.getMetadata();
    if (!metadata.assigned())
        return defaultValue;

    StringPtr value;
    if (!metadata.tryGet(key, value) || !value.assigned())
        return defaultValue;

    return std::stoi(value.toStdString());
}

template <typename T>
class LatestBox
{
public:
    void update(T value)
    {
        std::lock_guard<std::mutex> lock(mutex);
        lastValue = std::move(value);
        isNewValue = true;
    }

    bool tryGetLastValue(T& value)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!isNewValue)
            return false;

        value = lastValue;
        isNewValue = false;
        return true;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex);
        isNewValue = false;
    }

private:
    T lastValue{};
    bool isNewValue{false};
    mutable std::mutex mutex;
};
}  // namespace

struct VideoDisplayFbImpl::Impl
{
    enum class DisplayMode
    {
        None,
        CompressedImage,
        DecodedVideo,
    };

    struct RgbFrame
    {
        std::vector<std::uint8_t> pixels;
        unsigned width{0};
        unsigned height{0};
    };

    using AVFramePtr = std::unique_ptr<AVFrame, void (*)(AVFrame*)>;
    using AVCodecContextPtr = std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)>;
    using AVPacketPtr = std::unique_ptr<AVPacket, void (*)(AVPacket*)>;
    using SwsContextPtr = std::unique_ptr<SwsContext, void (*)(SwsContext*)>;

    static void av_frame_free_deleter(AVFrame* frame) { av_frame_free(&frame); }
    static void avcodec_free_context_deleter(AVCodecContext* ctx) { avcodec_free_context(&ctx); }
    static void av_packet_free_deleter(AVPacket* pkt) { av_packet_free(&pkt); }
    static void sws_freeContext_deleter(SwsContext* ctx) { sws_freeContext(ctx); }

    InputPortPtr inputPort;
    TimeReaderBase timeReader;

    std::unique_ptr<sf::RenderWindow> window;
    sf::Texture texture;
    sf::Sprite sprite;
    sf::Font font;
    sf::Text timestampText;

    LatestBox<DataPacketPtr> lastCompressedPacket;
    LatestBox<RgbFrame> lastRgbFrame;
    DataPacketPtr lastDomainPacket;

    DisplayMode displayMode{DisplayMode::None};

    Int nativeCodecId{0};
    Int nativePixelFormat{0};
    SizeT frameWidth{0};
    SizeT frameHeight{0};

    AVCodecContextPtr decodecCtx{nullptr, avcodec_free_context_deleter};
    SwsContextPtr swsCtx{nullptr, sws_freeContext_deleter};
    AVFramePtr rawFrame{nullptr, av_frame_free_deleter};
    AVFramePtr rgbaFrame{nullptr, av_frame_free_deleter};
    AVPacketPtr inputPacket{nullptr, av_packet_free_deleter};
    DataPacketPtr inputPacketHolder;

    int swsSrcWidth{0};
    int swsSrcHeight{0};
    AVPixelFormat swsSrcFormat{AV_PIX_FMT_NONE};

    bool pipelineReady{false};

    Impl()
        : sprite(texture)
        , font(ARIAL_TTF, sizeof(ARIAL_TTF))
        , timestampText(font)
    {
        timestampText.setCharacterSize(24);
        timestampText.setFillColor(sf::Color::White);
        timestampText.setOutlineColor(sf::Color::Black);
        timestampText.setOutlineThickness(2);
    }

    void resetPipeline()
    {
        pipelineReady = false;
        displayMode = DisplayMode::None;
        decodecCtx.reset();
        swsCtx.reset();
        rawFrame.reset();
        rgbaFrame.reset();
        inputPacket.reset();
        inputPacketHolder = nullptr;
        nativeCodecId = 0;
        nativePixelFormat = 0;
        frameWidth = 0;
        frameHeight = 0;
        swsSrcWidth = 0;
        swsSrcHeight = 0;
        swsSrcFormat = AV_PIX_FMT_NONE;
        lastCompressedPacket.clear();
        lastRgbFrame.clear();
        lastDomainPacket = nullptr;
    }

    void initDecoder()
    {
        decodecCtx.reset();
        swsCtx.reset();
        rgbaFrame.reset();
        swsSrcWidth = 0;
        swsSrcHeight = 0;
        swsSrcFormat = AV_PIX_FMT_NONE;

        if (nativeCodecId == AV_CODEC_ID_RAWVIDEO)
        {
            rawFrame.reset(av_frame_alloc());
            rawFrame->format = static_cast<int>(nativePixelFormat);
            rawFrame->width = static_cast<int>(frameWidth);
            rawFrame->height = static_cast<int>(frameHeight);
            return;
        }

        const AVCodec* decoder = avcodec_find_decoder(static_cast<AVCodecID>(nativeCodecId));
        if (!decoder)
            DAQ_THROW_EXCEPTION(NotFoundException, "Decoder not found for codec ID {}", nativeCodecId);

        decodecCtx.reset(avcodec_alloc_context3(decoder));
        decodecCtx->width = static_cast<int>(frameWidth);
        decodecCtx->height = static_cast<int>(frameHeight);

        if (avcodec_open2(decodecCtx.get(), decoder, nullptr) < 0)
            DAQ_THROW_EXCEPTION(InvalidOperationException, "Failed to open decoder");

        if (!inputPacket)
            inputPacket.reset(av_packet_alloc());
        if (!rawFrame)
            rawFrame.reset(av_frame_alloc());
    }

    bool refillInputPacket(const DataPacketPtr& packet, int inputSize)
    {
        if (!inputPacket)
            inputPacket.reset(av_packet_alloc());
        if (!inputPacket)
            return false;

        inputPacketHolder = packet;
        return bindDecoderInputFromDataPacket(inputPacket.get(), packet, inputSize);
    }

    bool decodeToRgb(const DataPacketPtr& packet, RgbFrame& outFrame)
    {
        if (!rawFrame)
            rawFrame.reset(av_frame_alloc());
        if (!rawFrame)
            return false;

        const auto* inputData = reinterpret_cast<const uint8_t*>(packet.getRawData());
        const int inputSize = static_cast<int>(packet.getRawDataSize());

        if (decodecCtx)
        {
            if (!refillInputPacket(packet, inputSize))
                return false;

            if (avcodec_send_packet(decodecCtx.get(), inputPacket.get()) < 0)
                return false;

            if (avcodec_receive_frame(decodecCtx.get(), rawFrame.get()) != 0)
                return false;
        }
        else if (nativeCodecId == AV_CODEC_ID_RAWVIDEO)
        {
            rawFrame->format = static_cast<int>(nativePixelFormat);
            rawFrame->width = static_cast<int>(frameWidth);
            rawFrame->height = static_cast<int>(frameHeight);

            if (av_image_fill_arrays(rawFrame->data,
                                     rawFrame->linesize,
                                     inputData,
                                     static_cast<AVPixelFormat>(nativePixelFormat),
                                     static_cast<int>(frameWidth),
                                     static_cast<int>(frameHeight),
                                     1) < 0)
                return false;
        }
        else
        {
            return false;
        }

        const int srcW = rawFrame->width;
        const int srcH = rawFrame->height;
        const auto srcFormat = static_cast<AVPixelFormat>(rawFrame->format);
        if (srcW <= 0 || srcH <= 0 || srcFormat == AV_PIX_FMT_NONE)
            return false;

        if (!swsCtx || swsSrcWidth != srcW || swsSrcHeight != srcH || swsSrcFormat != srcFormat)
        {
            swsCtx.reset(sws_getContext(srcW, srcH, srcFormat, srcW, srcH, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr));
            if (!swsCtx)
                return false;

            swsSrcWidth = srcW;
            swsSrcHeight = srcH;
            swsSrcFormat = srcFormat;
            rgbaFrame.reset();
        }

        if (!rgbaFrame)
        {
            rgbaFrame.reset(av_frame_alloc());
            rgbaFrame->format = AV_PIX_FMT_RGBA;
            rgbaFrame->width = srcW;
            rgbaFrame->height = srcH;
            if (av_frame_get_buffer(rgbaFrame.get(), 32) < 0)
                return false;
        }

        if (sws_scale(swsCtx.get(), rawFrame->data, rawFrame->linesize, 0, srcH, rgbaFrame->data, rgbaFrame->linesize) <= 0)
            return false;

        const auto rowBytes = static_cast<size_t>(rgbaFrame->linesize[0]);
        const auto imageBytes = rowBytes * static_cast<size_t>(srcH);
        outFrame.width = static_cast<unsigned>(srcW);
        outFrame.height = static_cast<unsigned>(srcH);
        outFrame.pixels.resize(imageBytes);

        for (int y = 0; y < srcH; ++y)
            std::memcpy(outFrame.pixels.data() + static_cast<size_t>(y) * rowBytes,
                        rgbaFrame->data[0] + y * rgbaFrame->linesize[0],
                        rowBytes);

        return true;
    }

    void applyInputDescriptor(const DataDescriptorPtr& dataDescriptor)
    {
        resetPipeline();

        if (dataDescriptor.getSampleType() != SampleType::Binary)
            DAQ_THROW_EXCEPTION(InvalidParameterException, "Input value signal must be binary");

        std::string formatName;
        if (const auto unit = dataDescriptor.getUnit(); unit.assigned())
            formatName = unit.getSymbol().toStdString();

        if (isCompressedImageFormat(formatName))
        {
            displayMode = DisplayMode::CompressedImage;
        }
        else
        {
            nativeCodecId = metadataInt(dataDescriptor, "NativeCodecId");
            nativePixelFormat = metadataInt(dataDescriptor, "NativePixelFormat");
            frameWidth = static_cast<SizeT>(metadataInt(dataDescriptor, "FrameWidth"));
            frameHeight = static_cast<SizeT>(metadataInt(dataDescriptor, "FrameHeight"));

            if (nativeCodecId == 0)
                DAQ_THROW_EXCEPTION(InvalidParameterException, "Unsupported video format '{}'", formatName);

            initDecoder();
            displayMode = DisplayMode::DecodedVideo;
        }

        pipelineReady = true;
        inputPort.setActive(true);
    }

    void updateTimestamp(const DataPacketPtr& domainPacket)
    {
        if (domainPacket.assigned() && domainPacket.getSampleCount())
        {
            std::chrono::system_clock::time_point timePoint;
            if (timeReader.transform(domainPacket.getData(), &timePoint, 1, domainPacket.getDataDescriptor()))
            {
                const std::time_t t = std::chrono::system_clock::to_time_t(timePoint);
                const std::tm tm = *std::localtime(&t);
                std::ostringstream oss;
                oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
                timestampText.setString(oss.str());
                return;
            }
        }

        timestampText.setString("No timestamp available");
    }

    bool closeWindow()
    {
        if (window)
            window->close();
        window.reset();
        return false;
    }
};

VideoDisplayFbImpl::VideoDisplayFbImpl(const ContextPtr& context,
                                       const ComponentPtr& parent,
                                       const StringPtr& localId)
    : FunctionBlockImpl(CreateType(), context, parent, localId)
    , impl(std::make_unique<Impl>())
{
    initComponentStatus();

    objPtr.addProperty(FunctionPropertyBuilder("OpenWindow", ProcedureInfo()).setReadOnly(true).build());
    objPtr.asPtr<IPropertyObjectProtected>(true).setProtectedPropertyValue("OpenWindow", Procedure([this] {
        startRender();
    }));

    impl->inputPort = createAndAddInputPort("inputPort", PacketReadyNotification::Scheduler);
    setComponentStatusWithMessage(ComponentStatus::Warning, "Signal is not connected");
}

VideoDisplayFbImpl::~VideoDisplayFbImpl() = default;

FunctionBlockTypePtr VideoDisplayFbImpl::CreateType()
{
    return FunctionBlockType(Id, Name, "Displays video frames from a connected signal");
}

void VideoDisplayFbImpl::onConnected(const InputPortPtr& port)
{
    auto inputValueSignal = port.getSignal();
    if (!inputValueSignal.getDomainSignal().assigned())
    {
        setComponentStatusWithMessage(ComponentStatus::Error, "Input signal must have a domain signal");
        port.setActive(false);
        return;
    }

    setComponentStatusWithMessage(ComponentStatus::Warning, "Waiting for signal descriptor");
}

void VideoDisplayFbImpl::onDisconnected(const InputPortPtr& port)
{
    impl->resetPipeline();
    impl->closeWindow();
    setComponentStatusWithMessage(ComponentStatus::Warning, "Signal is not connected");
}

void VideoDisplayFbImpl::handleEventPacket(const EventPacketPtr& packet)
{
    if (packet.getEventId() != event_packet_id::DATA_DESCRIPTOR_CHANGED)
        return;

    const auto [valueDescriptorChanged, domainDescriptorChanged, dataDescriptor, domainDescriptor] =
        parseDataDescriptorEventPacket(packet);
    (void)domainDescriptorChanged;
    (void)domainDescriptor;

    if (!valueDescriptorChanged || !dataDescriptor.assigned())
        return;

    try
    {
        impl->applyInputDescriptor(dataDescriptor);
        setComponentStatus(ComponentStatus::Ok);

        auto scheduler = context.getScheduler();
        auto thisWeakRef = this->template getWeakRefInternal<IFunctionBlock>();

        scheduler.scheduleWorkOnMainLoop(Work([this, thisWeakRef = std::move(thisWeakRef)] {
            const auto thisFb = thisWeakRef.getRef();
            if (!thisFb.assigned())
                return;

            if (!impl->window)
                startRender();
        }));
    }
    catch (const std::exception& e)
    {
        impl->inputPort.setActive(false);
        setComponentStatusWithMessage(ComponentStatus::Error, e.what());
    }
}

void VideoDisplayFbImpl::handleDataPacket(const DataPacketPtr& packet)
{
    if (!impl->pipelineReady || packet.getSampleCount() == 0)
        return;

    impl->lastDomainPacket = packet.getDomainPacket();

    if (impl->displayMode == Impl::DisplayMode::CompressedImage)
    {
        impl->lastCompressedPacket.update(packet);
        return;
    }

    Impl::RgbFrame frame;
    if (!impl->decodeToRgb(packet, frame))
        return;

    impl->lastRgbFrame.update(std::move(frame));
}

void VideoDisplayFbImpl::onPacketReceived(const InputPortPtr& port)
{
    const auto connection = port.getConnection();
    if (!connection.assigned())
        return;

    ListPtr<IPacket> packets;
    bool portIsActive;
    {
        auto lock = getAcquisitionLock2();
        packets = connection.dequeueAll();
        portIsActive = port.getActive();
    }

    for (const auto& packet : packets)
    {
        switch (packet.getType())
        {
            case PacketType::None:
                LOG_W("Packet type None");
                break;
            case PacketType::Event:
                handleEventPacket(packet.asPtr<IEventPacket>(true));
                portIsActive = port.getActive();
                break;
            case PacketType::Data:
                if (portIsActive)
                {
                    auto lock = getAcquisitionLock2();
                    if (port.getActive())
                        handleDataPacket(packet.asPtr<IDataPacket>(true));
                }
                break;
        }
    }
}

void VideoDisplayFbImpl::startRender()
{
    if (!context.getScheduler().isMainLoopSet())
        DAQ_THROW_EXCEPTION(InvalidStateException, "Main loop is not set in the scheduler. Video display requires a main loop for rendering.");

    if (impl->window && impl->window->isOpen())
        return;

    impl->window.reset();
    impl->inputPort.setActive(true);

    auto scheduler = context.getScheduler();
    auto thisWeakRef = this->template getWeakRefInternal<IFunctionBlock>();

    scheduler.scheduleWorkOnMainLoop(WorkRepetitive([this, thisWeakRef = thisWeakRef] {
        const auto thisFb = thisWeakRef.getRef();
        if (!thisFb.assigned())
            return false;

        if (impl->window)
        {
            if (!impl->window->isOpen())
                return false;

            while (auto event = impl->window->pollEvent())
            {
                if (event->is<sf::Event::Closed>())
                    return impl->closeWindow();
            }
        }

        bool frameUpdated = false;

        if (impl->displayMode == Impl::DisplayMode::CompressedImage)
        {
            DataPacketPtr packet;
            if (!impl->lastCompressedPacket.tryGetLastValue(packet))
                return true;

            const auto pictureSize = packet.getRawDataSize();
            const auto* pictureData = static_cast<const std::uint8_t*>(packet.getRawData());
            if (!impl->texture.loadFromMemory(pictureData, pictureSize))
            {
                impl->closeWindow();
                DAQ_THROW_EXCEPTION(InvalidOperationException, "Failed to load image from memory");
            }

            impl->updateTimestamp(packet.getDomainPacket());
            frameUpdated = true;
        }
        else if (impl->displayMode == Impl::DisplayMode::DecodedVideo)
        {
            Impl::RgbFrame frame;
            if (!impl->lastRgbFrame.tryGetLastValue(frame))
                return true;

            sf::Image image({frame.width, frame.height}, frame.pixels.data());
            if (!impl->texture.loadFromImage(image))
            {
                impl->closeWindow();
                DAQ_THROW_EXCEPTION(InvalidOperationException, "Failed to upload decoded frame to texture");
            }

            impl->updateTimestamp(impl->lastDomainPacket);
            frameUpdated = true;
        }

        if (!frameUpdated)
            return true;

        if (!impl->window)
            impl->window = std::make_unique<sf::RenderWindow>(sf::VideoMode(impl->texture.getSize()), "Video Display");

        impl->timestampText.setPosition({5.0f, 5.0f});
        impl->sprite.setTexture(impl->texture, true);

        impl->window->clear(sf::Color::Black);
        impl->window->draw(impl->sprite);
        impl->window->draw(impl->timestampText);
        impl->window->display();

        return true;
    }));
}

END_NAMESPACE_VIDEO_DEVICE_MODULE
