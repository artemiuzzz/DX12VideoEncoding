#pragma once

namespace DX12VideoEncoding {

using Microsoft::WRL::ComPtr;

class ReferenceFramesManager
{
public:
    ReferenceFramesManager(
        const ComPtr<ID3D12Device>& device,
        const D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC& resolutionDesc,
        DXGI_FORMAT inputFormat,
        uint32_t maxReferenceFrameCount,
        bool gopHasInterFrames
    );

    D3D12_VIDEO_ENCODER_RECONSTRUCTED_PICTURE GetReconstructedPicture();

    size_t GetReferenceFrameCount() const;

    D3D12_VIDEO_ENCODE_REFERENCE_FRAMES GetReferenceFrames();

    void PrepareForEncodingFrame(D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA currentPicParamsData,
        bool useFrameAsReference);

    bool IsCurrentFrameUsedAsReference() const;

    void GetPictureControlCodecData(D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA& pictureControlCodecData);

    void UpdateReferenceFrames();

private:
    void Reset();
    void AllocateTextures(int size);
    void CreateReconstructedPictureResource();
    ComPtr<ID3D12Resource> CreateTexture();
    void RemoveOldestReferenceFrame();

private:
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264 m_currentH264PicData = {};
    ComPtr<ID3D12Resource> m_reconstructedPictureResource;
    D3D12_VIDEO_ENCODER_RECONSTRUCTED_PICTURE m_reconstructedPicture = {};

    std::vector<D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_H264> m_referenceFrameDescriptors;
    std::vector<ID3D12Resource*> m_referenceFramesResources;

    std::set<ComPtr<ID3D12Resource>> m_freeTextures;
    std::set<ComPtr<ID3D12Resource>> m_usedTextures;


    const ComPtr<ID3D12Device> m_device;
    const D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC m_resolutionDesc;
    const DXGI_FORMAT m_inputFormat;
    const uint32_t m_maxReferenceFrameCount;
    const bool m_gopHasInterFrames;
    bool m_isCurrentFrameReference = false;
};

}
