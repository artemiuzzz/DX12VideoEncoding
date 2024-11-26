#include "pch.h"
#include "Encoder.h"
#include "Utils.h"
#include "gallium/d3d12_video_encoder_bitstream_builder_h264.h"


namespace DX12VideoEncoding {

using namespace Microsoft::WRL::Wrappers;

namespace {

void ThrowEncodingError(UINT64 encodeErrorFlags)
{
    std::string error("Encoding error:");
    if (D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED & encodeErrorFlags)
    {
        error.append("\nD3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED");
    }
    if (D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_SUBREGION_LAYOUT_CONFIGURATION_NOT_SUPPORTED & encodeErrorFlags)
    {
        error.append("\nD3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_SUBREGION_LAYOUT_CONFIGURATION_NOT_SUPPORTED");
    }
    if (D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_INVALID_REFERENCE_PICTURES & encodeErrorFlags)
    {
        error.append("\nD3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_INVALID_REFERENCE_PICTURES");
    }
    if (D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_RECONFIGURATION_REQUEST_NOT_SUPPORTED & encodeErrorFlags)
    {
        error.append("\nD3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_RECONFIGURATION_REQUEST_NOT_SUPPORTED");
    }
    if (D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_INVALID_METADATA_BUFFER_SOURCE & encodeErrorFlags)
    {
        error.append("\nD3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_INVALID_METADATA_BUFFER_SOURCE");
    }
    throw std::runtime_error(error);
}

D3D12_BOX H264FrameCroppingBox(D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC resolution)
{
    const UINT mbWidth = (resolution.Width + 15) / 16;
    const UINT mbHeight = (resolution.Height + 15) / 16;
    D3D12_BOX frameCropping{};
    if (mbWidth || mbHeight)
    {
        frameCropping.right = (16 * mbWidth - resolution.Width) / 2;
        frameCropping.bottom = (16 * mbHeight - resolution.Height) / 2;
    }
    return frameCropping;
}

}

Encoder::Encoder(const ComPtr<ID3D12Device> &device,
    const Configuration& config)
    : m_device(device)
    , m_maxReferenceFrameCount(config.maxReferenceFrameCount)
    , m_bitstreamBuilder(std::make_unique<d3d12_video_bitstream_builder_h264>())
{
    ThrowIfFailed(m_device->QueryInterface(IID_PPV_ARGS(&m_videoDevice)));
    m_encodeCompletedEvent.Attach(CreateEvent(NULL, FALSE, FALSE, TEXT("encodeCompletedEvent")));
    Configure(config);
}

Encoder::~Encoder() = default;

void Encoder::Flush()
{
    m_flushRequested = true;
}

bool Encoder::IsFlushed() const
{
    return m_isFlushed;
}

void Encoder::Configure(const Configuration& config)
{
    m_resolutionDesc.Width = config.width;
    m_resolutionDesc.Height = config.height;
    m_frameCropping = H264FrameCroppingBox(m_resolutionDesc);
    m_targetFramerate = config.fps;

    m_profileDesc.pH264Profile = &m_h264Profile;
    m_profileDesc.DataSize = sizeof(m_h264Profile);

    D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL profileLevel = {};
    profileLevel.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
    profileLevel.Profile = m_profileDesc;
    D3D12_VIDEO_ENCODER_LEVELS_H264 minLevelH264 = {};
    D3D12_VIDEO_ENCODER_LEVELS_H264 maxLevelH264 = {};
    D3D12_VIDEO_ENCODER_LEVEL_SETTING minLevel = {};
    minLevel.pH264LevelSetting = &minLevelH264;
    minLevel.DataSize = sizeof(minLevelH264);
    D3D12_VIDEO_ENCODER_LEVEL_SETTING maxLevel = {};
    maxLevel.pH264LevelSetting = &maxLevelH264;
    maxLevel.DataSize = sizeof(maxLevelH264);
    profileLevel.MinSupportedLevel = minLevel;
    profileLevel.MaxSupportedLevel = maxLevel;
    ThrowIfFailed(m_videoDevice->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL,
        &profileLevel, sizeof(profileLevel)));
    ThrowIfFalse(profileLevel.IsSupported == TRUE);


