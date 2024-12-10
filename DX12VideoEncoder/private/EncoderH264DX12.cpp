#include "pch.h"
#include "EncoderH264DX12.h"
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

void ThrowEncoderSupportError(D3D12_VIDEO_ENCODER_VALIDATION_FLAGS validationFlags)
{
    std::string error("Encoder support error:");
    if (D3D12_VIDEO_ENCODER_VALIDATION_FLAG_CODEC_NOT_SUPPORTED & validationFlags)
    {
        error.append("\nD3D12_VIDEO_ENCODER_VALIDATION_FLAG_CODEC_NOT_SUPPORTED");
    }
    if (D3D12_VIDEO_ENCODER_VALIDATION_FLAG_INPUT_FORMAT_NOT_SUPPORTED & validationFlags)
    {
        error.append("\nD3D12_VIDEO_ENCODER_VALIDATION_FLAG_INPUT_FORMAT_NOT_SUPPORTED");
    }
    if (D3D12_VIDEO_ENCODER_VALIDATION_FLAG_CODEC_CONFIGURATION_NOT_SUPPORTED & validationFlags)
    {
        error.append("\nD3D12_VIDEO_ENCODER_VALIDATION_FLAG_CODEC_CONFIGURATION_NOT_SUPPORTED");
    }
    if (D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RATE_CONTROL_MODE_NOT_SUPPORTED & validationFlags)
    {
        error.append("\nD3D12_VIDEO_ENCODER_VALIDATION_FLAG_RATE_CONTROL_MODE_NOT_SUPPORTED");
    }
    if (D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RATE_CONTROL_CONFIGURATION_NOT_SUPPORTED & validationFlags)
    {
        error.append("\nD3D12_VIDEO_ENCODER_VALIDATION_FLAG_RATE_CONTROL_CONFIGURATION_NOT_SUPPORTED");
    }
    if (D3D12_VIDEO_ENCODER_VALIDATION_FLAG_INTRA_REFRESH_MODE_NOT_SUPPORTED & validationFlags)
    {
        error.append("\nD3D12_VIDEO_ENCODER_VALIDATION_FLAG_INTRA_REFRESH_MODE_NOT_SUPPORTED");
    }
    if (D3D12_VIDEO_ENCODER_VALIDATION_FLAG_SUBREGION_LAYOUT_MODE_NOT_SUPPORTED & validationFlags)
    {
        error.append("\nD3D12_VIDEO_ENCODER_VALIDATION_FLAG_SUBREGION_LAYOUT_MODE_NOT_SUPPORTED");
    }
    if (D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RESOLUTION_NOT_SUPPORTED_IN_LIST & validationFlags)
    {
        error.append("\nD3D12_VIDEO_ENCODER_VALIDATION_FLAG_RESOLUTION_NOT_SUPPORTED_IN_LIST");
    }
    if (D3D12_VIDEO_ENCODER_VALIDATION_FLAG_GOP_STRUCTURE_NOT_SUPPORTED & validationFlags)
    {
        error.append("\nD3D12_VIDEO_ENCODER_VALIDATION_FLAG_GOP_STRUCTURE_NOT_SUPPORTED");
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

EncoderH264DX12::EncoderH264DX12(const ComPtr<ID3D12Device> &device,
    const EncoderConfiguration& config,
    DXGI_FORMAT inputFormat)
    : m_device(device)
    , m_maxReferenceFrameCount(config.maxReferenceFrameCount)
    , m_inputFormat(inputFormat)
    , m_bitstreamBuilder(std::make_unique<d3d12_video_bitstream_builder_h264>())
{
    ThrowIfFailed(m_device->QueryInterface(IID_PPV_ARGS(&m_videoDevice)));
    m_encodeCompletedEvent.Attach(CreateEvent(NULL, FALSE, FALSE, TEXT("encodeCompletedEvent")));
    Configure(config);
}

EncoderH264DX12::~EncoderH264DX12() = default;

void EncoderH264DX12::Configure(const EncoderConfiguration& config)
{
    m_resolutionDesc.Width = config.width;
    m_resolutionDesc.Height = config.height;
    m_frameCropping = H264FrameCroppingBox(m_resolutionDesc);
    m_targetFramerate.Numerator = config.fps.numerator;
    m_targetFramerate.Denominator = config.fps.denominator;

    m_h264GopStructure = ConfigureGOPStructure(config.keyFrameInterval, config.bFramesCount);
    m_gopStructure.pH264GroupOfPictures = &m_h264GopStructure;
    m_gopStructure.DataSize = sizeof(m_h264GopStructure);

    m_profileDesc.pH264Profile = &m_h264Profile;
    m_profileDesc.DataSize = sizeof(m_h264Profile);

    m_codecConfiguration.pH264Config = &m_codecH264Config;
    m_codecConfiguration.DataSize = sizeof(m_codecH264Config);

    m_rateControl.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
    m_rateControlCQP = D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP{
        .ConstantQP_FullIntracodedFrame = 30,
        .ConstantQP_InterPredictedFrame_PrevRefOnly = 30,
        .ConstantQP_InterPredictedFrame_BiDirectionalRef = 30,
    };
    m_rateControl.ConfigParams.DataSize = sizeof(m_rateControlCQP);
    m_rateControl.ConfigParams.pConfiguration_CQP = &m_rateControlCQP;
    m_rateControl.Flags = D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_NONE;
    m_rateControl.TargetFrameRate = m_targetFramerate;


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
    inputFormat.Format = m_inputFormat;
    ThrowIfFailed(m_videoDevice->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT,
        &inputFormat, sizeof(inputFormat)));

    ThrowIfFalse(inputFormat.IsSupported == TRUE);


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


    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC resolutionDescList[] = { m_resolutionDesc };

    D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT encoderSupport = {};
    encoderSupport.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
    encoderSupport.InputFormat = inputFormat.Format;
    encoderSupport.CodecConfiguration = m_codecConfiguration;
    encoderSupport.CodecGopSequence = m_gopStructure;
    encoderSupport.RateControl = m_rateControl;
    encoderSupport.IntraRefresh = D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE;
    encoderSupport.SubregionFrameEncoding = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
    encoderSupport.ResolutionsListCount = _countof(resolutionDescList);;
    encoderSupport.pResolutionList = resolutionDescList;
    encoderSupport.MaxReferenceFramesInDPB = m_maxReferenceFrameCount;

    D3D12_VIDEO_ENCODER_LEVELS_H264 receivedLevel = {};
    encoderSupport.SuggestedLevel.DataSize = sizeof(receivedLevel);
    encoderSupport.SuggestedLevel.pH264LevelSetting = &receivedLevel;

    D3D12_VIDEO_ENCODER_PROFILE_H264 receivedProfile = {};
    encoderSupport.SuggestedProfile.DataSize = sizeof(receivedProfile);
    encoderSupport.SuggestedProfile.pH264Profile = &receivedProfile;

    std::vector<D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS> resolutionLimits{ encoderSupport.ResolutionsListCount };
    encoderSupport.pResolutionDependentSupport = resolutionLimits.data();

    ThrowIfFailed(m_videoDevice->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_SUPPORT,
        &encoderSupport, sizeof(encoderSupport)));

    if ((encoderSupport.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK) == 0)
    {
        ThrowEncoderSupportError(encoderSupport.ValidationFlags);
    }


    D3D12_VIDEO_ENCODER_DESC encoderDesc = {};
    encoderDesc.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_H264;
    encoderDesc.EncodeProfile = profileLevel.Profile;
    encoderDesc.InputFormat = m_inputFormat;
    encoderDesc.CodecConfiguration = m_codecConfiguration;

    ThrowIfFailed(m_videoDevice->CreateVideoEncoder(&encoderDesc, IID_PPV_ARGS(&m_videoEncoder)));


    D3D12_VIDEO_ENCODER_HEAP_DESC encoderHeapDesc = {};
    encoderHeapDesc.Flags = D3D12_VIDEO_ENCODER_HEAP_FLAG_NONE;
    encoderHeapDesc.EncodeCodec = encoderDesc.EncodeCodec;
    encoderHeapDesc.EncodeProfile = encoderDesc.EncodeProfile;
    encoderHeapDesc.EncodeLevel.pHEVCLevelSetting = nullptr;
    encoderHeapDesc.EncodeLevel = maxLevel;
    encoderHeapDesc.ResolutionsListCount = _countof(resolutionDescList);
    encoderHeapDesc.pResolutionList = resolutionDescList;

    ThrowIfFailed(m_videoDevice->CreateVideoEncoderHeap(&encoderHeapDesc, IID_PPV_ARGS(&m_videoEncoderHeap)));

    bool gopHasInterFrames =
        (m_h264GopStructure.PPicturePeriod > 0) &&
        ((m_h264GopStructure.GOPLength == 0) || (m_h264GopStructure.PPicturePeriod < m_h264GopStructure.GOPLength));

    m_referenceFramesManager = std::make_unique<ReferenceFramesManager>(
        m_device, m_resolutionDesc, m_inputFormat, m_maxReferenceFrameCount, gopHasInterFrames);

    CreateOutputBufferResource();
    CreateEncodeCommand();
    CreateOutputCommand();
}

