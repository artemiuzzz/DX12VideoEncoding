#pragma once

#define WIN32_LEAN_AND_MEAN

#include <mutex>
#include <iostream>
#include <exception>
#include <cassert>
#include <cmath>
#include <algorithm>
#include <cstdarg>
#include <vector>
#include <set>
#include <string>
#include <memory>
#include <semaphore>
#include <string_view>

#include <Windows.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include "directx/d3dx12.h"
#include <d3d12.h>
#include <d3d12video.h>
#include <dxgi.h>


void debug_printf(const char* format, ...);
