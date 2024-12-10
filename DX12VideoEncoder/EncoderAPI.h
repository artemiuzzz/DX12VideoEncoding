#pragma once
#include <vector>
#include <memory>
#include <d3d12.h>
#include <wrl.h>

namespace DX12VideoEncoding
{

class IRawFrameData
{
public:
    virtual ~IRawFrameData() = default;
    virtual void* GetY() const = 0;
    virtual void* GetUV() const = 0;
    virtual size_t GetLinesizeY() const = 0;
    virtual size_t GetLinesizeUV() const = 0;
    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;
};

using RawFrameData = std::shared_ptr<IRawFrameData>;

struct EncodedFrame
{
    std::vector<uint8_t> encodedData;
    uint64_t pictureOrderCountNumber = 0;
    uint64_t decodingOrderNumber = 0;
    bool isKeyFrame = false;
};

class IEncoder
{
public:
    virtual ~IEncoder() = default;
    virtual void PushFrame(const RawFrameData& rawFrameData) = 0;
    virtual bool StartEncodingPushedFrame() = 0;
    virtual bool WaitForEncodedFrame(EncodedFrame& encodedFrame) = 0;
    virtual void Flush() = 0;
    virtual void Terminate() = 0;
};

struct EncoderConfiguration
{
    uint32_t width{};
    uint32_t height{};

    struct {
        uint32_t numerator;
        uint32_t denominator;
    } fps;

    uint32_t keyFrameInterval{}; // 0 - infinite GOP
    uint32_t bFramesCount{}; // amount of B-frames between I/P frames
    uint32_t maxReferenceFrameCount{};
};

std::unique_ptr<IEncoder> CreateH264Encoder(
    const Microsoft::WRL::ComPtr<ID3D12Device>& device,
    const EncoderConfiguration&);

}