    D3D12_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT_H264 h264PictureControl = {};
    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT capPictureControlData = {};
    capPictureControlData.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
    capPictureControlData.Profile.pH264Profile = &m_h264Profile;
    capPictureControlData.Profile.DataSize = sizeof(m_h264Profile);
    capPictureControlData.PictureSupport.pH264Support = &h264PictureControl;
    capPictureControlData.PictureSupport.DataSize = sizeof(h264PictureControl);
    ThrowIfFailed(m_videoDevice->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT,
        &capPictureControlData,
        sizeof(capPictureControlData)));

    ThrowIfFalse(capPictureControlData.IsSupported == TRUE);


    D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT inputFormat = {};
    inputFormat.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
    inputFormat.Profile = profileLevel.Profile;
    inputFormat.Format = DXGI_FORMAT_NV12;

    ThrowIfFailed(m_videoDevice->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT,
        &inputFormat, sizeof(inputFormat)));


    m_resourceRequirements.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
    m_resourceRequirements.Profile = inputFormat.Profile;
    m_resourceRequirements.InputFormat = inputFormat.Format;
    m_resourceRequirements.PictureTargetResolution = m_resolutionDesc;
    ThrowIfFailed(m_videoDevice->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS,
        &m_resourceRequirements, sizeof(m_resourceRequirements)));

    ThrowIfFalse(m_resourceRequirements.IsSupported == TRUE);

    m_resolvedMetadataBufferSize = D3DX12Align<UINT64>(
        sizeof(D3D12_VIDEO_ENCODER_OUTPUT_METADATA) + sizeof(D3D12_VIDEO_ENCODER_FRAME_SUBREGION_METADATA),
        m_resourceRequirements.EncoderMetadataBufferAccessAlignment);


    D3D12_VIDEO_ENCODER_DESC encoderDesc = {};
    encoderDesc.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_H264;

    encoderDesc.EncodeProfile = profileLevel.Profile;
    encoderDesc.InputFormat = DXGI_FORMAT_NV12;

    m_codecConfiguration.pH264Config = &m_codecH264Config;
    m_codecConfiguration.DataSize = sizeof(m_codecH264Config);
    encoderDesc.CodecConfiguration = m_codecConfiguration;

    ThrowIfFailed(m_videoDevice->CreateVideoEncoder(&encoderDesc, IID_PPV_ARGS(&m_videoEncoder)));


    D3D12_VIDEO_ENCODER_HEAP_DESC encoderHeapDesc = {};
    encoderHeapDesc.Flags = D3D12_VIDEO_ENCODER_HEAP_FLAG_NONE;
    encoderHeapDesc.EncodeCodec = encoderDesc.EncodeCodec;
    encoderHeapDesc.EncodeProfile = encoderDesc.EncodeProfile;
    encoderHeapDesc.EncodeLevel.pHEVCLevelSetting = nullptr;
    encoderHeapDesc.EncodeLevel = maxLevel;


    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC resolutionDescList[] = { m_resolutionDesc };
    encoderHeapDesc.ResolutionsListCount = _countof(resolutionDescList);
    encoderHeapDesc.pResolutionList = resolutionDescList;

    ThrowIfFailed(m_videoDevice->CreateVideoEncoderHeap(&encoderHeapDesc, IID_PPV_ARGS(&m_videoEncoderHeap)));

    ConfigureGOPStructure(config.gopLengthInFrames, config.usePframes);

    m_referenceFramesManager = std::make_unique<ReferenceFramesManager>(
        m_maxReferenceFrameCount, config.usePframes, m_device, m_resolutionDesc);

    CreateInputResource();
    CreateOutputBufferResource();
    CreateEncodeCommand();
    CreateInputCommand();
    CreateOutputCommand();
}

void Encoder::ConfigureGOPStructure(UINT gopLengthInFrames, bool usePframes)
{
    m_h264GopStructure.GOPLength = usePframes ? gopLengthInFrames : 1;
    m_h264GopStructure.PPicturePeriod = usePframes ? 1 /* Without B frames */ : 0;

    m_h264GopStructure.pic_order_cnt_type = 0;
    const uint32_t max_pic_order_cnt_lsb = 2 * m_h264GopStructure.GOPLength;
    const uint32_t max_max_frame_num = m_h264GopStructure.GOPLength;
    double log2_max_frame_num_minus4 = (std::max)(0.0, std::ceil(std::log2(max_max_frame_num)) - 4);
    double log2_max_pic_order_cnt_lsb_minus4 = (std::max)(0.0, std::ceil(std::log2(max_pic_order_cnt_lsb)) - 4);
    assert(log2_max_frame_num_minus4 < UCHAR_MAX);
    assert(log2_max_pic_order_cnt_lsb_minus4 < UCHAR_MAX);
    m_h264GopStructure.log2_max_frame_num_minus4 = static_cast<UCHAR>(log2_max_frame_num_minus4);
    m_h264GopStructure.log2_max_pic_order_cnt_lsb_minus4 = static_cast<UCHAR>(log2_max_pic_order_cnt_lsb_minus4);

    m_gopStructure.pH264GroupOfPictures = &m_h264GopStructure;
    m_gopStructure.DataSize = sizeof(m_h264GopStructure);
}

