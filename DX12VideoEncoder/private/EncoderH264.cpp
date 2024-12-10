#include "pch.h"
#include "EncoderH264.h"
#include "EncoderH264DX12.h"

namespace DX12VideoEncoding
{

namespace {

std::string GetFrameTypeName(D3D12_VIDEO_ENCODER_FRAME_TYPE_H264 frameType)
{
    switch (frameType)
    {
    case D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME:
        return "IDR";
    case D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_I_FRAME:
        return "I";
    case D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME:
        return "P";
    case D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_B_FRAME:
        return "B";
    default:
        return "Unknown";
    }
}

template <typename T>
std::string VectorNumbersToString(const std::vector<T>& numbers)
{
    std::string result;
    for (size_t i = 0; i < numbers.size(); ++i)
    {
        result += std::to_string(numbers[i]);
        if (i != numbers.size() - 1)
            result += ", ";
    }
    return result;
}

}

std::unique_ptr<IEncoder> CreateH264Encoder(
    const ComPtr<ID3D12Device>& device, const EncoderConfiguration& configuration)
{
    return std::make_unique<EncoderH264>(
        std::make_unique<EncoderH264DX12>(device, configuration, DXGI_FORMAT_NV12),
        std::make_unique<InputFrameResources>(device, DXGI_FORMAT_NV12, configuration.width, configuration.height),
        configuration.keyFrameInterval, configuration.bFramesCount, configuration.maxReferenceFrameCount);
}

EncoderH264::EncoderH264(
    std::unique_ptr<EncoderH264DX12> encoder,
    std::unique_ptr<InputFrameResources> inputFrameResources,
    uint32_t keyFrameInterval,
    uint32_t bFramesCount,
    uint32_t maxReferenceFrameCount)
    : m_inputFrameResources(std::move(inputFrameResources))
    , m_encoder(std::move(encoder))
    , m_keyFrameInterval(keyFrameInterval)
    , m_bFramesCount(bFramesCount)
    , m_maxReferenceFrameCount(maxReferenceFrameCount)
{
    m_termintateEvent.Attach(CreateEvent(NULL, TRUE, FALSE, TEXT("terminateEvent")));
}

void EncoderH264::PushFrame(const RawFrameData& frameData)
{
    // Check that there is no running encoding.
    assert(!m_currentFrame.has_value());

    auto frameType = GetFrameType(m_currentFrameOrderNumber);
    if (frameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME)
    {
        m_encodedReferenceFrameOrderNumberList.clear();
        m_lastIdrNumber = m_currentFrameOrderNumber;
        if (m_currentFrameOrderNumber != 0)
            ++m_idrPicId;
    }

    auto rawFrame = RawFrame{
        .frameData = frameData,
        .frameType = frameType,
        .frameOrderNumber = static_cast<uint32_t>(m_currentFrameOrderNumber),
        .idrPicId = m_idrPicId,
    };

    while (true)
    {
        if (rawFrame.frameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME)
        {
            assert(m_reorderingFrameBuffer.empty());
            rawFrame.useAsReference = true;
            m_currentFrame = rawFrame;
        }
        else if (rawFrame.frameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_B_FRAME)
        {
            auto futureRefFrameNumber = GetNextReferenceFrameNumber(rawFrame.frameOrderNumber);
            auto nextIdrFrameNumber = GetNextIDRFrameNumber(rawFrame.frameOrderNumber);
            if (futureRefFrameNumber >= nextIdrFrameNumber)
            {
                // No P frame in the end of GOP, treat this frame as P frame.
                LogMessage(LogLevel::E_DEBUG, "Queue P frame " + std::to_string(rawFrame.frameOrderNumber)
                    + " instead of B frame because of GOP end\n");
                rawFrame.frameType = D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME;
                continue;
            }
            else
            {
                LogMessage(LogLevel::E_DEBUG, "Queue B frame " + std::to_string(rawFrame.frameOrderNumber)
                    + "\n");

                // Don't use referenced B frames.
                rawFrame.useAsReference = false;

                rawFrame.futureReferenceFrameOrderNumber = futureRefFrameNumber;
                m_reorderingFrameBuffer.push_back(std::move(rawFrame));

                m_currentFrame.reset();
            }
        }
        else if (rawFrame.frameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME)
        {
            rawFrame.useAsReference = true;
            m_currentFrame = rawFrame;
        }

        break;
    };

    ++m_currentFrameOrderNumber;
}

bool EncoderH264::StartEncodingPushedFrame()
{
    // Returns false when need more frames to encode.
    if (!m_currentFrame.has_value())
    {
        m_currentFrame = GetNextFrameToEncode();
        if (!m_currentFrame.has_value())
            return false;
    }

    std::vector<uint32_t> l0List;
    std::vector<uint32_t> l1List;

    if ((*m_currentFrame).frameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME)
    {
        // Using only 1 past reference frame, and it should be previous encoded frame.
        assert(!m_encodedReferenceFrameOrderNumberList.empty());
        auto pastFrameOrderNumber = *m_encodedReferenceFrameOrderNumberList.rbegin() - m_lastIdrNumber;
        l0List.push_back(static_cast<uint32_t>(pastFrameOrderNumber));
    }
    else if ((*m_currentFrame).frameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_B_FRAME)
    {
        // Using only 1 past and 1 future reference frame.
        assert(m_encodedReferenceFrameOrderNumberList.size() >= 2);
        size_t lastIdx = m_encodedReferenceFrameOrderNumberList.size() - 1;

        auto pastFrameOrderNumber = m_encodedReferenceFrameOrderNumberList[lastIdx - 1] - m_lastIdrNumber;
        l0List.push_back(static_cast<uint32_t>(pastFrameOrderNumber));

        auto futureFrameOrderNumber = m_encodedReferenceFrameOrderNumberList[lastIdx] - m_lastIdrNumber;
        l1List.push_back(static_cast<uint32_t>(futureFrameOrderNumber));
    }


    EncoderH264DX12::InputFrame inputFrame = {
        .frameType = (*m_currentFrame).frameType,
        .pictureOrderCountNumber = static_cast<UINT>((*m_currentFrame).frameOrderNumber - m_lastIdrNumber),
        .decodingOrderNumber = static_cast<UINT>(m_currentDecodingOrderNumber - m_lastIdrNumber),
        .idrPicId = (*m_currentFrame).idrPicId,
        .l0List = std::move(l0List),
        .l1List = std::move(l1List),
        .useAsReference = (*m_currentFrame).useAsReference,
    };

    LogMessage(LogLevel::E_INFO, "Encoding frame: type " + GetFrameTypeName(inputFrame.frameType)
        + ", pic order " + std::to_string(inputFrame.pictureOrderCountNumber)
        + ", dec order " + std::to_string(inputFrame.decodingOrderNumber));

    if (!inputFrame.l0List.empty())
    {
        LogMessage(LogLevel::E_DEBUG, "L0: " + VectorNumbersToString(inputFrame.l0List));
    }
    if (!inputFrame.l1List.empty())
    {
        LogMessage(LogLevel::E_DEBUG, "L1: " + VectorNumbersToString(inputFrame.l1List));
    }
    if (inputFrame.frameType != D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME)
    {
        LogMessage(LogLevel::E_DEBUG, "POC of referenece frames in DPB: " + VectorNumbersToString(m_encodedReferenceFrameOrderNumberList));
    }
    LogMessage(LogLevel::E_INFO, "\n");

    m_inputFrameResources->SetFrameData((*m_currentFrame).frameData);
    m_inputFrameResources->UploadTexture();
    m_encoder->SendFrame(inputFrame, *m_inputFrameResources.get());

    return true;
}

bool EncoderH264::WaitForEncodedFrame(EncodedFrame& encodedFrame)
{
    assert(m_currentFrame.has_value());

    if (!m_encoder->WaitForEncodedData(m_termintateEvent, encodedFrame.encodedData))
        return false;

    encodedFrame.pictureOrderCountNumber = m_currentFrame->frameOrderNumber;
    encodedFrame.decodingOrderNumber = m_currentDecodingOrderNumber;
    encodedFrame.isKeyFrame = m_currentFrame->frameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME
        || m_currentFrame->frameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_I_FRAME;

    if (m_currentFrame->useAsReference)
    {
        m_encodedReferenceFrameOrderNumberList.push_back(m_currentFrame->frameOrderNumber);

        // Keep only last reference frames. Although count of reference frames should be received from the DPB.
        if (m_encodedReferenceFrameOrderNumberList.size() > m_maxReferenceFrameCount)
            m_encodedReferenceFrameOrderNumberList.erase(m_encodedReferenceFrameOrderNumberList.begin());
    }

    ++m_currentDecodingOrderNumber;

    m_inputFrameResources->ResetCommands();
    m_currentFrame.reset();

    return true;
}

void EncoderH264::Flush()
{
    if (!m_reorderingFrameBuffer.empty())
    {
        // Change buffered B frames to P frames and push the current IDR frame.
        for (size_t i = 0; i < m_reorderingFrameBuffer.size(); ++i)
        {
            auto& frame = m_reorderingFrameBuffer[i];
            if (frame.frameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_B_FRAME)
            {
                frame.frameType = D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME;
                frame.useAsReference = true;
            }
            else
                assert(false && "Only B frames should be in the reordering buffer.");
        }
    }
}

void EncoderH264::Terminate()
{
    SetEvent(m_termintateEvent.Get());
}

std::optional<EncoderH264::RawFrame> EncoderH264::GetNextFrameToEncode()
{
    if (m_reorderingFrameBuffer.empty())
        return {};

    auto bufferedFrame = m_reorderingFrameBuffer.front();
    if (bufferedFrame.frameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_B_FRAME)
    {
        // Check if all future reference frames are already encoded.

        if (m_encodedReferenceFrameOrderNumberList.empty())
            return {};

        auto lastEncodedReferenceFrameOrderNumber = m_encodedReferenceFrameOrderNumberList.back();
        if (lastEncodedReferenceFrameOrderNumber < bufferedFrame.futureReferenceFrameOrderNumber)
            return {};
    }

    RawFrame frame = bufferedFrame;
    m_reorderingFrameBuffer.erase(m_reorderingFrameBuffer.begin());
    return frame;
}

D3D12_VIDEO_ENCODER_FRAME_TYPE_H264 EncoderH264::GetFrameType(uint64_t pictureOrderCountNumber)
{
    bool firstFrame = pictureOrderCountNumber == 0;
    if (firstFrame ||
        (m_keyFrameInterval > 0 && (pictureOrderCountNumber % m_keyFrameInterval == 0)))
    {
        return D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME;
    }

    const auto gopStart = m_keyFrameInterval == 0 ? 0
        : (pictureOrderCountNumber / m_keyFrameInterval) * m_keyFrameInterval;

    if (((pictureOrderCountNumber - gopStart) % (m_bFramesCount + 1)) == 0)
        return D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME;

    return D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_B_FRAME;
}

uint64_t EncoderH264::GetNextReferenceFrameNumber(uint64_t pictureOrderCountNumber)
{
    const auto gopStart = m_keyFrameInterval == 0 ? 0
        : (pictureOrderCountNumber / m_keyFrameInterval) * m_keyFrameInterval;

    const auto pFrameInterval = m_bFramesCount + 1;
    return ((pictureOrderCountNumber - gopStart) / pFrameInterval) * pFrameInterval + pFrameInterval + gopStart;
}

uint64_t EncoderH264::GetNextIDRFrameNumber(uint64_t pictureOrderCountNumber)
{
    if (m_keyFrameInterval == 0)
        return (std::numeric_limits<uint64_t>::max)();
    return (pictureOrderCountNumber / m_keyFrameInterval + 1) * m_keyFrameInterval;
}

}
