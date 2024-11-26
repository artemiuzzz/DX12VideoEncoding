#include "StreamWriter.h"
#include <stdexcept>

StreamWriter::~StreamWriter()
{
    av_packet_free(&pkt);

    if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
        avio_closep(&ofmt_ctx->pb);

    avformat_free_context(ofmt_ctx);
}

void StreamWriter::OpenOutputFile(const char* outFilename, int width, int height, AVRational fps)
{
    m_targetFramerate = fps;

    if (!pkt)
    {
        throw std::runtime_error("Could not allocate AVPacket");
    }

    int ret = avformat_alloc_output_context2(&ofmt_ctx, nullptr, nullptr, outFilename);
    if (!ofmt_ctx)
    {
        throw std::runtime_error("Could not create output context: " + av_error_to_string(ret));
    }

    ofmt = ofmt_ctx->oformat;

    const AVCodec* videoCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (videoCodec == nullptr)
    {
        throw std::runtime_error("Could not find codec H264");
    }

    AVStream* out_stream = avformat_new_stream(ofmt_ctx, videoCodec);
    if (!out_stream)
    {
        throw std::runtime_error("Failed allocating output stream");
    }
    out_stream->codecpar->codec_tag = 0;
    out_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    out_stream->codecpar->codec_id = AV_CODEC_ID_H264;
    out_stream->codecpar->format = AV_PIX_FMT_NV12;
    out_stream->codecpar->width = width;
    out_stream->codecpar->height = height;
    out_stream->index = 0;
    //out_stream->time_base = m_targetFramerate;

    av_dump_format(ofmt_ctx, 0, outFilename, 1);

    if (!(ofmt->flags & AVFMT_NOFILE))
    {
        ret = avio_open(&ofmt_ctx->pb, outFilename, AVIO_FLAG_WRITE);
        if (ret < 0)
        {
            throw std::runtime_error("Could not open output file \"" + std::string(outFilename) + "\": "
                + av_error_to_string(ret));
        }
    }

    ret = avformat_write_header(ofmt_ctx, nullptr);
    if (ret < 0)
    {
        throw std::runtime_error("Error occurred when opening output file: " + av_error_to_string(ret));
    }
}

void StreamWriter::WriteVideoPacket(const uint8_t* data, size_t size, bool isKeyFrame, int64_t pts, int64_t duration)
{
    pkt->data = (uint8_t*)data;
    pkt->size = size;
    pkt->stream_index = 0;
    pkt->pts = pts;
    pkt->dts = pts;
    pkt->duration = duration;

    const AVRational &streamTimeBase = ofmt_ctx->streams[0]->time_base;
    av_packet_rescale_ts(pkt, AVRational{ 1, 1000 }, streamTimeBase);

    pkt->flags = 0;
    if (isKeyFrame)
        pkt->flags = AV_PKT_FLAG_KEY;

    int ret = av_interleaved_write_frame(ofmt_ctx, pkt);
    if (ret < 0)
        throw std::runtime_error("Error occurred when writing packet: " + av_error_to_string(ret));
}

void StreamWriter::Finalize()
{
    int ret = av_write_trailer(ofmt_ctx);
    if (ret < 0)
        throw std::runtime_error("Error occurred when writing trailer: " + av_error_to_string(ret));
}