void Encoder::SendFrame(const FrameNV12& frame)
{
    // Resolution change is not supported for now.
    ThrowIfFalse((frame.height == m_resolutionDesc.Height) && (frame.width == m_resolutionDesc.Width));

    CopyFrameToInputTexture(frame);

    m_consumerToEncoderSemaphore.acquire();

    // Wait on GPU for completion of copying the frame to input texture.
    ThrowIfFailed(m_encodeCommandQueue->Wait(m_inputTextureFence.Get(), m_inputTextureFenceValue));

    UpdateCurrentFrameInfo();
    bool isCurrentFrameUsedAsReference = true;
    m_referenceFramesManager->PrepareForEncodingFrame(m_curPicParamsData, isCurrentFrameUsedAsReference);
    m_referenceFramesManager->GetPictureControlCodecData(m_curPicParamsData);


    D3D12_RESOURCE_BARRIER currentFrameStateTransitions[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(m_encoderInputResource.Get(),
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ),
        CD3DX12_RESOURCE_BARRIER::Transition(m_outputBitrstreamBuffer.Get(),
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE),
        CD3DX12_RESOURCE_BARRIER::Transition(m_metadataOutputBuffer.Get(),
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE)
    };

    m_encodeCommandList->ResourceBarrier(_countof(currentFrameStateTransitions),
        currentFrameStateTransitions);

    const D3D12_VIDEO_ENCODE_REFERENCE_FRAMES referenceFrames = m_referenceFramesManager->GetReferenceFrames();
    std::vector<D3D12_RESOURCE_BARRIER> refFramesTransitions;
    for (UINT index = 0; index < referenceFrames.NumTexture2Ds; ++index)
    {
        refFramesTransitions.push_back(
            CD3DX12_RESOURCE_BARRIER::Transition(referenceFrames.ppTexture2Ds[index],
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ)
        );
    }
    if (m_referenceFramesManager->IsCurrentFrameUsedAsReference())
    {
        const D3D12_VIDEO_ENCODER_RECONSTRUCTED_PICTURE reconstructedPicture =
            m_referenceFramesManager->GetReconstructedPicture();
        if (reconstructedPicture.pReconstructedPicture != nullptr)
        {
            refFramesTransitions.push_back(
                CD3DX12_RESOURCE_BARRIER::Transition(reconstructedPicture.pReconstructedPicture,
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE)
            );
        }
    }
    if (!refFramesTransitions.empty())
    {
        m_encodeCommandList->ResourceBarrier(refFramesTransitions.size(), refFramesTransitions.data());
    }

    uint32_t prefixGeneratedHeadersByteSize = BuildCodecHeadersH264();
    if ((m_resourceRequirements.CompressedBitstreamBufferAccessAlignment > 1)
        && ((prefixGeneratedHeadersByteSize % m_resourceRequirements.CompressedBitstreamBufferAccessAlignment) != 0))
    {
        prefixGeneratedHeadersByteSize = D3DX12Align(prefixGeneratedHeadersByteSize,
            m_resourceRequirements.CompressedBitstreamBufferAccessAlignment);

        m_bitstreamHeadersBuffer.resize(prefixGeneratedHeadersByteSize, 0);
    }

    D3D12_VIDEO_ENCODER_RATE_CONTROL rateControl = {};
    rateControl.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
    D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP rateControlCQP = {
        30, // ConstantQP_FullIntracodedFrame
        30, // ConstantQP_InterPredictedFrame_PrevRefOnly
        30, // ConstantQP_InterPredictedFrame_BiDirectionalRef
    };
    rateControl.ConfigParams.DataSize = sizeof(rateControlCQP);
    rateControl.ConfigParams.pConfiguration_CQP = &rateControlCQP;
    rateControl.Flags = D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_NONE;
    rateControl.TargetFrameRate = m_targetFramerate;

    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAGS pictureControlFlags = D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_NONE;
    D3D12_VIDEO_ENCODER_RECONSTRUCTED_PICTURE reconstructedPicture = m_referenceFramesManager->GetReconstructedPicture();
    if (reconstructedPicture.pReconstructedPicture != nullptr)
    {
        pictureControlFlags |= D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_USED_AS_REFERENCE_PICTURE;
    }

    const D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS inputArguments = {
        .SequenceControlDesc = D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_DESC
        {
            .Flags = D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_NONE,
            .IntraRefreshConfig = D3D12_VIDEO_ENCODER_INTRA_REFRESH
            {
                .Mode = D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE,
                .IntraRefreshDuration = 0,
            },
            .RateControl = rateControl,
            .PictureTargetResolution = m_resolutionDesc,
            .SelectedLayoutMode = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME,
            .FrameSubregionsLayoutData = {},
            .CodecGopSequence = m_gopStructure,
        },

        .PictureControlDesc = D3D12_VIDEO_ENCODER_PICTURE_CONTROL_DESC
        {
            .IntraRefreshFrameIndex = 0,
            .Flags = pictureControlFlags,
            .PictureControlCodecData = m_curPicParamsData,
            .ReferenceFrames = referenceFrames,
        },

        .pInputFrame = m_encoderInputResource.Get(),
        .InputFrameSubresource = 0,
        .CurrentFrameBitstreamMetadataSize = prefixGeneratedHeadersByteSize,
    };

    const D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS outputArguments =
    {
        .Bitstream = D3D12_VIDEO_ENCODER_COMPRESSED_BITSTREAM
        {
            .pBuffer = m_outputBitrstreamBuffer.Get(),
            .FrameStartOffset = prefixGeneratedHeadersByteSize // Bitstream headers in the beginning
        },
        .ReconstructedPicture = reconstructedPicture,
        .EncoderOutputMetadata = D3D12_VIDEO_ENCODER_ENCODE_OPERATION_METADATA_BUFFER
        {
            .pBuffer = m_metadataOutputBuffer.Get(),
            .Offset = 0
        }
    };

    assert(prefixGeneratedHeadersByteSize == m_bitstreamHeadersBuffer.size());

    // Upload bitstream headers to GPU (see description of CurrentFrameBitstreamMetadataSize in the doc).
    UploadBitstreamHeaders();


    m_encodeCommandList->EncodeFrame(m_videoEncoder.Get(),
        m_videoEncoderHeap.Get(), &inputArguments, &outputArguments);


    const D3D12_RESOURCE_BARRIER resolveMetadataStateTransitions[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(m_resolvedMetadataBuffer.Get(),
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE),
        CD3DX12_RESOURCE_BARRIER::Transition(m_metadataOutputBuffer.Get(),
            D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE,
            D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ),
        CD3DX12_RESOURCE_BARRIER::Transition(m_encoderInputResource.Get(),
            D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ,
            D3D12_RESOURCE_STATE_COMMON),
        CD3DX12_RESOURCE_BARRIER::Transition(m_outputBitrstreamBuffer.Get(),
            D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE,
            D3D12_RESOURCE_STATE_COMMON)
    };

    m_encodeCommandList->ResourceBarrier(_countof(resolveMetadataStateTransitions),
        resolveMetadataStateTransitions);



    const D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS inputMetadataArgs = {
        .EncoderCodec = D3D12_VIDEO_ENCODER_CODEC_H264,
        .EncoderProfile = m_profileDesc,
        .EncoderInputFormat = DXGI_FORMAT_NV12,
        .EncodedPictureEffectiveResolution = m_resolutionDesc,
        .HWLayoutMetadata = D3D12_VIDEO_ENCODER_ENCODE_OPERATION_METADATA_BUFFER
        {
            .pBuffer = m_metadataOutputBuffer.Get(),
            .Offset = 0
        }
    };

    const D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS outputMetadataArgs = {
        { m_resolvedMetadataBuffer.Get(), 0 }
    };
    m_encodeCommandList->ResolveEncoderOutputMetadata(&inputMetadataArgs, &outputMetadataArgs);

    // Reference frames transition back.
    if (!refFramesTransitions.empty())
    {
        for (auto& transition : refFramesTransitions)
        {
            std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
        }
        m_encodeCommandList->ResourceBarrier(refFramesTransitions.size(), refFramesTransitions.data());
    }

    const D3D12_RESOURCE_BARRIER rgRevertResolveMetadataStateTransitions[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(m_resolvedMetadataBuffer.Get(),
            D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE,
            D3D12_RESOURCE_STATE_COMMON),
        CD3DX12_RESOURCE_BARRIER::Transition(m_metadataOutputBuffer.Get(),
            D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ,
            D3D12_RESOURCE_STATE_COMMON),
    };

    m_encodeCommandList->ResourceBarrier(_countof(rgRevertResolveMetadataStateTransitions),
        rgRevertResolveMetadataStateTransitions);


    ThrowIfFailed(m_encodeCommandList->Close());
    ID3D12CommandList* commandLists[] = { m_encodeCommandList.Get() };
    m_encodeCommandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);

    ThrowIfFailed(m_encodeCommandQueue->Signal(m_encoderFence.Get(), m_encoderFenceValue));

    // Waiting on CPU because copying may not have completed yet,
    // and if we go out of scope, raw data can become unavailable.
    WaitForInputTexture();
}

