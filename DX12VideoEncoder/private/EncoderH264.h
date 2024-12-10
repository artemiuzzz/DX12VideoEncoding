#pragma once

#include "EncoderAPI.h"
#include "InputFrameResources.h"
#include "Utils.h"
#include "ReferenceFramesManager.h"

namespace DX12VideoEncoding
{

class EncoderH264DX12;

class EncoderH264 : public IEncoder
{
public:
    EncoderH264(
        std::unique_ptr<EncoderH264DX12> encoder,
        std::unique_ptr<InputFrameResources> inputFrameResources,
        uint32_t keyFrameInterval,
        uint32_t bFramesCount,
        uint32_t maxReferenceFrameCount);

    void PushFrame(const RawFrameData& frameData) override;
    bool StartEncodingPushedFrame() override;
    bool WaitForEncodedFrame(EncodedFrame& encodedFrame) override;
    void Flush() override;
    void Terminate() override;

private:

    struct RawFrame
    {
        RawFrameData frameData;
        D3D12_VIDEO_ENCODER_FRAME_TYPE_H264 frameType{ D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME };
        uint64_t frameOrderNumber{};
        uint32_t idrPicId{};
        uint64_t futureReferenceFrameOrderNumber{}; // Only valid for B-frames
        bool useAsReference{ false };
    };

    std::optional<RawFrame> GetNextFrameToEncode();
    D3D12_VIDEO_ENCODER_FRAME_TYPE_H264 GetFrameType(uint64_t pictureOrderCountNumber);
    uint64_t GetNextReferenceFrameNumber(uint64_t pictureOrderCountNumber);
    uint64_t GetNextIDRFrameNumber(uint64_t pictureOrderCountNumber);

private:

    std::unique_ptr<EncoderH264DX12> m_encoder;
    Microsoft::WRL::Wrappers::Event m_termintateEvent;

    const uint32_t m_keyFrameInterval; // 0 - inifinite GOP
    const uint32_t m_bFramesCount;
    const uint32_t m_maxReferenceFrameCount;

    uint64_t m_currentFrameOrderNumber = 0;
    uint64_t m_currentDecodingOrderNumber = 0;
    uint64_t m_lastIdrNumber = 0;
    uint32_t m_idrPicId = 0;
    std::vector<uint64_t> m_encodedReferenceFrameOrderNumberList;
    std::unique_ptr<InputFrameResources> m_inputFrameResources;

    std::optional<RawFrame> m_currentFrame;
    std::vector<RawFrame> m_reorderingFrameBuffer;
};

} // namespace DX12VideoEncoding
