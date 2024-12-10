# DX12VideoEncoding

HW video encoding using DirectX 12, inspired by https://devblogs.microsoft.com/directx/announcing-new-directx-12-feature-video-encoding/

## Overview

This solution demonstrates how to use DirectX 12 for video encoding. It provides a simple video encoding application that reads video frames from an input file, encodes them using DirectX 12, and writes the encoded frames to an output file. Reading and writing video frames are performed using FFmpeg in **DX12VideoTranscodingApp** command line executable, while DirectX 12 encoding is done in **DX12VideoEncoder** project.

DirectX 12 is a low-level graphics API and user has to manage all the resources and synchronization. For example, one has to take care about managing buffers for reference frames and reconstructed picture. As input frame this project supports NV12 format. The application uses FFmpeg to decode the input video file and convert frames to NV12 format. NV12 frames are then copied to a texture and used as input for the DirectX 12 encoder. The encoder produces H264 encoded data, and for building and writing SPS/PPS data some Gallium3D's source code is used. Then the encoded data is written to the output file by FFmpeg.

My aim of working on this project is to learn DirectX 12 encoding API, so the project is in state of development and may not be suitable for production use, there are no such things as error handling, logging, etc.

## Requirements
- **Windows 11** or later
- Visual Studio 2022 (at least it was developed in this version)

## Limitations

- H264 encoding is supported. Although input file and video codec can be in any format supported by FFmpeg.
- GOP structure can contain P- and B-frames, but the GOP should be closed or infinite.
- Maximum 2 reference frames can be used: 1 past and 1 future for B-frames and 1 past for P-frames.
- No dynamic changes to encoding settings like framerate, resolution etc. are supported.
- Tested on Windows 11 with GeForce RTX 2060 GPU mobile.

## Usage

To use the DX12VideoTranscodingApp, follow these steps:

1. **Requirements**. Install DirectX SDK, refer to https://devblogs.microsoft.com/directx/announcing-new-directx-12-feature-video-encoding/
2. **Build the Solution**: Open the solution in Visual Studio 2022 and build the project. FFmpeg should be downloaded as NuGet package during the build process.
3. **Run the Application**: Execute the application with the input and output file paths as command-line arguments:
```cmd
.\DX12VideoTranscodingApp.exe input.mp4 output.mp4
```
