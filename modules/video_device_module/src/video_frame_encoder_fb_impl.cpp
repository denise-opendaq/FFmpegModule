#include <video_device_module/video_frame_encoder_fb_impl.h>

#include <video_device_module/ffmpeg_av_packet_utils.h>

#include <opendaq/event_packet_ids.h>
#include <opendaq/event_packet_params.h>
#include <opendaq/event_packet_utils.h>
#include <opendaq/packet_factory.h>
#include <opendaq/binary_data_packet_factory.h>
#include <opendaq/deleter_factory.h>

#include <memory>
#include <string>

#define JPEG_FORMAT_INDEX 0
#define PNG_FORMAT_INDEX 1
#define BMP_FORMAT_INDEX 2
#define GIF_FORMAT_INDEX 3
#define TARGA_FORMAT_INDEX 4
#define HDR_FORMAT_INDEX 5

extern "C"
{
#include <libavutil/imgutils.h>
}

BEGIN_NAMESPACE_VIDEO_DEVICE_MODULE

void VideoFrameEncoderFbImpl::av_frame_free_deleter(AVFrame* frame)
{
    av_frame_free(&frame);
}

void VideoFrameEncoderFbImpl::avcodec_free_context_deleter(AVCodecContext* ctx)
{
    avcodec_free_context(&ctx);
}

void VideoFrameEncoderFbImpl::av_packet_free_deleter(AVPacket* pkt)
{
    av_packet_free(&pkt);
}

void VideoFrameEncoderFbImpl::sws_freeContext_deleter(SwsContext* ctx)
{
    sws_freeContext(ctx);
}

VideoFrameEncoderFbImpl::VideoFrameEncoderFbImpl(const ContextPtr& context,
                                                 const ComponentPtr& parent,
                                                 const StringPtr& localId)
    : FunctionBlockImpl(CreateType(), context, parent, localId)
{
    initComponentStatus();
    initProperties();
    initInputPorts();
    initSignals();
}

ListPtr<IString> VideoFrameEncoderFbImpl::GetSupportedFrameFormats()
{
    return List<IString>("JPEG", "PNG", "BMP", "GIF", "TARGA", "HDR");
}

FunctionBlockTypePtr VideoFrameEncoderFbImpl::CreateType()
{
    return FunctionBlockType(Id, Name, "Encodes native camera frames to a selected image format");
}

void VideoFrameEncoderFbImpl::initProperties()
{
    objPtr.addProperty(SelectionProperty("FrameFormat", GetSupportedFrameFormats(), 0));
    objPtr.getOnPropertyValueWrite("FrameFormat") += [this](PropertyObjectPtr& /*obj*/, PropertyValueEventArgsPtr& args) {
        updateFrameFormat(args.getValue());
    };

    const auto frameProperty = IntPropertyBuilder("FrameQuality", 0)
                                   .setMinValue(0)
                                   .setMaxValue(10)
                                   .setDescription("Compression level for the video frames. 0 is auto, 1 is low, 10 is high")
                                   .build();
    objPtr.addProperty(frameProperty);
    objPtr.getOnPropertyValueWrite("FrameQuality") += [this](PropertyObjectPtr& /*obj*/, PropertyValueEventArgsPtr& args) {
        updateFrameQuality(args.getValue());
    };

    updateFrameFormat(JPEG_FORMAT_INDEX);
}

void VideoFrameEncoderFbImpl::initInputPorts()
{
    inputPort = createAndAddInputPort("inputPort", PacketReadyNotification::Scheduler);
    setComponentStatusWithMessage(ComponentStatus::Warning, "Signal is not connected");
}

void VideoFrameEncoderFbImpl::initSignals()
{
    timeSignal = createAndAddSignal("time", nullptr, false);
    valueSignal = createAndAddSignal("value", nullptr);
    valueSignal.setDomainSignal(timeSignal);
}

void VideoFrameEncoderFbImpl::onConnected(const InputPortPtr& port)
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

void VideoFrameEncoderFbImpl::onDisconnected(const InputPortPtr& port)
{
    resetPipeline();
    setComponentStatusWithMessage(ComponentStatus::Warning, "Signal is not connected");
}