D3D12_VIDEO_ENCODER_OUTPUT_METADATA Encoder::ReadResolvedMetadata()
{
    D3D12_VIDEO_ENCODER_OUTPUT_METADATA metadata;
    const auto metadataBufferSize = sizeof(metadata);
    void* resolvedMetadata = nullptr;
    D3D12_RANGE readRange = { 0, m_resolvedMetadataBufferSize };
    ThrowIfFailed(m_resolvedMetadataBuffer->Map(0, &readRange, &resolvedMetadata));
    memcpy(&metadata, resolvedMetadata, metadataBufferSize);
    m_resolvedMetadataBuffer->Unmap(0, nullptr);

    if (metadata.EncodeErrorFlags != D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_NO_ERROR)
    {
        ThrowEncodingError(metadata.EncodeErrorFlags);
    }

    return metadata;
}

std::shared_ptr<Encoder::EncodedFrame> Encoder::ReadEncodedData()
{
    const D3D12_VIDEO_ENCODER_OUTPUT_METADATA metadata = ReadResolvedMetadata();
    auto encodedFrameSize = metadata.EncodedBitstreamWrittenBytesCount + m_bitstreamHeadersBuffer.size();
    assert(encodedFrameSize);

    auto encodedFrame = std::make_shared<EncodedFrame>();
    encodedFrame->encodedData.resize(encodedFrameSize);

    void* encodedFrameData = nullptr;
    D3D12_RANGE readRange = { 0, encodedFrameSize };
    ThrowIfFailed(m_outputBitrstreamBuffer->Map(0, &readRange, &encodedFrameData));
    memcpy(encodedFrame->encodedData.data(), encodedFrameData, encodedFrameSize);
    m_outputBitrstreamBuffer->Unmap(0, nullptr);

    encodedFrame->displayOrderNumber = m_currentFrame.displayOrderNumber;
    encodedFrame->isKeyFrame =
        (m_curPicParamsData.pH264PicData->FrameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_I_FRAME)
        || (m_curPicParamsData.pH264PicData->FrameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME);

    return encodedFrame;
}