D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264
EncoderH264DX12::ConfigureGOPStructure(UINT gopLengthInFrames, UINT bFramesCount)
{
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264 h264GopStructure = {};
    h264GopStructure.GOPLength = gopLengthInFrames;
    h264GopStructure.PPicturePeriod = gopLengthInFrames == 1 /*Key frames only*/ ? 0 : bFramesCount + 1;
    h264GopStructure.pic_order_cnt_type = 0;

    // @todo: This is temp fix for infinite GOP.
    auto GOPLength = h264GopStructure.GOPLength == 0 ? (1 << 15) : h264GopStructure.GOPLength;

    const uint32_t max_pic_order_cnt_lsb = 2 * GOPLength;
    const uint32_t max_max_frame_num = GOPLength;
    double log2_max_frame_num_minus4 = (std::max)(0.0, std::ceil(std::log2(max_max_frame_num)) - 4);
    double log2_max_pic_order_cnt_lsb_minus4 = (std::max)(0.0, std::ceil(std::log2(max_pic_order_cnt_lsb)) - 4);
    assert(log2_max_frame_num_minus4 < UCHAR_MAX);
    assert(log2_max_pic_order_cnt_lsb_minus4 < UCHAR_MAX);
    h264GopStructure.log2_max_frame_num_minus4 = static_cast<UCHAR>(log2_max_frame_num_minus4);
    h264GopStructure.log2_max_pic_order_cnt_lsb_minus4 = static_cast<UCHAR>(log2_max_pic_order_cnt_lsb_minus4);
    
    assert(h264GopStructure.log2_max_pic_order_cnt_lsb_minus4 >=0
        && h264GopStructure.log2_max_pic_order_cnt_lsb_minus4 <= 12);

    return h264GopStructure;
}

