#pragma once
#include <string_view>

namespace DX12VideoEncoding {

void ThrowIfFailed(HRESULT hr);
void ThrowIfFalse(bool condition);

enum class LogLevel : int
{
    E_ERROR = 0,
    E_WARNING,
    E_INFO,
    E_DEBUG,
    E_NONE,
};

void LogMessage(LogLevel level, std::string_view message);

}