uint32_t Encoder::BuildCodecHeadersH264()
{
    size_t writtenSPSBytesCount = 0;
    bool isFirstFrame = (m_encoderFenceValue.load() == 1);
    bool writeNewSPS = isFirstFrame;

    uint32_t activeSeqParameterSetId = m_bitstreamBuilder->get_active_sps_id();

    if (writeNewSPS)
    {
        if (!isFirstFrame)
        {
            ++activeSeqParameterSetId;
            m_bitstreamBuilder->set_active_sps_id(activeSeqParameterSetId);
        }
        m_bitstreamBuilder->build_sps(*m_profileDesc.pH264Profile,
            m_selectedLevel,
            DXGI_FORMAT_NV12,
            *m_codecConfiguration.pH264Config,
            *m_gopStructure.pH264GroupOfPictures,
            activeSeqParameterSetId,
            m_maxReferenceFrameCount,
            m_resolutionDesc,
            m_frameCropping,
            m_bitstreamHeadersBuffer,
            m_bitstreamHeadersBuffer.begin(),
            writtenSPSBytesCount);
    }

    size_t writtenPPSBytesCount = 0;
    m_bitstreamBuilder->build_pps(*m_profileDesc.pH264Profile,
        *m_codecConfiguration.pH264Config,
        *m_curPicParamsData.pH264PicData,
        m_curPicParamsData.pH264PicData->pic_parameter_set_id,
        activeSeqParameterSetId,
        m_bitstreamHeadersBuffer,
        m_bitstreamHeadersBuffer.begin() + writtenSPSBytesCount,
        writtenPPSBytesCount);

    if (m_bitstreamHeadersBuffer.size() > (writtenPPSBytesCount + writtenSPSBytesCount))
    {
        m_bitstreamHeadersBuffer.resize(writtenPPSBytesCount + writtenSPSBytesCount);
    }

    return m_bitstreamHeadersBuffer.size();
}

