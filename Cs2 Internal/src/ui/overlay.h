#pragma once
#include "platform/windows.h"

namespace overlay
{
    BOOL Start();
    BOOL Stop();
    DWORD State();
} // namespace overlay
