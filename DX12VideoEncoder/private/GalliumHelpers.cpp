#include "pch.h"
#include "GalliumHelpers.h"

void debug_printf(const char* format, ...)
{
    //#if 0
#ifdef _DEBUG
    va_list ap;
    va_start(ap, format);
    vprintf(format, ap);
    va_end(ap);
#else
    (void)format; /* silence warning */
#endif
}