void Encoder::PrepareForNextFrame()
{
    ThrowIfFailed(m_encodeCommandAllocator->Reset());
    ThrowIfFailed(m_encodeCommandList->Reset(m_encodeCommandAllocator.Get()));
    m_encoderFenceValue.fetch_add(1);

    ++m_currentFrame.displayOrderNumber;

    if(m_h264PicData.FrameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME)
        ++m_currentFrame.idrPicId;

    m_referenceFramesManager->UpdateReferenceFrames();
}

void Encoder::UpdateCurrentFrameInfo()
{
    // @note: Only I and P frames are supported. Each I frame is IDR.
    // 
    // Define frame type from order count number.
    if (m_currentFrame.displayOrderNumber == 0)
    {
        // First frame.
        m_h264PicData.FrameType = D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME;
    }
    else if (m_gopStructure.pH264GroupOfPictures->GOPLength == 0)
    {
        // Infinite GOP, only the first frame is intra.
        m_h264PicData.FrameType = D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME;
    }
    else
    {
        if (m_gopStructure.pH264GroupOfPictures->PPicturePeriod == 0)
        {
            // GOP with only IDR frames.
            m_h264PicData.FrameType = D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME;
        }
        else
        {
            UINT orderNumberInGop =
                (m_currentFrame.displayOrderNumber / m_gopStructure.pH264GroupOfPictures->GOPLength) == 0
                ? m_currentFrame.displayOrderNumber
                : (m_currentFrame.displayOrderNumber % m_gopStructure.pH264GroupOfPictures->GOPLength);

            if(orderNumberInGop == 0)
                m_h264PicData.FrameType = D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME;
            else
                m_h264PicData.FrameType = D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME;
        }
    }

    m_h264PicData.pic_parameter_set_id = m_bitstreamBuilder->get_active_pps_id();
    m_h264PicData.idr_pic_id = m_currentFrame.idrPicId;

    if (m_h264PicData.FrameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME)
    {
        m_h264PicData.PictureOrderCountNumber = 0;
        m_currentFrame.lastIdrNumber = m_currentFrame.displayOrderNumber;
    }
    else
    {
        m_h264PicData.PictureOrderCountNumber = m_currentFrame.displayOrderNumber - m_currentFrame.lastIdrNumber;
    }

    m_h264PicData.FrameDecodingOrderNumber = m_h264PicData.PictureOrderCountNumber;

    m_h264PicData.List0ReferenceFramesCount = 0;
    m_h264PicData.pList0ReferenceFrames = nullptr;
    m_h264PicData.List1ReferenceFramesCount = 0;
    m_h264PicData.pList1ReferenceFrames = nullptr;
    m_currentFrame.list0ReferenceFramesCount.clear();

    if (m_h264PicData.FrameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_B_FRAME)
    {
        throw std::runtime_error("B frames are not supported");
    }
    else if (m_h264PicData.FrameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME)
    {
        // Fill l0 list.

        // For now just use sequental last frames as reference.
        const auto refFrameCount = m_referenceFramesManager->GetReferenceFrameCount();
        for (auto i = 0; i < refFrameCount; ++i)
        {
            m_currentFrame.list0ReferenceFramesCount.push_back(i);
        }

        m_h264PicData.List0ReferenceFramesCount = m_currentFrame.list0ReferenceFramesCount.size();
        m_h264PicData.pList0ReferenceFrames = m_currentFrame.list0ReferenceFramesCount.data();
    }

    m_curPicParamsData.pH264PicData = &m_h264PicData;
    m_curPicParamsData.DataSize = sizeof(m_h264PicData);
}