void VideoFrameEncoderFbImpl::resetPipeline()
{
    pipelineReady = false;
    decodecCtx.reset();
    encodecCtx.reset();
    swsCtx.reset();
    rawFrame.reset();
    swcFrame.reset();
    encodedPacket.reset();
    inputPacket.reset();
    inputPacketHolder = nullptr;
    nativeCodecId = 0;
    nativePixelFormat = 0;
    frameWidth = 0;
    frameHeight = 0;
    frameRate = 0.0f;
    nativeFormat.clear();
    swsSrcWidth = 0;
    swsSrcHeight = 0;
    swsSrcFormat = AV_PIX_FMT_NONE;
}

static Int metadataInt(const DataDescriptorPtr& descriptor, const char* key, Int defaultValue = 0)
{
    const auto metadata = descriptor.getMetadata();
    if (!metadata.assigned())
        return defaultValue;

    StringPtr value;
    if (!metadata.tryGet(key, value) || !value.assigned())
        return defaultValue;

    return std::stoi(value.toStdString());
}

static Float metadataFloat(const DataDescriptorPtr& descriptor, const char* key, Float defaultValue = 0.0f)
{
    const auto metadata = descriptor.getMetadata();
    if (!metadata.assigned())
        return defaultValue;

    StringPtr value;
    if (!metadata.tryGet(key, value) || !value.assigned())
        return defaultValue;

    return std::stof(value.toStdString());
}

void VideoFrameEncoderFbImpl::handleEventPacket(const EventPacketPtr& packet)
{
    if (packet.getEventId() != event_packet_id::DATA_DESCRIPTOR_CHANGED)
        return;

    const auto [valueDescriptorChanged, domainDescriptorChanged, dataDescriptor, domainDescriptor] =
        parseDataDescriptorEventPacket(packet);

    if (domainDescriptorChanged && domainDescriptor.assigned())
        timeSignal.setDescriptor(domainDescriptor);

    if (!valueDescriptorChanged || !dataDescriptor.assigned())
        return;

    if (dataDescriptor.getSampleType() != SampleType::Binary)
    {
        setComponentStatusWithMessage(ComponentStatus::Error, "Input value signal must be binary");
        inputPort.setActive(false);
        return;
    }

    applyInputDescriptor(dataDescriptor, domainDescriptor);
}

void VideoFrameEncoderFbImpl::applyInputDescriptor(const DataDescriptorPtr& dataDescriptor,
                                                   const DataDescriptorPtr& domainDescriptor)
{
    resetPipeline();

    nativeCodecId = metadataInt(dataDescriptor, "NativeCodecId");
    nativePixelFormat = metadataInt(dataDescriptor, "NativePixelFormat");
    frameWidth = static_cast<SizeT>(metadataInt(dataDescriptor, "FrameWidth"));
    frameHeight = static_cast<SizeT>(metadataInt(dataDescriptor, "FrameHeight"));
    frameRate = metadataFloat(dataDescriptor, "FrameRateHz");

    if (const auto unit = dataDescriptor.getUnit(); unit.assigned())
        nativeFormat = unit.getSymbol().toStdString();

    if (!shouldPassthrough())
    {
        initDecoder();
        initEncoder();
    }

    pipelineReady = true;

    if (shouldPassthrough())
        valueSignal.setDescriptor(dataDescriptor);
    else
    {
        const StringPtr frameFormat = encodeFrameFormatName;
        valueSignal.setDescriptor(DataDescriptorBuilder()
                                      .setSampleType(SampleType::Binary)
                                      .setName("Video")
                                      .setUnit(Unit(frameFormat, -1, frameFormat, "Video frame format"))
                                      .setRule(ExplicitDataRule())
                                      .build());
    }

    if (domainDescriptor.assigned())
        timeSignal.setDescriptor(domainDescriptor);

    inputPort.setActive(true);
    setComponentStatus(ComponentStatus::Ok);
}

void VideoFrameEncoderFbImpl::onPacketReceived(const InputPortPtr& port)
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

void VideoFrameEncoderFbImpl::handleDataPacket(const DataPacketPtr& packet)
{
    if (!pipelineReady)
        return;

    if (packet.getSampleCount() == 0)
        return;

    if (shouldPassthrough())
        processPassthrough(packet);
    else
        processTranscode(packet);
}

bool VideoFrameEncoderFbImpl::shouldPassthrough() const
{
    return encodeCodecId == AV_CODEC_ID_MJPEG && nativeCodecId == AV_CODEC_ID_MJPEG;
}

