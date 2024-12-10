#include "pch.h"
#include "InputFrameResources.h"
#include "Utils.h"

namespace DX12VideoEncoding
{

InputFrameResources::InputFrameResources(const ComPtr<ID3D12Device>& device,
    DXGI_FORMAT dxgiFormat, UINT width, UINT height)
    : m_device(device)
    , m_dxgiFormat(dxgiFormat)
{
    CreateCommandResources();
    CreateTextureResources(width, height);
}

void InputFrameResources::SetFrameData(const RawFrameData& rawFrameData)
{
    // Ensure previous frame is uploaded.
    assert(m_rawFrameData == nullptr);
    m_rawFrameData = rawFrameData;
}

void InputFrameResources::UploadTexture()
{
    // Starts copying the frame to input texture.

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint[2];
    UINT numRows[2];
    UINT64 rowBytes[2];
    UINT64 totalBytes;
    auto inputTextureDesc = m_inputTexture->GetDesc();
    m_device->GetCopyableFootprints(&inputTextureDesc, 0, 2, 0, footprint, numRows, rowBytes, &totalBytes);

    D3D12_SUBRESOURCE_DATA frameData[2] = {};
    frameData[0].pData = m_rawFrameData->GetY();
    frameData[0].RowPitch = m_rawFrameData->GetLinesizeY();
    frameData[0].SlicePitch = m_rawFrameData->GetLinesizeY() * m_rawFrameData->GetHeight();

    frameData[1].pData = m_rawFrameData->GetUV();
    frameData[1].RowPitch = m_rawFrameData->GetLinesizeUV();
    frameData[1].SlicePitch = m_rawFrameData->GetLinesizeUV() * m_rawFrameData->GetHeight() / 2;

    auto requiredSize = UpdateSubresources(m_inputTextureCommandList.Get(),
        m_inputTexture.Get(),
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
        m_inputTexture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_COMMON);
    m_inputTextureCommandList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(m_inputTextureCommandList->Close());
    ID3D12CommandList* commandLists[] = { m_inputTextureCommandList.Get() };
    m_inputTextureCommandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);

    ThrowIfFailed(m_inputTextureCommandQueue->Signal(m_inputTextureFence.Get(), m_inputTextureFenceValue));
}

void InputFrameResources::WaitForUploadingCPU()
{
    if (m_inputTextureFence->GetCompletedValue() < m_inputTextureFenceValue)
    {
        ThrowIfFailed(m_inputTextureFence->SetEventOnCompletion(m_inputTextureFenceValue, nullptr));
    }
}

void InputFrameResources::WaitForUploadingGPU(ID3D12CommandQueue* commandQueue) const
{
    ThrowIfFailed(commandQueue->Wait(m_inputTextureFence.Get(), m_inputTextureFenceValue));
}

void InputFrameResources::ResetCommands()
{
    ThrowIfFailed(m_inputTextureCommandAllocator->Reset());
    ThrowIfFailed(m_inputTextureCommandList->Reset(m_inputTextureCommandAllocator.Get(), nullptr));
    ++m_inputTextureFenceValue;

    m_rawFrameData.reset();
}

void InputFrameResources::CreateCommandResources()
{
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

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&m_inputTextureFence)));
}


void InputFrameResources::CreateTextureResources(UINT width, UINT height)
{
    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Alignment = 0;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = m_dxgiFormat;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    m_inputTexture.Reset();
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_inputTexture)
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

} // namespace DX12VideoEncoding