void Encoder::UploadBitstreamHeaders()
{
    auto prefixGeneratedHeadersByteSize = m_bitstreamHeadersBuffer.size();
    void* outputBitrstreamData = nullptr;
    D3D12_RANGE readRange = { 0, prefixGeneratedHeadersByteSize };
    ThrowIfFailed(m_outputBitrstreamBuffer->Map(0, &readRange, &outputBitrstreamData));
    memcpy(outputBitrstreamData, m_bitstreamHeadersBuffer.data(), prefixGeneratedHeadersByteSize);
    m_outputBitrstreamBuffer->Unmap(0, nullptr);
}

void Encoder::CopyFrameToInputTexture(const FrameNV12& frame)
{
    // Starts copying the frame to input texture.

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint[2];
    UINT numRows[2];
    UINT64 rowBytes[2];
    UINT64 totalBytes;
    auto inputTextureDesc = m_encoderInputResource->GetDesc();
    m_device->GetCopyableFootprints(&inputTextureDesc, 0, 2, 0, footprint, numRows, rowBytes, &totalBytes);

    D3D12_SUBRESOURCE_DATA frameData[2] = {};
    frameData[0].pData = frame.pY;
    frameData[0].RowPitch = frame.linesizeY;
    frameData[0].SlicePitch = frame.linesizeY * frame.height;

    frameData[1].pData = frame.pUV;
    frameData[1].RowPitch = frame.linesizeUV;
    frameData[1].SlicePitch = frame.linesizeUV * frame.height / 2;

    auto requiredSize = UpdateSubresources(m_inputTextureCommandList.Get(),
        m_encoderInputResource.Get(),
        m_frameUploadBuffer.Get(),
        0, // FirstSubresource
        2, // NumSubresources
        totalBytes, // UINT64 RequiredSize
        footprint,
        numRows,
        rowBytes,
        frameData
    );
    assert(requiredSize == totalBytes);

    const CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_encoderInputResource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_COMMON);
    m_inputTextureCommandList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(m_inputTextureCommandList->Close());
    ID3D12CommandList* commandLists[] = { m_inputTextureCommandList.Get() };
    m_inputTextureCommandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);

    ThrowIfFailed(m_inputTextureCommandQueue->Signal(m_inputTextureFence.Get(), m_inputTextureFenceValue));
}

void Encoder::WaitForInputTexture()
{
    // Waits for copying to complete.

    ThrowIfFailed(m_inputTextureFence->SetEventOnCompletion(m_inputTextureFenceValue, nullptr));
    ThrowIfFailed(m_inputTextureCommandAllocator->Reset());
    ThrowIfFailed(m_inputTextureCommandList->Reset(m_inputTextureCommandAllocator.Get(), nullptr));
    ++m_inputTextureFenceValue;
}

void Encoder::CreateEncodeCommand()
{
    m_encodeCommandQueue.Reset();
    m_encodeCommandAllocator.Reset();
    m_encodeCommandList.Reset();
    m_encoderFence.Reset();

    D3D12_COMMAND_QUEUE_DESC commandQueueDesc = { D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE };
    ThrowIfFailed(m_device->CreateCommandQueue(
        &commandQueueDesc,
        IID_PPV_ARGS(&m_encodeCommandQueue)));

    ThrowIfFailed(m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE,
        IID_PPV_ARGS(&m_encodeCommandAllocator)));

    ThrowIfFailed(m_device->CreateCommandList(0,
        D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE,
        m_encodeCommandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&m_encodeCommandList)));

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_encoderFence)));
}

void Encoder::CreateInputCommand()
{
    m_inputTextureCommandQueue.Reset();
    m_inputTextureCommandAllocator.Reset();
    m_inputTextureCommandList.Reset();
    m_inputTextureFence.Reset();

    D3D12_COMMAND_QUEUE_DESC queueDesc = { D3D12_COMMAND_LIST_TYPE_COPY };
    ThrowIfFailed(m_device->CreateCommandQueue(
        &queueDesc,
        IID_PPV_ARGS(&m_inputTextureCommandQueue)));

    ThrowIfFailed(m_device->CreateCommandAllocator(
        queueDesc.Type,
        IID_PPV_ARGS(&m_inputTextureCommandAllocator)));

    ThrowIfFailed(m_device->CreateCommandList(0,
        queueDesc.Type,
        m_inputTextureCommandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&m_inputTextureCommandList)));

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_inputTextureFence)));
}

