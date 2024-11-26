#pragma once

#include "FrameNV12.h"
#include "ReferenceFramesManager.h"


class d3d12_video_bitstream_builder_h264;

namespace DX12VideoEncoding {

using Microsoft::WRL::ComPtr;

class Encoder
{
public:
    struct Configuration
    {
        UINT width{};
        UINT height{};
        DXGI_RATIONAL fps{};
        UINT maxReferenceFrameCount{};
        UINT gopLengthInFrames{};
        bool usePframes{};
    };

    Encoder(const ComPtr<ID3D12Device>& device, const Configuration& config);
    ~Encoder();

    void Flush();
    bool IsFlushed() const;

    void SendFrame(const FrameNV12& frame);

    struct EncodedFrame
    {
        std::vector<uint8_t> encodedData;
        UINT displayOrderNumber = 0;
        bool isKeyFrame = false;
    };
    std::shared_ptr<EncodedFrame> WaitForEncodedFrame();

private:
    void Configure(const Configuration& config);
    void ConfigureGOPStructure(UINT gopLengthInFrames, bool usePframes);
    D3D12_VIDEO_ENCODER_OUTPUT_METADATA ReadResolvedMetadata();
    std::shared_ptr<EncodedFrame> ReadEncodedData();
    uint32_t BuildCodecHeadersH264();
    void PrepareForNextFrame();
    void UpdateCurrentFrameInfo();
    void UploadBitstreamHeaders();
    void CopyFrameToInputTexture(const FrameNV12& frame);
    void WaitForInputTexture();
    void CreateEncodeCommand();
    void CreateInputCommand();
    void CreateOutputCommand();
    void CreateOutputBufferResource();
    void CreateInputResource();

private:
    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC m_resolutionDesc = {};
    DXGI_RATIONAL m_targetFramerate = {};
    D3D12_BOX m_frameCropping = {};

    // DX12 context.

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12VideoDevice3> m_videoDevice;
    ComPtr<ID3D12VideoEncoder> m_videoEncoder;
    ComPtr<ID3D12VideoEncoderHeap> m_videoEncoderHeap;


    // Resources for raw frame input.

    ComPtr<ID3D12CommandQueue> m_inputTextureCommandQueue;
    ComPtr<ID3D12CommandAllocator> m_inputTextureCommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_inputTextureCommandList;
    ComPtr<ID3D12Fence> m_inputTextureFence;
    UINT64 m_inputTextureFenceValue = 1;

    ComPtr<ID3D12Resource> m_frameUploadBuffer;
    ComPtr<ID3D12Resource> m_encoderInputResource;


    // Resources for reading encoded data.

    ComPtr<ID3D12CommandQueue> m_outputEncodedCommandQueue;
    ComPtr<ID3D12CommandAllocator> m_outputEncodedCommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_outputEncodedCommandList;


    // Resources for encoding.

    ComPtr<ID3D12CommandQueue> m_encodeCommandQueue;
    ComPtr<ID3D12CommandAllocator> m_encodeCommandAllocator;
    ComPtr<ID3D12VideoEncodeCommandList2> m_encodeCommandList;
    ComPtr<ID3D12Fence> m_encoderFence;
    std::atomic<UINT64> m_encoderFenceValue = 1;

    ComPtr<ID3D12Resource> m_resolvedMetadataBuffer;
    ComPtr<ID3D12Resource> m_metadataOutputBuffer;
    ComPtr<ID3D12Resource> m_outputBitrstreamBuffer;



    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS m_resourceRequirements = {};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 m_codecH264Config = {};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION m_codecConfiguration = {};

    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264 m_h264GopStructure = {};
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE m_gopStructure = {};

    D3D12_VIDEO_ENCODER_LEVELS_H264 m_selectedLevel = D3D12_VIDEO_ENCODER_LEVELS_H264_11;
    D3D12_VIDEO_ENCODER_PROFILE_H264 m_h264Profile = D3D12_VIDEO_ENCODER_PROFILE_H264_MAIN;
    D3D12_VIDEO_ENCODER_PROFILE_DESC m_profileDesc = {};

    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264 m_h264PicData = {};
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA m_curPicParamsData = {};

    std::vector<uint8_t> m_bitstreamHeadersBuffer;
    std::unique_ptr<d3d12_video_bitstream_builder_h264> m_bitstreamBuilder;

    struct FrameInfo
    {
        UINT displayOrderNumber = 0;
        UINT lastIdrNumber = 0;
        UINT idrPicId = 0;
        std::vector<UINT> list0ReferenceFramesCount;
    };
    FrameInfo m_currentFrame;

    UINT64 m_resolvedMetadataBufferSize = 0;

    const uint32_t m_maxReferenceFrameCount = 0;
    std::unique_ptr<ReferenceFramesManager> m_referenceFramesManager;

    std::atomic_bool m_flushRequested{ false };
    std::atomic_bool m_isFlushed{ false };
    Microsoft::WRL::Wrappers::Event m_encodeCompletedEvent;

    // For now allow only 1 thread that sends frame, that's why semaphore is
    // binary and not counting. The semaphore is acquired in the sending thread
    // and is released in consumer thread, thus preventing writing new frame while
    // the encoded frame has not been read yet by the consumer thread.
    // Initially it is in signalled state {1} so that the first encoding could start.
    std::binary_semaphore m_consumerToEncoderSemaphore{ 1 };
};

}
