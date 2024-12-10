#include "StreamReader.h"
#include "FFmpegHelpers.h"
#include <stdexcept>

StreamReader::~StreamReader()
{
    avfilter_graph_free(&filter_graph);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    av_frame_free(&frame);
    av_frame_free(&filt_frame);
    av_packet_free(&packet);

    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
}

void StreamReader::ReadAllFrames(const TFrameCallback& frameCallback)
{
    if (!frame || !filt_frame || !packet)
    {
        throw std::runtime_error("Could not allocate frame or packet");
    }

    if (!outputs || !inputs || !filter_graph)
    {
        throw std::runtime_error("Could not allocate filter graph");
    }

    m_frameCallback = frameCallback;

    int ret;

    const char* filter_descr = "format=pix_fmts=nv12";
    InitFilters(filter_descr);

    /* read all packets */
    while (1)
    {
        if ((ret = av_read_frame(fmt_ctx, packet)) < 0)
            break;

        if (packet->stream_index == video_stream_index)
        {
            ret = avcodec_send_packet(dec_ctx, packet);
            if (ret < 0)
            {
                throw std::runtime_error("Error while sending a packet to the decoder: " + av_error_to_string(ret));
            }

            while (ret >= 0)
            {
                ret = avcodec_receive_frame(dec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                {
                    break;
                }
                else if (ret < 0)
                {
                    throw std::runtime_error("Error while receiving a frame from the decoder: " + av_error_to_string(ret));
                }

                frame->pts = frame->best_effort_timestamp;

                /* push the decoded frame into the filtergraph */
                if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
                {
                    throw std::runtime_error("Error while feeding the filtergraph");
                }

                /* pull filtered frames from the filtergraph */
                while (1)
                {
                    ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        break;
                    if (ret < 0)
                        throw std::runtime_error("av_buffersink_get_frame(): " + av_error_to_string(ret));

                    DisplayFrame(filt_frame, buffersink_ctx->inputs[0]->time_base);
                    av_frame_unref(filt_frame);
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);
    }
    if (ret == AVERROR_EOF)
    {
        /* signal EOF to the filtergraph */
        ret = av_buffersrc_add_frame_flags(buffersrc_ctx, nullptr, 0);
        if (ret < 0)
        {
            throw std::runtime_error("Error while closing the filtergraph: " + av_error_to_string(ret));
        }

        /* pull remaining frames from the filtergraph */
        while (1)
        {
            ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                throw std::runtime_error("av_buffersink_get_frame(): " + av_error_to_string(ret));

            DisplayFrame(filt_frame, buffersink_ctx->inputs[0]->time_base);
            av_frame_unref(filt_frame);
        }
    }
}

void StreamReader::DisplayFrame(const AVFrame* frame, AVRational /*time_base*/)
{
    m_frameCallback(*frame);
}

StreamReader::VideoStreamInfo StreamReader::OpenInputFile(const char* filename)
{
    if (dec_ctx || fmt_ctx)
        throw std::runtime_error("Already opened");

    int ret;

    if ((ret = avformat_open_input(&fmt_ctx, filename, nullptr, nullptr)) < 0)
        throw std::runtime_error("Cannot open input file: " + av_error_to_string(ret));

    if ((ret = avformat_find_stream_info(fmt_ctx, nullptr)) < 0)
        throw std::runtime_error("Cannot find stream information: " + av_error_to_string(ret));

    /* select the video stream */
    const AVCodec* dec;
    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (ret < 0)
        throw std::runtime_error("Cannot find a video stream in the input file: " + av_error_to_string(ret));

    video_stream_index = ret;

    /* create decoding context */
    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx)
        throw std::runtime_error("avcodec_alloc_context3() failed");
    avcodec_parameters_to_context(dec_ctx, fmt_ctx->streams[video_stream_index]->codecpar);

    /* init the video decoder */
    if ((ret = avcodec_open2(dec_ctx, dec, nullptr)) < 0)
        throw std::runtime_error("Cannot open video decoder: " + av_error_to_string(ret));

    AVRational frameRate = av_guess_frame_rate(fmt_ctx, fmt_ctx->streams[video_stream_index], nullptr);
    if (frameRate.den == 0)
        throw std::runtime_error("Cannot guess framerate");

    VideoStreamInfo result{
        .frameRate = frameRate,
        .width = dec_ctx->width,
        .height = dec_ctx->height,
    };
    return result;
}

void StreamReader::InitFilters(const char* filters_descr)
{
    if (buffersrc_ctx || buffersink_ctx)
        throw std::runtime_error("Already started");

    char args[512];
    const AVFilter* buffersrc = avfilter_get_by_name("buffer");
    const AVFilter* buffersink = avfilter_get_by_name("buffersink");

    AVRational time_base = fmt_ctx->streams[video_stream_index]->time_base;
    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_GRAY8, AV_PIX_FMT_NONE };

    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    snprintf(args, sizeof(args),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
        dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
        time_base.num, time_base.den,
        dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);

    int ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
        args, nullptr, filter_graph);
    if (ret < 0)
        throw std::runtime_error("Cannot create buffer source: " + av_error_to_string(ret));

    /* buffer video sink: to terminate the filter chain. */
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
        nullptr, nullptr, filter_graph);
    if (ret < 0)
        throw std::runtime_error("Cannot create buffer sink: " + av_error_to_string(ret));

    /*
    * Set the endpoints for the filter graph. The filter_graph will
    * be linked to the graph described by filters_descr.
    */

    /*
    * The buffer source output must be connected to the input pad of
    * the first filter described by filters_descr; since the first
    * filter input label is not specified, it is set to "in" by
    * default.
    */
    outputs->name = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    /*
    * The buffer sink input must be connected to the output pad of
    * the last filter described by filters_descr; since the last
    * filter output label is not specified, it is set to "out" by
    * default.
    */
    inputs->name = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filters_descr,
        &inputs, &outputs, nullptr)) < 0)
    {
        throw std::runtime_error("avfilter_graph_parse_ptr(): " + av_error_to_string(ret));
    }

    if ((ret = avfilter_graph_config(filter_graph, nullptr)) < 0)
    {
        throw std::runtime_error("avfilter_graph_config(): " + av_error_to_string(ret));
    }
}
