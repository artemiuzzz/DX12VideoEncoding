#pragma once
#include <string>

extern "C"
{
#include <libavutil/error.h>
}

inline std::string av_error_to_string(int errnum)
{
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, sizeof(errbuf));
    return std::string(errbuf);
}
