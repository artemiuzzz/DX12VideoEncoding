#include "pch.h"
#include "ReferenceFramesManager.h"
#include "Utils.h"


namespace DX12VideoEncoding {

ReferenceFramesManager::ReferenceFramesManager(
    const ComPtr<ID3D12Device>& device,
    const D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC& resolutionDesc,
    DXGI_FORMAT inputFormat,
    uint32_t maxReferenceFrameCount,
    bool gopHasInterFrames)
    : m_device(device)
    , m_resolutionDesc(resolutionDesc)
    , m_inputFormat(inputFormat)
    , m_maxReferenceFrameCount(maxReferenceFrameCount)
    , m_gopHasInterFrames(gopHasInterFrames)
{
    if (m_gopHasInterFrames)
        AllocateTextures(maxReferenceFrameCount);
}

D3D12_VIDEO_ENCODER_RECONSTRUCTED_PICTURE ReferenceFramesManager::GetReconstructedPicture()
{
    return m_reconstructedPicture;
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
        || !m_gopHasInterFrames)
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


namespace {

void MapDecodingOrderToReferenceFrameIndex(
    const std::vector<D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_H264>& referenceFrameDescriptors,
    UINT* listReferenceFrames,
    UINT listReferenceFramesCount)
{
    std::vector<UINT> referenceFrameIndices(listReferenceFrames, listReferenceFrames + listReferenceFramesCount);
    for (size_t index = 0; index < listReferenceFramesCount; ++index)
    {
        auto foundItemIt = std::find_if(
            referenceFrameDescriptors.begin(),
            referenceFrameDescriptors.end(),
            [value = referenceFrameIndices[index]]
            (const D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_H264& desc)
            {
                return desc.PictureOrderCountNumber == value;
            });

        assert(foundItemIt != referenceFrameDescriptors.end());
        listReferenceFrames[index] = std::distance(referenceFrameDescriptors.begin(), foundItemIt);
    }
}

}

void ReferenceFramesManager::GetPictureControlCodecData(
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA& pictureControlCodecData)
{
    bool usesL0RefFrames = (m_currentH264PicData.FrameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME) ||
        (m_currentH264PicData.FrameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_B_FRAME);

    bool usesL1RefFrames = m_currentH264PicData.FrameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_B_FRAME;

    if (usesL0RefFrames && (m_currentH264PicData.List0ReferenceFramesCount > 0))
    {
        MapDecodingOrderToReferenceFrameIndex(m_referenceFrameDescriptors,
            m_currentH264PicData.pList0ReferenceFrames, m_currentH264PicData.List0ReferenceFramesCount);
    }
    if (usesL1RefFrames && (m_currentH264PicData.List1ReferenceFramesCount > 0))
    {
        MapDecodingOrderToReferenceFrameIndex(m_referenceFrameDescriptors,
            m_currentH264PicData.pList1ReferenceFrames, m_currentH264PicData.List1ReferenceFramesCount);
    }

    if (!usesL0RefFrames)
    {
        m_currentH264PicData.List0ReferenceFramesCount = 0;
        m_currentH264PicData.pList0ReferenceFrames = nullptr;
    }
    if (!usesL1RefFrames)
    {
        m_currentH264PicData.List1ReferenceFramesCount = 0;
        m_currentH264PicData.pList1ReferenceFrames = nullptr;
    }

    m_currentH264PicData.ReferenceFramesReconPictureDescriptorsCount =
        usesL0RefFrames ? static_cast<uint32_t>(m_referenceFrameDescriptors.size()) : 0;

    m_currentH264PicData.pReferenceFramesReconPictureDescriptors =
        usesL0RefFrames ? m_referenceFrameDescriptors.data() : nullptr;


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
    m_reconstructedPicture = {};
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
    m_reconstructedPicture = { nullptr, 0 };

    if (!IsCurrentFrameUsedAsReference() || !m_gopHasInterFrames)
        return;

    if (m_freeTextures.empty())
    {
        auto reconstructedPic = CreateTexture();
        m_usedTextures.insert(reconstructedPic);
        m_reconstructedPictureResource = std::move(reconstructedPic);
    }
    else
    {
        auto iter = m_freeTextures.begin();
        m_reconstructedPictureResource = *iter;
        m_freeTextures.erase(iter);
        m_usedTextures.insert(m_reconstructedPictureResource);
    }

    assert((m_freeTextures.size() + m_usedTextures.size())
        <= (m_maxReferenceFrameCount + 1 /* One additional is allowed for reconstructed pic */));

    m_reconstructedPicture.pReconstructedPicture = m_reconstructedPictureResource.Get();
    m_reconstructedPicture.ReconstructedPictureSubresource = 0;
}

ComPtr<ID3D12Resource> ReferenceFramesManager::CreateTexture()
{
    D3D12_HEAP_PROPERTIES heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    CD3DX12_RESOURCE_DESC reconstructedPictureResourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        m_inputFormat,
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
    assert(!(m_referenceFrameDescriptors.empty() || m_referenceFramesResources.empty()));
    if (m_referenceFrameDescriptors.empty() || m_referenceFramesResources.empty())
        return;

    auto oldestResource = m_referenceFramesResources.rbegin();
    if (auto usedTextureIter = m_usedTextures.find(*oldestResource); usedTextureIter != m_usedTextures.end())
    {
        m_freeTextures.insert(*usedTextureIter);
        m_usedTextures.erase(usedTextureIter);
    }

    m_referenceFramesResources.pop_back();
    m_referenceFrameDescriptors.pop_back();
}

}