void VideoFrameEncoderFbImpl::processPassthrough(const DataPacketPtr& packet)
{
    const auto domainPacket = packet.getDomainPacket();
    timeSignal.sendPacket(domainPacket);

    valueSignal.sendPacket(BinaryDataPacketWithExternalMemory(
        domainPacket,
        valueSignal.getDescriptor(),
        static_cast<UInt>(packet.getRawDataSize()),
        const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(packet.getRawData())),
        Deleter([packet](void*) {})));
}

void VideoFrameEncoderFbImpl::processTranscode(const DataPacketPtr& packet)
{
    if (!encodecCtx)
        return;
    if (!decodecCtx && nativeCodecId != AV_CODEC_ID_RAWVIDEO)
        return;

    if (!encodedPacket)
        encodedPacket.reset(av_packet_alloc());
    if (!rawFrame)
        rawFrame.reset(av_frame_alloc());

    const auto* inputData = reinterpret_cast<const uint8_t*>(packet.getRawData());
    const int inputSize = static_cast<int>(packet.getRawDataSize());

    AVFrame* inputFrame = nullptr;

    if (decodecCtx)
    {
        if (!refillInputPacket(packet, inputSize))
            return;

        if (avcodec_send_packet(decodecCtx.get(), inputPacket.get()) < 0)
            return;

        if (avcodec_receive_frame(decodecCtx.get(), rawFrame.get()) != 0)
            return;

        if (!prepareConvertedFrame(rawFrame.get(), inputFrame))
            return;
    }
    else
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
            return;

        if (!prepareConvertedFrame(rawFrame.get(), inputFrame))
            return;
    }

    av_packet_unref(encodedPacket.get());

    if (avcodec_send_frame(encodecCtx.get(), inputFrame) < 0)
        return;

    if (avcodec_receive_packet(encodecCtx.get(), encodedPacket.get()) != 0)
        return;

    if (encodedPacket->size <= 0)
        return;

    const auto domainPacket = packet.getDomainPacket();
    timeSignal.sendPacket(domainPacket);

    auto outputPacket = AvPacketSharedPtr(encodedPacket.release(), av_packet_free_deleter);
    encodedPacket.reset(av_packet_alloc());
    if (!outputPacket || !encodedPacket)
        return;

    valueSignal.sendPacket(binaryDataPacketFromAvPacket(domainPacket, valueSignal.getDescriptor(), std::move(outputPacket)));
}

void VideoFrameEncoderFbImpl::updateFrameFormat(Int format)
{
    switch (format)
    {
        case JPEG_FORMAT_INDEX:
            encodeCodecId = AV_CODEC_ID_MJPEG;
            swsFormat = AV_PIX_FMT_YUVJ420P;
            encodeFrameFormatName = "JPEG";
            break;
        case PNG_FORMAT_INDEX:
            encodeCodecId = AV_CODEC_ID_PNG;
            swsFormat = AV_PIX_FMT_RGB24;
            encodeFrameFormatName = "PNG";
            break;
        case BMP_FORMAT_INDEX:
            encodeCodecId = AV_CODEC_ID_BMP;
            swsFormat = AV_PIX_FMT_BGR24;
            encodeFrameFormatName = "BMP";
            break;
        case GIF_FORMAT_INDEX:
            encodeCodecId = AV_CODEC_ID_GIF;
            swsFormat = AV_PIX_FMT_RGB8;
            encodeFrameFormatName = "GIF";
            break;
        case TARGA_FORMAT_INDEX:
            encodeCodecId = AV_CODEC_ID_TARGA;
            swsFormat = AV_PIX_FMT_BGR24;
            encodeFrameFormatName = "TARGA";
            break;
        case HDR_FORMAT_INDEX:
            encodeCodecId = AV_CODEC_ID_RADIANCE_HDR;
            swsFormat = AV_PIX_FMT_GBRPF32LE;
            encodeFrameFormatName = "HDR";
            break;
        default:
            DAQ_THROW_EXCEPTION(InvalidParameterException, "Unsupported frame format code {}", format);
    }

    if (pipelineReady)
    {
        initDecoder();
        initEncoder();

        if (!shouldPassthrough())
        {
            const StringPtr frameFormat = encodeFrameFormatName;
            valueSignal.setDescriptor(DataDescriptorBuilder()
                                          .setSampleType(SampleType::Binary)
                                          .setName("Video")
                                          .setUnit(Unit(frameFormat, -1, frameFormat, "Video frame format"))
                                          .setRule(ExplicitDataRule())
                                          .build());
        }
    }
}

