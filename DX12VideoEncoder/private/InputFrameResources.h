#pragma once
#include "EncoderAPI.h"

namespace DX12VideoEncoding
{

using Microsoft::WRL::ComPtr;

class InputFrameResources
{
public:
    InputFrameResources(const ComPtr<ID3D12Device>& device,
        DXGI_FORMAT dxgiFormat, UINT width, UINT height);

    InputFrameResources(const InputFrameResources&) = delete;

    UINT GetWidth() const { return m_rawFrameData->GetWidth(); }
    UINT GetHeight() const { return m_rawFrameData->GetHeight(); }

    void SetFrameData(const RawFrameData& rawFrameData);
    void UploadTexture();
    ID3D12Resource* GetInputTextureRawPtr() const { return m_inputTexture.Get(); }
    void WaitForUploadingCPU();
    void WaitForUploadingGPU(ID3D12CommandQueue* commandQueue) const;
    void ResetCommands();

private:

    void CreateCommandResources();
    void CreateTextureResources(UINT width, UINT height);

private:
    ComPtr<ID3D12Device> m_device;
    const DXGI_FORMAT m_dxgiFormat;

    RawFrameData m_rawFrameData;
    ComPtr<ID3D12Resource> m_frameUploadBuffer;
    ComPtr<ID3D12Resource> m_inputTexture;

    ComPtr<ID3D12CommandQueue> m_inputTextureCommandQueue;
    ComPtr<ID3D12CommandAllocator> m_inputTextureCommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_inputTextureCommandList;
    ComPtr<ID3D12Fence> m_inputTextureFence;
    UINT64 m_inputTextureFenceValue = 1;
};

}