void Encoder::CreateOutputCommand()
{
    m_outputEncodedCommandQueue.Reset();
    m_outputEncodedCommandAllocator.Reset();
    m_outputEncodedCommandList.Reset();

    D3D12_COMMAND_QUEUE_DESC queueDesc = { D3D12_COMMAND_LIST_TYPE_COPY };
    ThrowIfFailed(m_device->CreateCommandQueue(
        &queueDesc,
        IID_PPV_ARGS(&m_outputEncodedCommandQueue)));

    ThrowIfFailed(m_device->CreateCommandAllocator(
        queueDesc.Type,
        IID_PPV_ARGS(&m_outputEncodedCommandAllocator)));

    ThrowIfFailed(m_device->CreateCommandList(0,
        queueDesc.Type,
        m_outputEncodedCommandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&m_outputEncodedCommandList)));
}

void Encoder::CreateOutputBufferResource()
{
    const CD3DX12_RESOURCE_DESC resolvedMetadataBufferDesc =
        CD3DX12_RESOURCE_DESC::Buffer(m_resolvedMetadataBufferSize);

    const D3D12_HEAP_PROPERTIES resolvedMetadataHeapProps = CD3DX12_HEAP_PROPERTIES(
        D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE, D3D12_MEMORY_POOL_L0);

    m_resolvedMetadataBuffer.Reset();
    ThrowIfFailed(m_device->CreateCommittedResource(
        &resolvedMetadataHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &resolvedMetadataBufferDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&m_resolvedMetadataBuffer)));

    assert(m_resolvedMetadataBuffer->GetDesc().Width == m_resolvedMetadataBufferSize);


    const CD3DX12_RESOURCE_DESC metadataBufferDesc =
        CD3DX12_RESOURCE_DESC::Buffer(m_resourceRequirements.MaxEncoderOutputMetadataBufferSize);

    const D3D12_HEAP_PROPERTIES metadataHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    m_metadataOutputBuffer.Reset();
    ThrowIfFailed(m_device->CreateCommittedResource(
        &metadataHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &metadataBufferDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&m_metadataOutputBuffer)));


    const D3D12_HEAP_PROPERTIES outputBitstreamHeapProps = CD3DX12_HEAP_PROPERTIES(
        D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE, D3D12_MEMORY_POOL_L0);

    // Roughly estimated size of the output bitstream buffer, that should be improved.
    const UINT64 outputBitstreamBufferSize = 4 * m_resolutionDesc.Width * m_resolutionDesc.Height;
    CD3DX12_RESOURCE_DESC outputBitstreamBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(outputBitstreamBufferSize);

    m_outputBitrstreamBuffer.Reset();
    ThrowIfFailed(m_device->CreateCommittedResource(
        &outputBitstreamHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &outputBitstreamBufferDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&m_outputBitrstreamBuffer)));
}

void Encoder::CreateInputResource()
{
    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Alignment = 0;
    textureDesc.Width = m_resolutionDesc.Width;
    textureDesc.Height = m_resolutionDesc.Height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_NV12;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    m_encoderInputResource.Reset();
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_encoderInputResource)
    ));


    m_frameUploadBuffer.Reset();

    UINT64 totalBytes;
    m_device->GetCopyableFootprints(&textureDesc, 0, 2, 0, nullptr, nullptr, nullptr, &totalBytes);

    D3D12_HEAP_PROPERTIES uploadHeapProperties = { D3D12_HEAP_TYPE_UPLOAD };
    CD3DX12_RESOURCE_DESC uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(totalBytes);
    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &uploadBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_frameUploadBuffer)));
}

std::shared_ptr<Encoder::EncodedFrame> Encoder::WaitForEncodedFrame()
{
    if (IsFlushed())
        return {};

    if (m_encoderFence->GetCompletedValue() < m_encoderFenceValue.load())
    {
        // Wait for the fence to be set from GPU.
        ThrowIfFailed(m_encoderFence->SetEventOnCompletion(m_encoderFenceValue, m_encodeCompletedEvent.Get()));
        DWORD result = WaitForSingleObject(m_encodeCompletedEvent.Get(), INFINITE);
        if (result != WAIT_OBJECT_0)
            throw std::runtime_error("WaitForSingleObject() failed: " + GetLastError());
    }

    const auto encodedFrame = ReadEncodedData();
    if (m_flushRequested)
    {
        m_isFlushed = true;
    }
    else
    {
        PrepareForNextFrame();
    }

    m_consumerToEncoderSemaphore.release();

    return encodedFrame;
}

}
