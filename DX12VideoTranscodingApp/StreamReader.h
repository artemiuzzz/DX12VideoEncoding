#pragma once

#include <functional>
#include "FFmpegHelpers.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}


class StreamReader
{
public:
    using TFrameCallback = std::function<void(const AVFrame& frame)>;

    ~StreamReader();

    struct VideoStreamInfo
    {
        AVRational frameRate{};
        int width{};
        int height{};
    };
    VideoStreamInfo OpenInputFile(const char* filename);
    void ReadAllFrames(const TFrameCallback& frameCallback);

private:
    void DisplayFrame(const AVFrame* frame, AVRational time_base);
    void InitFilters(const char* filters_descr);

private:
    TFrameCallback m_frameCallback;

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* filt_frame = av_frame_alloc();
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* dec_ctx = nullptr;
    AVFilterContext* buffersink_ctx = nullptr;
    AVFilterContext* buffersrc_ctx = nullptr;
    AVFilterGraph* filter_graph = avfilter_graph_alloc();
    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();

    int video_stream_index = -1;
    int64_t last_pts = AV_NOPTS_VALUE;
};

