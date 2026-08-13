#pragma once

// The single place where <windows.h> enters the build. Including it directly
// anywhere else risks pulling in the min/max macros or the full winsock stack.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
