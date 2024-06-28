// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//
#pragma once

#include "targetver.h"

#include <cstdio>
#include <tchar.h>

// TODO: reference additional headers your program requires here

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <commctrl.h>

#include <cstdint>

#include <fcntl.h>
#include <io.h>
#include <vector>
#include <string>

enum class VSTHostCommand : uint32_t
{
    Exit = 0,
    GetChunk,
    SetChunk,
    HasEditor,
    DisplayEditorModal,
    SetSampleRate,
    Reset,
    SendMIDIEvent,
    SendSysexEvent,
    RenderSamples,
    SendMIDIEventWithTimestamp,
    SendSysexEventWithTimestamp,
};

enum
{
    BUFFER_SIZE = 4096
};

template <typename T>
static void append_be(std::vector<uint8_t>& out, const T& value)
{
    union
    {
        T original;
        uint8_t raw[sizeof(T)];
    } carriage;

    carriage.original = value;

    for (unsigned i = 0; i < sizeof(T); ++i)
    {
        out.push_back(carriage.raw[sizeof(T) - 1 - i]);
    }
}

template <typename T>
static void retrieve_be(T& out, const uint8_t*& in, unsigned& size)
{
    if (size < sizeof(T))
        return;

    size -= sizeof(T);

    union
    {
        T original;
        uint8_t raw[sizeof(T)];
    } carriage;

    carriage.raw[0] = 0;

    for (unsigned i = 0; i < sizeof(T); ++i)
    {
        carriage.raw[sizeof(T) - 1 - i] = *in++;
    }

    out = carriage.original;
}
