// Minimal unistd.h stub for MSVC — provides usleep() used by kdtree_parallel.cpp
#pragma once
#ifndef COMPAT_UNISTD_H
#define COMPAT_UNISTD_H

#include <windows.h>

static inline void usleep(unsigned long usec)
{
    // Sleep() takes milliseconds; round up to avoid busy-waiting.
    DWORD ms = (usec + 999) / 1000;
    if (ms == 0) ms = 1;
    Sleep(ms);
}

#endif
