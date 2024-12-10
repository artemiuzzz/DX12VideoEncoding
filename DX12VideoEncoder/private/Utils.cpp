#include "pch.h"
#include "Utils.h"


namespace DX12VideoEncoding {

void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr))
    {
        char buffer[256]{};
        snprintf(buffer, sizeof(buffer), "Failed with HRESULT code: %08X", static_cast<unsigned int>(hr));
        throw std::runtime_error(buffer);
    }
}

void ThrowIfFalse(bool condition)
{
    if (!condition)
    {
        throw std::runtime_error("Assertion failed");
    }
}

namespace {
std::mutex g_logMutex;
LogLevel g_currentLogLevel = LogLevel::E_DEBUG;
}

void LogMessage(LogLevel level, std::string_view message)
{
    if (level > g_currentLogLevel)
    {
        return;
    }
    std::lock_guard guard(g_logMutex);
    std::cout << message << std::endl;
}

} // namespace DX12VideoEncoding