void VideoFrameEncoderFbImpl::updateFrameQuality(Int quality)
{
    encodeQuality = quality;
    if (pipelineReady && !shouldPassthrough())
        initEncoder();
}

void VideoFrameEncoderFbImpl::initDecoder()
{
    decodecCtx.reset();
    swsCtx.reset();
    swcFrame.reset();
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
}

bool VideoFrameEncoderFbImpl::refillInputPacket(const DataPacketPtr& packet, int inputSize)
{
    if (!inputPacket && !decodecCtx)
        return false;

    if (!inputPacket)
        inputPacket.reset(av_packet_alloc());
    if (!inputPacket)
        return false;

    inputPacketHolder = packet;
    return bindDecoderInputFromDataPacket(inputPacket.get(), packet, inputSize);
}

bool VideoFrameEncoderFbImpl::prepareConvertedFrame(AVFrame* srcFrame, AVFrame*& outFrame)
{
    if (!srcFrame || srcFrame->width <= 0 || srcFrame->height <= 0)
        return false;

    const auto srcFormat = static_cast<AVPixelFormat>(srcFrame->format);
    if (srcFormat == AV_PIX_FMT_NONE)
        return false;

    if (srcFormat == swsFormat)
    {
        outFrame = srcFrame;
        return true;
    }

    const int srcW = srcFrame->width;
    const int srcH = srcFrame->height;

    if (!swsCtx || swsSrcWidth != srcW || swsSrcHeight != srcH || swsSrcFormat != srcFormat)
    {
        swsCtx.reset(sws_getContext(srcW, srcH, srcFormat, srcW, srcH, swsFormat, SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!swsCtx)
            return false;

        swsSrcWidth = srcW;
        swsSrcHeight = srcH;
        swsSrcFormat = srcFormat;
        swcFrame.reset();
    }

    if (!swcFrame)
    {
        swcFrame.reset(av_frame_alloc());
        swcFrame->format = swsFormat;
        swcFrame->width = srcW;
        swcFrame->height = srcH;
        if (av_frame_get_buffer(swcFrame.get(), 32) < 0)
            return false;
    }

    if (sws_scale(swsCtx.get(), srcFrame->data, srcFrame->linesize, 0, srcH, swcFrame->data, swcFrame->linesize) <= 0)
        return false;

    outFrame = swcFrame.get();
    return true;
}

void VideoFrameEncoderFbImpl::applyFrameQuality(AVDictionary** options)
{
    if (encodeQuality == 0)
        return;

    if (encodeCodecId == AV_CODEC_ID_MJPEG)
    {
        encodecCtx->flags |= AV_CODEC_FLAG_QSCALE;
        encodecCtx->global_quality = static_cast<int>(FF_QP2LAMBDA * (11 - encodeQuality));
    }
    else if (encodeCodecId == AV_CODEC_ID_PNG)
    {
        const int compressionLevel = static_cast<int>(10 - encodeQuality);
        av_dict_set(options, "compression_level", std::to_string(compressionLevel).c_str(), 0);
    }
    else if (encodeCodecId == AV_CODEC_ID_RADIANCE_HDR)
    {
        encodecCtx->thread_count = 1;
    }
}

void VideoFrameEncoderFbImpl::initEncoder()
{
    const AVCodec* codec = avcodec_find_encoder(encodeCodecId);
    if (!codec)
        DAQ_THROW_EXCEPTION(NotFoundException, "Encoder not found for codec ID {}", static_cast<int>(encodeCodecId));

    encodecCtx.reset(avcodec_alloc_context3(codec));
    encodecCtx->pix_fmt = swsFormat;
    encodecCtx->width = static_cast<int>(frameWidth);
    encodecCtx->height = static_cast<int>(frameHeight);
    encodecCtx->time_base = {1, static_cast<int>(frameRate > 0.0f ? frameRate : 30.0f)};

    AVDictionary* options = nullptr;
    applyFrameQuality(&options);

    const int ret = avcodec_open2(encodecCtx.get(), codec, &options);
    av_dict_free(&options);

    if (ret < 0)
        DAQ_THROW_EXCEPTION(InvalidOperationException, "Failed to open encoder");

    if (!encodedPacket)
        encodedPacket.reset(av_packet_alloc());
}

END_NAMESPACE_VIDEO_DEVICE_MODULE
