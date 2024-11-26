#pragma once

#include "FFmpegHelpers.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

class StreamWriter
{
public:
    ~StreamWriter();

    void OpenOutputFile(const char* outFilename, int width, int height, AVRational fps);
    void WriteVideoPacket(const uint8_t* data, size_t size, bool isKeyFrame, int64_t pts, int64_t duration);
    void Finalize();

private:
    const AVOutputFormat* ofmt = nullptr;
    AVFormatContext* ofmt_ctx = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVRational m_targetFramerate = {};
};

