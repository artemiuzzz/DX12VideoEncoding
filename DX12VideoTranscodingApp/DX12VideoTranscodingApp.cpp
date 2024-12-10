#include "EncoderAPI.h"
#include "Utils.h"

#include "StreamReader.h"
#include "StreamWriter.h"
#include "FfmpegFrameData.h"

#include <d3d12.h>
#include <dxgi1_6.h>

#include <iostream>
#include <stdexcept>
#include <semaphore>
#include <thread>
#include <cassert>

// Uncomment to enable WinPixGpuCapturer.dll module loading for profiling.
//#define WIN_PIX_GPU_CAPTURER

#ifdef WIN_PIX_GPU_CAPTURER
#include "PIX.h"
std::wstring GetLatestWinPixGpuCapturerPath();
#endif

using namespace DX12VideoEncoding;
using Microsoft::WRL::ComPtr;


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

    std::thread decodingThread;
    std::atomic_bool terminated = false;
    std::binary_semaphore readingSemaphore(1), writingSemaphore(0);

    try
    {
        auto dx12Device = CreateDeviceForEncoding();

#ifdef WIN_PIX_GPU_CAPTURER
        // Check to see if a copy of WinPixGpuCapturer.dll has already been injected into the application.
        // This may happen if the application is launched through the PIX UI. 
        if (GetModuleHandle(L"WinPixGpuCapturer.dll") == 0)
        {
            LoadLibrary(GetLatestWinPixGpuCapturerPath().c_str());
        }
#endif
        const auto inputFilename = argv[1];
        const auto outputFilename = argv[2];

        StreamReader streamReader;
        auto streamInfo = streamReader.OpenInputFile(inputFilename);

        StreamWriter streamWriter;
        streamWriter.OpenOutputFile(outputFilename, streamInfo.width, streamInfo.height, streamInfo.frameRate);
        const double frameDurationMs = 1000.0 * double(streamInfo.frameRate.den) / streamInfo.frameRate.num;

        // I B B P I
        uint32_t keyFrameInterval = 4;
        uint32_t bFramesCount = 2;

        // I B B P B B P I
        //uint32_t keyFrameInterval = 7;
        //uint32_t bFramesCount = 2;

        // I B B P B B P B I
        //uint32_t keyFrameInterval = 8;
        //uint32_t bFramesCount = 2;

        // I I I I...
        //uint32_t keyFrameInterval = 1;
        //uint32_t bFramesCount = 0;

        // I P I P I P I
        //uint32_t keyFrameInterval = 2;
        //uint32_t bFramesCount = 0;

        // I B P I
        //uint32_t keyFrameInterval = 3;
        //uint32_t bFramesCount = 1;

        // I P P I
        //uint32_t keyFrameInterval = 3;
        //uint32_t bFramesCount = 0;


        // Long GOPs:
        
        // I P P P ... I
        //uint32_t keyFrameInterval = 30;
        //uint32_t bFramesCount = 0;

        // I P P P ... I
        //uint32_t keyFrameInterval = 30;
        //uint32_t bFramesCount = 4;


        // Infinite GOPs:
        
        // I B B P B B P B B ...
        //uint32_t keyFrameInterval = 0;
        //uint32_t bFramesCount = 2;

        // I P P P P P P P P ...
        //uint32_t keyFrameInterval = 0;
        //uint32_t bFramesCount = 0;

        auto encoder = CreateH264Encoder(dx12Device,
            EncoderConfiguration{
            .width = static_cast<uint32_t>(streamInfo.width),
            .height = static_cast<uint32_t>(streamInfo.height),
            .fps = {
                .numerator = static_cast<uint32_t>(streamInfo.frameRate.num),
                .denominator = static_cast<uint32_t>(streamInfo.frameRate.den)
            },
            .keyFrameInterval = keyFrameInterval,
            .bFramesCount = bFramesCount,
            .maxReferenceFrameCount = 2,
            });

        // This is a fix for FFmpeg error "pts < dts" for B-frames.
        const uint64_t ptsOffset = bFramesCount;


        EncodedFrame encodedFrame;
        bool finishedReading = false;
        std::shared_ptr<FfmpegFrameData> frameData;
        std::exception_ptr exceptionPtr = nullptr;

        decodingThread = std::thread([&]
        {
            try
            {
                streamReader.ReadAllFrames([&](const AVFrame& frame)
                {
                    readingSemaphore.acquire();
                    assert(!frameData);
                    frameData = FfmpegFrameData::Create(frame);
                    writingSemaphore.release();
                });

                readingSemaphore.acquire();
                finishedReading = true;
                writingSemaphore.release();
            }
            catch (...)
            {
                terminated = true;
                encoder->Terminate();
                writingSemaphore.release();
                exceptionPtr = std::current_exception();
            }
        });


        while (true)
        {
            writingSemaphore.acquire();

            if (terminated)
                break;

            if (finishedReading)
            {
                encoder->Flush();
            }
            else
            {
                assert(frameData);
                encoder->PushFrame(frameData);
                frameData.reset();
            }

            while (!terminated && encoder->StartEncodingPushedFrame())
            {
                if (!encoder->WaitForEncodedFrame(encodedFrame))
                    break;

                streamWriter.WriteVideoPacket(
                    encodedFrame.encodedData.data(),
                    encodedFrame.encodedData.size(),
                    encodedFrame.isKeyFrame,
                    static_cast<int64_t>((encodedFrame.pictureOrderCountNumber + ptsOffset) * frameDurationMs),
                    static_cast<int64_t>(encodedFrame.decodingOrderNumber * frameDurationMs),
                    static_cast<int64_t>(frameDurationMs));
            
                LogMessage(LogLevel::E_INFO, "Packet written: "
                    + std::string(encodedFrame.isKeyFrame ? "key" : "non-key")
                    + ", pic order " + std::to_string(encodedFrame.pictureOrderCountNumber)
                    + ", dec order " + std::to_string(encodedFrame.decodingOrderNumber));
            }
            readingSemaphore.release();

            if (finishedReading || terminated)
                break;
        }

        decodingThread.join();
        if (exceptionPtr)
            std::rethrow_exception(exceptionPtr);

        streamWriter.Finalize();
        return EXIT_SUCCESS;
    }
    catch (const std::exception& ex)
    {
        LogMessage(LogLevel::E_ERROR, ex.what());
    }
    return EXIT_FAILURE;
}
