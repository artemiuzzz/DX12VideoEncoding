#pragma once

namespace DX12VideoEncoding {

struct FrameNV12
{
    uint8_t* pY{};
    uint8_t* pUV{};
    size_t linesizeY{};
    size_t linesizeUV{};
    int width{};
    int height{};
};

}
