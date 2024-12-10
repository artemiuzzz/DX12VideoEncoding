#pragma once
#include "EncoderAPI.h"
#include "FFmpegHelpers.h"
#include <stdexcept>

extern "C"
{
#include <libavcodec/avcodec.h>
}

// This is a wrapper around AVFrame that increases ref counter
// to keep the frame alive while before encoding starts.
class FfmpegFrameData : public DX12VideoEncoding::IRawFrameData
{
private:
    FfmpegFrameData()
        : m_frame(av_frame_alloc())
    {
    }

    void AddRef(const AVFrame& frame)
    {
        int ret = av_frame_ref(m_frame, &frame);
        if (ret < 0)
        {
            throw std::runtime_error("av_frame_ref() failed: " + av_error_to_string(ret));
        }
    }

public:
    static std::shared_ptr<FfmpegFrameData> Create(const AVFrame& frame)
    {
        std::shared_ptr<FfmpegFrameData> frameData(new FfmpegFrameData());
        frameData->AddRef(frame);
        return frameData;
    }

    ~FfmpegFrameData() override
    {
        av_frame_unref(m_frame);
        av_frame_free(&m_frame);
    }

    void* GetY() const override
    {
        return m_frame->data[0];
    }
    void* GetUV() const override
    {
        return m_frame->data[1];
    }
    size_t GetLinesizeY() const override
    {
        return static_cast<size_t>(m_frame->linesize[0]);
    }
    size_t GetLinesizeUV() const override
    {
        return static_cast<size_t>(m_frame->linesize[1]);
    }
    uint32_t GetWidth() const override
    {
        return m_frame->width;
    }
    uint32_t GetHeight() const override
    {
        return m_frame->height;
    }

private:
    AVFrame* m_frame;
};

