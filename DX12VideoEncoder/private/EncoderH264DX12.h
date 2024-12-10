#pragma once

#include "EncoderAPI.h"
#include "ReferenceFramesManager.h"
#include "InputFrameResources.h"

class d3d12_video_bitstream_builder_h264;

namespace DX12VideoEncoding {

using Microsoft::WRL::ComPtr;

class EncoderH264DX12
{
public:

    EncoderH264DX12(const ComPtr<ID3D12Device>& device, const EncoderConfiguration& config,
        DXGI_FORMAT inputFormat);
    ~EncoderH264DX12();

    struct InputFrame
    {
        D3D12_VIDEO_ENCODER_FRAME_TYPE_H264 frameType{ D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME };
        UINT pictureOrderCountNumber{};
        UINT decodingOrderNumber{};
        UINT idrPicId{};
        std::vector<UINT> l0List;
        std::vector<UINT> l1List;
        bool useAsReference{ false };
    };

    void SendFrame(const InputFrame& inputFrame, const InputFrameResources& inputFrameResources);
    bool WaitForEncodedData(Microsoft::WRL::Wrappers::Event& termintateEvent,
        std::vector<uint8_t>& encodedData);

private:
    void Configure(const EncoderConfiguration& config);
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264 ConfigureGOPStructure(UINT gopLengthInFrames, UINT bFramesCount);
    D3D12_VIDEO_ENCODER_OUTPUT_METADATA ReadResolvedMetadata();
    void ReadEncodedData(std::vector<uint8_t>& encodedData);
    uint32_t BuildCodecHeadersH264();
    void PrepareForNextFrame();
    void UpdateCurrentFrameInfo(InputFrame& inputFrame);
    void UploadBitstreamHeaders();
    void CreateEncodeCommand();
    void CreateOutputCommand();
    void CreateOutputBufferResource();


private:
    const uint32_t m_maxReferenceFrameCount;
    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC m_resolutionDesc = {};
    DXGI_RATIONAL m_targetFramerate = {};
    D3D12_BOX m_frameCropping = {};
    const DXGI_FORMAT m_inputFormat;

    // DX12 context.

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12VideoDevice3> m_videoDevice;
    ComPtr<ID3D12VideoEncoder> m_videoEncoder;
    ComPtr<ID3D12VideoEncoderHeap> m_videoEncoderHeap;


    // Resources for reading encoded data.

    ComPtr<ID3D12CommandQueue> m_outputEncodedCommandQueue;
    ComPtr<ID3D12CommandAllocator> m_outputEncodedCommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_outputEncodedCommandList;


    // Resources for encoding.

    ComPtr<ID3D12CommandQueue> m_encodeCommandQueue;
    ComPtr<ID3D12CommandAllocator> m_encodeCommandAllocator;
    ComPtr<ID3D12VideoEncodeCommandList2> m_encodeCommandList;
    ComPtr<ID3D12Fence> m_encoderFence;
    UINT64 m_encoderFenceValue = 1;

    ComPtr<ID3D12Resource> m_resolvedMetadataBuffer;
    ComPtr<ID3D12Resource> m_metadataOutputBuffer;
    ComPtr<ID3D12Resource> m_outputBitrstreamBuffer;



    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS m_resourceRequirements = {};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 m_codecH264Config = {};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION m_codecConfiguration = {};

    D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP m_rateControlCQP = {};
    D3D12_VIDEO_ENCODER_RATE_CONTROL m_rateControl = {};

    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264 m_h264GopStructure = {};
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE m_gopStructure = {};

    D3D12_VIDEO_ENCODER_LEVELS_H264 m_selectedLevel = D3D12_VIDEO_ENCODER_LEVELS_H264_42;
    D3D12_VIDEO_ENCODER_PROFILE_H264 m_h264Profile = D3D12_VIDEO_ENCODER_PROFILE_H264_MAIN;
    D3D12_VIDEO_ENCODER_PROFILE_DESC m_profileDesc = {};

    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264 m_h264PicData = {};
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA m_curPicParamsData = {};

    InputFrame m_currentFrame;
    std::vector<uint8_t> m_bitstreamHeadersBuffer;
    std::unique_ptr<d3d12_video_bitstream_builder_h264> m_bitstreamBuilder;

    UINT64 m_resolvedMetadataBufferSize = 0;

    std::unique_ptr<ReferenceFramesManager> m_referenceFramesManager;

    Microsoft::WRL::Wrappers::Event m_encodeCompletedEvent;
};

}
