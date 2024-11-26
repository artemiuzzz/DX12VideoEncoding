#include "framework.h"
#include "Encoder.h"
#include "Utils.h"
#include "StreamReader.h"
#include "StreamWriter.h"

#include <d3d12.h>
#include <dxgi1_6.h>

#include <iostream>
#include <stdexcept>

using namespace DX12VideoEncoding;

ComPtr<ID3D12Device> CreateDeviceForEncoding()
{
    UINT dxgiFactoryFlags = 0;

#if defined(_DEBUG)
    // Enable the debug layer (requires the Graphics Tools "optional feature").
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();

            // Enable additional debug layers.
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }

    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> pDredSettings;
    ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&pDredSettings)));

    // Turn on AutoBreadcrumbs and Page Fault reporting
    pDredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    pDredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;

    ComPtr<IDXGIFactory6> factory6;
    ThrowIfFailed(factory->QueryInterface(IID_PPV_ARGS(&factory6)));

    UINT adapterIndex = 0;
    ThrowIfFailed(factory6->EnumAdapterByGpuPreference(
        adapterIndex,
        DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
        IID_PPV_ARGS(&adapter)));

    DXGI_ADAPTER_DESC1 desc;
    adapter->GetDesc1(&desc);

    ComPtr<ID3D12Device> device;
    ThrowIfFailed(D3D12CreateDevice(
        adapter.Get(),
        D3D_FEATURE_LEVEL_12_2,
        IID_PPV_ARGS(&device)
    ));

    return device;
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cout << "Set input and output file paths" << std::endl;
        return EXIT_FAILURE;
    }

    try
    {
        auto dx12Device = CreateDeviceForEncoding();

        const auto inputFilename = argv[1];
        const auto outputFilename = argv[2];

        StreamReader streamReader;
        auto streamInfo = streamReader.OpenInputFile(inputFilename);

        StreamWriter streamWriter;
        streamWriter.OpenOutputFile(outputFilename, streamInfo.width, streamInfo.height, streamInfo.frameRate);

        Encoder encoder(dx12Device, Encoder::Configuration{
            .width = static_cast<UINT>(streamInfo.width),
            .height = static_cast<UINT>(streamInfo.height),
            .fps = DXGI_RATIONAL {
                .Numerator = static_cast<UINT>(streamInfo.frameRate.num),
                .Denominator = static_cast<UINT>(streamInfo.frameRate.den)
            },
            .maxReferenceFrameCount = 2,
            .gopLengthInFrames = 2,
            .usePframes = true,
            });

        auto writeThread = std::thread([&streamWriter, &encoder, frameRate = streamInfo.frameRate]
        {
            const double frameDurationMs = 1000.0 * double(frameRate.den) / frameRate.num;
            while (true)
            {
                auto encodedFrame = encoder.WaitForEncodedFrame();
                if (!encodedFrame)
                {
                    streamWriter.Finalize();
                    break;
                }

                streamWriter.WriteVideoPacket(
                    encodedFrame->encodedData.data(),
                    encodedFrame->encodedData.size(),
                    encodedFrame->isKeyFrame,
                    static_cast<int64_t>(encodedFrame->displayOrderNumber * frameDurationMs),
                    static_cast<int64_t>(frameDurationMs));
            }
        });

        streamReader.ReadAllFrames([&encoder](const AVFrame& frame)
        {
            DX12VideoEncoding::FrameNV12 frameNV12
            {
                .pY = frame.data[0],
                .pUV = frame.data[1],
                .linesizeY = static_cast<size_t>(frame.linesize[0]),
                .linesizeUV = static_cast<size_t>(frame.linesize[1]),
                .width = frame.width,
                .height = frame.height,
            };

            encoder.SendFrame(frameNV12);
        });

        encoder.Flush();
        writeThread.join();

        return EXIT_SUCCESS;
    }
    catch (const std::exception& ex)
    {
        std::cout << "Error occurred: " << ex.what() << std::endl;
    }
    return EXIT_FAILURE;
}
