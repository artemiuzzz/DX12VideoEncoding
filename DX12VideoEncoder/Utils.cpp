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

}

void debug_printf(const char* format, ...)
{
#ifdef _DEBUG
    va_list ap;
    va_start(ap, format);
    vprintf(format, ap);
    va_end(ap);
#else
    (void)format; /* silence warning */
#endif
}
