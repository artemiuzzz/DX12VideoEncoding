#include "pch.h"
#include "ReferenceFramesManager.h"
#include "Utils.h"


namespace DX12VideoEncoding {

ReferenceFramesManager::ReferenceFramesManager(uint32_t maxReferenceFrameCount,
    bool GOPhasInterFrames,
    const ComPtr<ID3D12Device>& device,
    const D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC& resolutionDesc)
    : m_device(device)
    , m_resolutionDesc(resolutionDesc)
    , m_maxReferenceFrameCount(maxReferenceFrameCount)
    , m_GOPhasInterFrames(GOPhasInterFrames)
{
    if (m_GOPhasInterFrames)
        AllocateTextures(maxReferenceFrameCount);
}

D3D12_VIDEO_ENCODER_RECONSTRUCTED_PICTURE ReferenceFramesManager::GetReconstructedPicture()
{
    return m_reconstructuredPicture;
}

size_t ReferenceFramesManager::GetReferenceFrameCount() const
{
    return m_referenceFramesResources.size();
}

D3D12_VIDEO_ENCODE_REFERENCE_FRAMES ReferenceFramesManager::GetReferenceFrames()
{
    D3D12_VIDEO_ENCODE_REFERENCE_FRAMES result = {};

    if (m_currentH264PicData.FrameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME
        || m_currentH264PicData.FrameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_I_FRAME
        || !m_GOPhasInterFrames)
    {
        return result;
    }

    result.NumTexture2Ds = static_cast<UINT>(m_referenceFramesResources.size());
    result.ppTexture2Ds = m_referenceFramesResources.data();
    return result;
}

void ReferenceFramesManager::PrepareForEncodingFrame(
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA currentPicParamsData, bool useFrameAsReference)
{
    m_currentH264PicData = *currentPicParamsData.pH264PicData;
    m_isCurrentFrameReference = useFrameAsReference;

    if (m_currentH264PicData.FrameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME)
    {
        Reset();
    }
    CreateReconstructedPictureResource();
}

bool ReferenceFramesManager::IsCurrentFrameUsedAsReference() const
{
    return m_isCurrentFrameReference;
}

void ReferenceFramesManager::GetPictureControlCodecData(
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA& pictureControlCodecData)
{
    bool usesL0RefFrames = (m_currentH264PicData.FrameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME) ||
        (m_currentH264PicData.FrameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_B_FRAME);

    // No support for B-frames for now.
    m_currentH264PicData.pList1ReferenceFrames = nullptr;
    m_currentH264PicData.List1ReferenceFramesCount = 0;

    if (usesL0RefFrames)
    {
        m_currentH264PicData.ReferenceFramesReconPictureDescriptorsCount =
            static_cast<uint32_t>(m_referenceFrameDescriptors.size());
        m_currentH264PicData.pReferenceFramesReconPictureDescriptors = m_referenceFrameDescriptors.data();
    }
    else
    {
        // Remove reference data.
        m_currentH264PicData.ReferenceFramesReconPictureDescriptorsCount = 0;
        m_currentH264PicData.pReferenceFramesReconPictureDescriptors = nullptr;
        m_currentH264PicData.pList0ReferenceFrames = nullptr;
        m_currentH264PicData.List0ReferenceFramesCount = 0;
    }

    *pictureControlCodecData.pH264PicData = m_currentH264PicData;
}

void ReferenceFramesManager::UpdateReferenceFrames()
{
    if (!IsCurrentFrameUsedAsReference())
        return;

    if (m_referenceFramesResources.size() >= m_maxReferenceFrameCount)
    {
        RemoveOldestReferenceFrame();
    }

    // Add reconstructed pic to the ref frames.
    D3D12_VIDEO_ENCODER_RECONSTRUCTED_PICTURE reconPic = GetReconstructedPicture();
    m_referenceFramesResources.insert(m_referenceFramesResources.begin(), reconPic.pReconstructedPicture);

    const D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_H264 descriptorH264 = {
        .ReconstructedPictureResourceIndex = 0,
        .IsLongTermReference = FALSE,
        .LongTermPictureIdx = 0,
        .PictureOrderCountNumber = m_currentH264PicData.PictureOrderCountNumber,
        .FrameDecodingOrderNumber = m_currentH264PicData.FrameDecodingOrderNumber,
        .TemporalLayerIndex = 0
    };

    m_referenceFrameDescriptors.insert(m_referenceFrameDescriptors.begin(), descriptorH264);

    for (size_t i = 1; i < m_referenceFrameDescriptors.size(); ++i)
    {
        m_referenceFrameDescriptors[i].ReconstructedPictureResourceIndex = i;
    }
}

void ReferenceFramesManager::Reset()
{
    m_referenceFrameDescriptors.clear();
    m_referenceFramesResources.clear();
    m_reconstructuredPicture = {};
    m_freeTextures.merge(m_usedTextures);
}

void ReferenceFramesManager::AllocateTextures(int size)
{
    for (int i = 0; i < size; ++i)
    {
        m_freeTextures.emplace(CreateTexture());
    }
}

void ReferenceFramesManager::CreateReconstructedPictureResource()
{
    m_reconstructuredPicture = { nullptr, 0 };

    if (!IsCurrentFrameUsedAsReference() || !m_GOPhasInterFrames)
        return;

    if (m_freeTextures.empty())
    {
        auto reconstructedPic = CreateTexture();
        m_freeTextures.insert(reconstructedPic);
        m_reconstructuredPictureResource = std::move(reconstructedPic);
    }
    else
    {
        auto iter = m_freeTextures.begin();
        m_reconstructuredPictureResource = *iter;
        m_freeTextures.erase(iter);
        m_usedTextures.insert(m_reconstructuredPictureResource);
    }

    assert((m_freeTextures.size() + m_usedTextures.size()) <= m_maxReferenceFrameCount);

    m_reconstructuredPicture.pReconstructedPicture = m_reconstructuredPictureResource.Get();
    m_reconstructuredPicture.ReconstructedPictureSubresource = 0;
}

ComPtr<ID3D12Resource> ReferenceFramesManager::CreateTexture()
{
    D3D12_HEAP_PROPERTIES heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    CD3DX12_RESOURCE_DESC reconstructedPictureResourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_NV12,
        m_resolutionDesc.Width,
        m_resolutionDesc.Height,
        1, // arraySize
        1, // mipLevels
        1, // sampleCount
        0, // sampleQuality
        D3D12_RESOURCE_FLAG_VIDEO_ENCODE_REFERENCE_ONLY | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);

    ComPtr<ID3D12Resource> result;
    ThrowIfFailed(m_device->CreateCommittedResource(&heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &reconstructedPictureResourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&result)));

    return result;
}

void ReferenceFramesManager::RemoveOldestReferenceFrame()
{
    if (!m_referenceFramesResources.empty())
    {
        auto oldestResource = m_referenceFramesResources.rbegin();
        if (auto usedTextureIter = m_usedTextures.find(*oldestResource); usedTextureIter != m_usedTextures.end())
        {
            m_freeTextures.insert(*usedTextureIter);
            m_usedTextures.erase(usedTextureIter);
        }

        m_referenceFramesResources.pop_back();
    }

    m_referenceFrameDescriptors.pop_back();
}

}