void EncoderH264DX12::SendFrame(const InputFrame& inputFrame, const InputFrameResources& inputFrameResources)
{
    // Resolution change is not supported for now.
    ThrowIfFalse((inputFrameResources.GetHeight() == m_resolutionDesc.Height)
        && (inputFrameResources.GetWidth() == m_resolutionDesc.Width));

    m_currentFrame = inputFrame;

    // Wait on GPU for completion of copying the frame to input texture.
    inputFrameResources.WaitForUploadingGPU(m_encodeCommandQueue.Get());

    UpdateCurrentFrameInfo(m_currentFrame);
    bool isCurrentFrameUsedAsReference = m_currentFrame.useAsReference;
    m_referenceFramesManager->PrepareForEncodingFrame(m_curPicParamsData, isCurrentFrameUsedAsReference);
    m_referenceFramesManager->GetPictureControlCodecData(m_curPicParamsData);


    D3D12_RESOURCE_BARRIER currentFrameStateTransitions[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(inputFrameResources.GetInputTextureRawPtr(),
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
            .RateControl = m_rateControl,
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

        .pInputFrame = inputFrameResources.GetInputTextureRawPtr(),
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
        CD3DX12_RESOURCE_BARRIER::Transition(inputFrameResources.GetInputTextureRawPtr(),
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
        .EncoderInputFormat = m_inputFormat,
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
}

D3D12_VIDEO_ENCODER_OUTPUT_METADATA EncoderH264DX12::ReadResolvedMetadata()
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

void EncoderH264DX12::ReadEncodedData(std::vector<uint8_t>& encodedData)
{
    const D3D12_VIDEO_ENCODER_OUTPUT_METADATA metadata = ReadResolvedMetadata();
    auto encodedFrameSize = metadata.EncodedBitstreamWrittenBytesCount + m_bitstreamHeadersBuffer.size();
    assert(encodedFrameSize);

    encodedData.resize(encodedFrameSize);

    void* encodedFrameData = nullptr;
    D3D12_RANGE readRange = { 0, encodedFrameSize };
    ThrowIfFailed(m_outputBitrstreamBuffer->Map(0, &readRange, &encodedFrameData));
    memcpy(encodedData.data(), encodedFrameData, encodedFrameSize);
    m_outputBitrstreamBuffer->Unmap(0, nullptr);
}

uint32_t EncoderH264DX12::BuildCodecHeadersH264()
{
    size_t writtenSPSBytesCount = 0;
    bool isFirstFrame = (m_encoderFenceValue == 1);
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
            m_inputFormat,
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

void EncoderH264DX12::PrepareForNextFrame()
{
    ThrowIfFailed(m_encodeCommandAllocator->Reset());
    ThrowIfFailed(m_encodeCommandList->Reset(m_encodeCommandAllocator.Get()));
    ++m_encoderFenceValue;

    m_referenceFramesManager->UpdateReferenceFrames();
}

void EncoderH264DX12::UpdateCurrentFrameInfo(InputFrame& inputFrame)
{
    m_h264PicData.pic_parameter_set_id = m_bitstreamBuilder->get_active_pps_id();
    m_h264PicData.FrameType = inputFrame.frameType;
    m_h264PicData.idr_pic_id = inputFrame.idrPicId;

    m_h264PicData.PictureOrderCountNumber = inputFrame.pictureOrderCountNumber;
    m_h264PicData.FrameDecodingOrderNumber = inputFrame.decodingOrderNumber;

    m_h264PicData.List0ReferenceFramesCount = inputFrame.l0List.empty() ? 0 : inputFrame.l0List.size();
    m_h264PicData.pList0ReferenceFrames = inputFrame.l0List.data();
    m_h264PicData.List1ReferenceFramesCount = inputFrame.l1List.empty() ? 0 : inputFrame.l1List.size();
    m_h264PicData.pList1ReferenceFrames = inputFrame.l1List.data();

    m_curPicParamsData.pH264PicData = &m_h264PicData;
    m_curPicParamsData.DataSize = sizeof(m_h264PicData);
}

void EncoderH264DX12::UploadBitstreamHeaders()
{
    auto prefixGeneratedHeadersByteSize = m_bitstreamHeadersBuffer.size();
    void* outputBitrstreamData = nullptr;
    D3D12_RANGE readRange = { 0, prefixGeneratedHeadersByteSize };
    ThrowIfFailed(m_outputBitrstreamBuffer->Map(0, &readRange, &outputBitrstreamData));
    memcpy(outputBitrstreamData, m_bitstreamHeadersBuffer.data(), prefixGeneratedHeadersByteSize);
    m_outputBitrstreamBuffer->Unmap(0, nullptr);
}

void EncoderH264DX12::CreateEncodeCommand()
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

void EncoderH264DX12::CreateOutputCommand()
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

void EncoderH264DX12::CreateOutputBufferResource()
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

bool EncoderH264DX12::WaitForEncodedData(Microsoft::WRL::Wrappers::Event& termintateEvent,
    std::vector<uint8_t>& encodedData)
{
    if (m_encoderFence->GetCompletedValue() < m_encoderFenceValue)
    {
        // Wait for the fence to be set from GPU.
        ThrowIfFailed(m_encoderFence->SetEventOnCompletion(m_encoderFenceValue, m_encodeCompletedEvent.Get()));
        HANDLE events[] = { m_encodeCompletedEvent.Get(), termintateEvent.Get() };
        DWORD result = WaitForMultipleObjects(_countof(events), events, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0)
        {
        }
        else if (result == WAIT_OBJECT_0 + 1)
        {
            // Terminated
            return false;
        }
        else
            throw std::runtime_error("WaitForSingleObject() failed: " + GetLastError());
    }

    ReadEncodedData(encodedData);
    PrepareForNextFrame();

    return true;
}


}
