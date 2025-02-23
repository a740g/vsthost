#include "aeffect.h"
#include "aeffectx.h"
#include "stdafx.h"
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>

typedef AEffect* (VSTCALLBACK* main_func)(audioMasterCallback audioMaster);

// #define LOG_EXCHANGE

#ifdef LOG_EXCHANGE
unsigned exchange_count = 0;
#endif

bool need_idle = false;
bool idle_started = false;

static std::string dll_dir;

static HANDLE null_file = nullptr;
static HANDLE pipe_in = nullptr;
static HANDLE pipe_out = nullptr;

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

#pragma pack(push, 8)
#pragma warning(disable : 4820) // x bytes padding added after data member
struct myVstEvent
{
    struct myVstEvent* next;

    unsigned port;

    union
    {
        VstMidiEvent midiEvent;
        VstMidiSysexEvent sysexEvent;
    } ev;
} *_EventHead = nullptr, * evTail = nullptr;
#pragma warning(default : 4820) // x bytes padding added after data member
#pragma pack(pop)

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

void freeChain()
{
    myVstEvent* ev = _EventHead;

    while (ev)
    {
        myVstEvent* next = ev->next;

        if (ev->port && ev->ev.sysexEvent.type == kVstSysExType)
            free(ev->ev.sysexEvent.sysexDump);

        free(ev);

        ev = next;
    }

    _EventHead = nullptr;
    evTail = nullptr;
}

void put_bytes(const void* out, uint32_t size)
{
    DWORD BytesWritten;

    WriteFile(pipe_out, out, size, &BytesWritten, NULL);

#ifdef LOG_EXCHANGE
    TCHAR logfile[MAX_PATH];
    _stprintf_s(logfile, _T("C:\\temp\\log\\bytes_%08u.out"), exchange_count++);
    FILE* f = _tfopen(logfile, _T("wb"));
    fwrite(out, 1, size, f);
    fclose(f);
#endif
}

void put_code(uint32_t code)
{
    put_bytes(&code, sizeof(code));
}

void get_bytes(void* in, uint32_t size)
{
    DWORD BytesRead;

    if (!ReadFile(pipe_in, in, size, &BytesRead, NULL) || (BytesRead < size))
    {
        memset(in, 0, size);

#ifdef LOG_EXCHANGE
        TCHAR logfile[MAX_PATH];
        _stprintf_s(logfile, _T("C:\\temp\\log\\bytes_%08u.err"), exchange_count++);
        FILE* f = _tfopen(logfile, _T("wb"));
        _ftprintf(f, _T("Wanted %u bytes, got %u"), size, BytesRead);
        fclose(f);
#endif
    }
    else
    {
#ifdef LOG_EXCHANGE
        TCHAR logfile[MAX_PATH];
        _stprintf_s(logfile, _T("C:\\temp\\log\\bytes_%08u.in"), exchange_count++);
        FILE* f = _tfopen(logfile, _T("wb"));
        fwrite(in, 1, size, f);
        fclose(f);
#endif
    }
}

uint32_t get_code()
{
    uint32_t code;

    get_bytes(&code, sizeof(code));

    return code;
}

void getChunk(AEffect* effect, std::vector<uint8_t>& out)
{
    out.resize(0);

    uint32_t unique_id = (uint32_t)effect->uniqueID;

    append_be(out, unique_id);

    bool type_chunked = !!(effect->flags & effFlagsProgramChunks);

    append_be(out, type_chunked);

    if (!type_chunked)
    {
        uint32_t num_params = (uint32_t)effect->numParams;

        append_be(out, num_params);

        for (uint32_t i = 0; i < num_params; ++i)
        {
            float parameter = effect->getParameter(effect, (VstInt32)i);

            append_be(out, parameter);
        }
    }
    else
    {
        void* chunk;

        uint32_t size = (uint32_t)effect->dispatcher(effect, effGetChunk, 0, 0, &chunk, 0);

        append_be(out, size);

        size_t chunk_size = out.size();

        out.resize(chunk_size + size);

        memcpy(&out[chunk_size], chunk, size);
    }
}

void setChunk(AEffect* pEffect, std::vector<uint8_t> const& in)
{
    uint32_t size = (uint32_t)in.size();

    if (pEffect == nullptr || size == 0)
        return;

    const uint8_t* inc = in.data();

    uint32_t effect_id;

    retrieve_be(effect_id, inc, size);

    if (effect_id != (uint32_t)pEffect->uniqueID)
        return;

    bool type_chunked;

    retrieve_be(type_chunked, inc, size);

    if (type_chunked != !!(pEffect->flags & effFlagsProgramChunks))
        return;

    if (!type_chunked)
    {
        uint32_t num_params;

        retrieve_be(num_params, inc, size);

        if (num_params != (uint32_t)pEffect->numParams)
            return;

        for (uint32_t i = 0; i < num_params; ++i)
        {
            float parameter;

            retrieve_be(parameter, inc, size);

            pEffect->setParameter(pEffect, (VstInt32)i, parameter);
        }
    }
    else
    {
        uint32_t chunk_size;

        retrieve_be(chunk_size, inc, size);

        if (chunk_size > size)
            return;

        pEffect->dispatcher(pEffect, effSetChunk, 0, (VstIntPtr)chunk_size, (void*)inc, 0);
    }
}

struct MyDLGTEMPLATE : DLGTEMPLATE
{
    WORD ext[3];

    MyDLGTEMPLATE()
    {
        memset(this, 0, sizeof(*this));
    };
};

INT_PTR CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM, LPARAM lParam) noexcept
{
    AEffect* effect;

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, lParam);

        effect = (AEffect*)lParam;

        ::SetWindowTextW(hwnd, L"VST Editor");
        ::SetTimer(hwnd, 1, 20, 0);

        if (effect)
        {
            effect->dispatcher(effect, effEditOpen, 0, 0, hwnd, 0);

            ERect* eRect = 0;

            effect->dispatcher(effect, effEditGetRect, 0, 0, &eRect, 0);

            if (eRect)
            {
                int width = eRect->right - eRect->left;
                int height = eRect->bottom - eRect->top;

                if (width < 50)
                    width = 50;
                if (height < 50)
                    height = 50;

                RECT wRect;

                ::SetRect(&wRect, 0, 0, width, height);
                ::AdjustWindowRectEx(&wRect, (DWORD)::GetWindowLongW(hwnd, GWL_STYLE), FALSE, (DWORD)::GetWindowLongW(hwnd, GWL_EXSTYLE));

                width = wRect.right - wRect.left;
                height = wRect.bottom - wRect.top;

                ::SetWindowPos(hwnd, HWND_TOP, 0, 0, width, height, SWP_NOMOVE);
            }
        }
    }
    break;

    case WM_TIMER:
        effect = (AEffect*)::GetWindowLongPtrW(hwnd, GWLP_USERDATA);

        if (effect)
            effect->dispatcher(effect, effEditIdle, 0, 0, 0, 0);
        break;

    case WM_CLOSE:
    {
        effect = (AEffect*)::GetWindowLongPtrW(hwnd, GWLP_USERDATA);

        ::KillTimer(hwnd, 1);

        if (effect)
            effect->dispatcher(effect, effEditClose, 0, 0, 0, 0);

        ::EndDialog(hwnd, IDOK);
        break;
    }
    }

    return 0;
}

struct audioMasterData
{
    VstIntPtr effect_number;
};

static VstIntPtr VSTCALLBACK audioMaster(AEffect* effect, VstInt32 opcode, VstInt32, VstIntPtr, void* ptr, float)
{
    audioMasterData* data = nullptr;

    if (effect)
        data = (audioMasterData*)effect->user;

    switch (opcode)
    {
    case audioMasterVersion:
        return kVstVersion;

    case audioMasterCurrentId:
        if (data)
            return data->effect_number;
        break;

    case audioMasterGetVendorString:
        strncpy((char*)ptr, "NoWork, Inc.", 64);
        break;

    case audioMasterGetProductString:
        strncpy((char*)ptr, "VSTi Host Bridge", 64);
        break;

    case audioMasterGetVendorVersion:
        return 1000;

    case audioMasterGetLanguage:
        return kVstLangEnglish;

    case audioMasterVendorSpecific: // Steinberg HACK
        if (ptr)
        {
            uint32_t* blah = (uint32_t*)(((char*)ptr) - 4);
            if (*blah == 0x0737bb68)
            {
                *blah ^= 0x5CC8F349;
                blah[2] = 0x19E;
                return 0x1E7;
            }
        }
        break;

    case audioMasterGetDirectory:
        return (VstIntPtr)dll_dir.c_str();

        /* More crap */
    case DECLARE_VST_DEPRECATED(audioMasterNeedIdle):
        need_idle = true;
        return 0;
    }

    return 0;
}

LONG __stdcall myExceptFilterProc(LPEXCEPTION_POINTERS param)
{
    if (IsDebuggerPresent())
    {
        return UnhandledExceptionFilter(param);
    }
    else
    {
        // DumpCrashInfo( param );
        TerminateProcess(GetCurrentProcess(), 0);
        return 0; // never reached
    }
}

int main(int argc, const char* argv[])
{
    if (argv == nullptr || argc != 3)
        return 1;

    char* end_char = nullptr;

    unsigned Cookie = ::strtoul(argv[2], &end_char, 16);

    if (end_char == argv[2] || *end_char)
        return 2;

    uint32_t Sum = 0;

    end_char = (char*)argv[1];

    while (*end_char)
        Sum += *end_char++ * 820109;

    if (Sum != Cookie)
        return 3;

    unsigned code = 0;

    audioMasterData effectData[3] = { {0}, {1}, {2} };

    std::vector<uint8_t> State;

    uint32_t SampleRate = 44100;

    std::vector<uint8_t> chunk;
    std::vector<float> sample_buffer;

    null_file = ::CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

    pipe_in = ::GetStdHandle(STD_INPUT_HANDLE);
    pipe_out = ::GetStdHandle(STD_OUTPUT_HANDLE);

    ::SetStdHandle(STD_INPUT_HANDLE, null_file);
    ::SetStdHandle(STD_OUTPUT_HANDLE, null_file);

    {
        INITCOMMONCONTROLSEX icc =
        {
            sizeof(icc),
            ICC_WIN95_CLASSES | ICC_COOL_CLASSES | ICC_STANDARD_CLASSES };

        if (!::InitCommonControlsEx(&icc))
            return 4;
    }

    if (FAILED(::CoInitialize(NULL)))
        return 5;

#ifndef _DEBUG
    SetUnhandledExceptionFilter(myExceptFilterProc);
#endif

    dll_dir = argv[1];
    dll_dir = dll_dir.substr(0, dll_dir.find_last_of("/\\") + 1);

    float** float_list_in = nullptr;
    float** float_list_out = nullptr;
    float* float_null = nullptr;
    float* float_out = nullptr;
    uint32_t max_num_outputs;
    AEffect* Effect[3] = { 0, 0, 0 };
    main_func Main;

    HMODULE hDll = ::LoadLibraryA(argv[1]);

    if (hDll == 0)
    {
        code = 6;
        goto exit;
    }

#pragma warning(disable : 4191) // unsafe conversion from 'FARPROC' to 'main_func'
    Main = (main_func)::GetProcAddress(hDll, "VSTPluginMain");

    if (Main == nullptr)
    {
        Main = (main_func)::GetProcAddress(hDll, "main");

        if (Main == nullptr)
        {
            Main = (main_func)::GetProcAddress(hDll, "MAIN");

            if (Main == nullptr)
            {
                code = 7;
                goto exit;
            }
        }
    }

    {
        Effect[0] = Main(&audioMaster);

        if ((Effect[0] == nullptr) || (Effect[0]->magic != kEffectMagic))
        {
            code = 8;
            goto exit;
        }

        Effect[0]->user = &effectData[0];
        Effect[0]->dispatcher(Effect[0], effOpen, 0, 0, 0, 0);

        if ((Effect[0]->dispatcher(Effect[0], effGetPlugCategory, 0, 0, 0, 0) != kPlugCategSynth) || (Effect[0]->dispatcher(Effect[0], effCanDo, 0, 0, (void*)"receiveVstMidiEvent", 0) < 1))
        {
            code = 9;
            goto exit;
        }
    }

    max_num_outputs = (uint32_t)min(Effect[0]->numOutputs, 2);

    {
        char name_string[256] = { 0 };
        char vendor_string[256] = { 0 };
        char product_string[256] = { 0 };

        uint32_t name_string_length;
        uint32_t vendor_string_length;
        uint32_t product_string_length;
        uint32_t vendor_version;
        uint32_t unique_id;

        Effect[0]->dispatcher(Effect[0], effGetEffectName, 0, 0, &name_string, 0);
        Effect[0]->dispatcher(Effect[0], effGetVendorString, 0, 0, &vendor_string, 0);
        Effect[0]->dispatcher(Effect[0], effGetProductString, 0, 0, &product_string, 0);

        name_string_length = (uint32_t)::strlen(name_string);
        vendor_string_length = (uint32_t)::strlen(vendor_string);
        product_string_length = (uint32_t)::strlen(product_string);
        vendor_version = (uint32_t)Effect[0]->dispatcher(Effect[0], effGetVendorVersion, 0, 0, 0, 0);
        unique_id = (uint32_t)Effect[0]->uniqueID;

        put_code(0);
        put_code(name_string_length);
        put_code(vendor_string_length);
        put_code(product_string_length);
        put_code(vendor_version);
        put_code(unique_id);
        put_code(max_num_outputs);

        if (name_string_length)
            put_bytes(name_string, name_string_length);

        if (vendor_string_length)
            put_bytes(vendor_string, vendor_string_length);

        if (product_string_length)
            put_bytes(product_string, product_string_length);
    }

    for (;;)
    {
        auto command = static_cast<VSTHostCommand>(get_code());

        if (command == VSTHostCommand::Exit)
            break;

        switch (command)
        {
        case VSTHostCommand::GetChunk: // Get Chunk
        {
            getChunk(Effect[0], chunk);

            put_code(0);
            put_code((uint32_t)chunk.size());
            put_bytes(chunk.data(), (uint32_t)chunk.size());
            break;
        }

        case VSTHostCommand::SetChunk: // Set Chunk
        {
            uint32_t size = get_code();
            chunk.resize(size);
            if (size)
                get_bytes(chunk.data(), size);

            setChunk(Effect[0], chunk);
            setChunk(Effect[1], chunk);
            setChunk(Effect[2], chunk);

            put_code(0);
            break;
        }

        case VSTHostCommand::HasEditor: // Has Editor
        {
            uint32_t has_editor = (Effect[0]->flags & effFlagsHasEditor) ? 1u : 0u;

            put_code(0);
            put_code(has_editor);
            break;
        }

        case VSTHostCommand::DisplayEditorModal: // Display Editor Modal
        {
            if (Effect[0]->flags & effFlagsHasEditor)
            {
                MyDLGTEMPLATE t;

                t.style = WS_POPUPWINDOW | WS_DLGFRAME | DS_MODALFRAME | DS_CENTER;

                DialogBoxIndirectParam(0, &t, ::GetDesktopWindow(), (DLGPROC)EditorProc, (LPARAM)(Effect[0]));

                getChunk(Effect[0], chunk);
                setChunk(Effect[1], chunk);
                setChunk(Effect[2], chunk);
            }

            put_code(0);
            break;
        }

        case VSTHostCommand::SetSampleRate: // Set Sample Rate
        {
            uint32_t size = get_code();

            if (size != sizeof(SampleRate))
            {
                code = 10;
                goto exit;
            }

            SampleRate = get_code();

            put_code(0);
            break;
        }

        case VSTHostCommand::Reset: // Reset
        {
            if (Effect[2])
            {
                if (State.size())
                    Effect[2]->dispatcher(Effect[2], effStopProcess, 0, 0, 0, 0);

                Effect[2]->dispatcher(Effect[2], effClose, 0, 0, 0, 0);
                Effect[2] = nullptr;
            }

            if (Effect[1])
            {
                if (State.size())
                    Effect[1]->dispatcher(Effect[1], effStopProcess, 0, 0, 0, 0);

                Effect[1]->dispatcher(Effect[1], effClose, 0, 0, 0, 0);
                Effect[1] = nullptr;
            }

            if (State.size())
                Effect[0]->dispatcher(Effect[0], effStopProcess, 0, 0, 0, 0);

            Effect[0]->dispatcher(Effect[0], effClose, 0, 0, 0, 0);

            State.resize(0);

            freeChain();

            Effect[0] = Main(&audioMaster);

            if (!Effect[0])
            {
                code = 8;
                goto exit;
            }

            Effect[0]->user = &effectData[0];
            Effect[0]->dispatcher(Effect[0], effOpen, 0, 0, 0, 0);
            setChunk(Effect[0], chunk);

            put_code(0);
            break;
        }

        case VSTHostCommand::SendMIDIEvent: // Send MIDI Event
        {
            myVstEvent* ev = (myVstEvent*)calloc(sizeof(myVstEvent), 1);

            if (ev != nullptr)
            {
                if (evTail)
                    evTail->next = ev;

                evTail = ev;

                if (!_EventHead)
                    _EventHead = ev;

                uint32_t b = get_code();

                ev->port = (b & 0x7F000000) >> 24;

                if (ev->port > 2)
                    ev->port = 2;

                ev->ev.midiEvent.type = kVstMidiType;
                ev->ev.midiEvent.byteSize = sizeof(ev->ev.midiEvent);

                memcpy(&ev->ev.midiEvent.midiData, &b, 3);

                put_code(0);
            }
            break;
        }

        case VSTHostCommand::SendSysexEvent: // Send System Exclusive Event
        {
            myVstEvent* ev = (myVstEvent*)calloc(sizeof(myVstEvent), 1);

            if (ev != nullptr)
            {
                if (evTail)
                    evTail->next = ev;

                evTail = ev;

                if (!_EventHead)
                    _EventHead = ev;

                uint32_t size = get_code();
                uint32_t port = size >> 24;
                size &= 0xFFFFFF;

                ev->port = port;

                if (ev->port > 2)
                    ev->port = 2;

                ev->ev.sysexEvent.type = kVstSysExType;
                ev->ev.sysexEvent.byteSize = sizeof(ev->ev.sysexEvent);
                ev->ev.sysexEvent.dumpBytes = (VstInt32)size;
                ev->ev.sysexEvent.sysexDump = (char*)::malloc(size);

                get_bytes(ev->ev.sysexEvent.sysexDump, size);

                put_code(0);
            }
            break;
        }

        case VSTHostCommand::RenderSamples: // Render Samples
        {
            if (Effect[1] == nullptr)
            {
                Effect[1] = Main(&audioMaster);

                if (Effect[1] == nullptr)
                {
                    code = 11;
                    goto exit;
                }

                Effect[1]->user = &effectData[1];
                Effect[1]->dispatcher(Effect[1], effOpen, 0, 0, 0, 0);

                setChunk(Effect[1], chunk);
            }

            if (Effect[2] == nullptr)
            {
                Effect[2] = Main(&audioMaster);

                if (Effect[2] == nullptr)
                {
                    code = 11;
                    goto exit;
                }

                Effect[2]->user = &effectData[2];
                Effect[2]->dispatcher(Effect[2], effOpen, 0, 0, 0, 0);

                setChunk(Effect[2], chunk);
            }

            // Initialize the lists and the sample buffer.
            if (State.size() == 0)
            {
                Effect[0]->dispatcher(Effect[0], effSetSampleRate, 0, 0, 0, float(SampleRate));
                Effect[0]->dispatcher(Effect[0], effSetBlockSize, 0, BUFFER_SIZE, 0, 0);
                Effect[0]->dispatcher(Effect[0], effMainsChanged, 0, 1, 0, 0);
                Effect[0]->dispatcher(Effect[0], effStartProcess, 0, 0, 0, 0);

                Effect[1]->dispatcher(Effect[1], effSetSampleRate, 0, 0, 0, float(SampleRate));
                Effect[1]->dispatcher(Effect[1], effSetBlockSize, 0, BUFFER_SIZE, 0, 0);
                Effect[1]->dispatcher(Effect[1], effMainsChanged, 0, 1, 0, 0);
                Effect[1]->dispatcher(Effect[1], effStartProcess, 0, 0, 0, 0);

                Effect[2]->dispatcher(Effect[2], effSetSampleRate, 0, 0, 0, float(SampleRate));
                Effect[2]->dispatcher(Effect[2], effSetBlockSize, 0, BUFFER_SIZE, 0, 0);
                Effect[2]->dispatcher(Effect[2], effMainsChanged, 0, 1, 0, 0);
                Effect[2]->dispatcher(Effect[2], effStartProcess, 0, 0, 0, 0);

                {
                    {
                        size_t buffer_size = sizeof(float*) * (Effect[0]->numInputs + (Effect[0]->numOutputs * 3)); // float lists

                        buffer_size += sizeof(float) * BUFFER_SIZE;                             // null input
                        buffer_size += sizeof(float) * BUFFER_SIZE * Effect[0]->numOutputs * 3; // outputs

                        State.resize(buffer_size);
                    }

                    float_list_in = (float**)State.data();
                    float_list_out = float_list_in + Effect[0]->numInputs;
                    float_null = (float*)(float_list_out + Effect[0]->numOutputs * 3);
                    float_out = float_null + BUFFER_SIZE;

                    for (uint32_t i = 0; i < (uint32_t)Effect[0]->numInputs; ++i)
                        float_list_in[i] = float_null;

                    for (uint32_t i = 0; i < (uint32_t)Effect[0]->numOutputs * 3; ++i)
                        float_list_out[i] = float_out + (BUFFER_SIZE * i);

                    memset(float_null, 0, BUFFER_SIZE * sizeof(float));

                    size_t NewSize = BUFFER_SIZE * max_num_outputs * sizeof(float);

                    sample_buffer.resize(NewSize);
                }
            }

            if (need_idle && float_list_in && float_list_out)
            {
                Effect[0]->dispatcher(Effect[0], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);
                Effect[1]->dispatcher(Effect[1], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);
                Effect[2]->dispatcher(Effect[2], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);

                if (!idle_started)
                {
                    unsigned idle_run = BUFFER_SIZE * 200;

                    while (idle_run)
                    {
                        uint32_t count_to_do = min(idle_run, BUFFER_SIZE);
                        uint32_t num_outputs = (uint32_t)Effect[0]->numOutputs;

                        Effect[0]->processReplacing(Effect[0], float_list_in, float_list_out, (VstInt32)count_to_do);
                        Effect[1]->processReplacing(Effect[1], float_list_in, float_list_out + num_outputs, (VstInt32)count_to_do);
                        Effect[2]->processReplacing(Effect[2], float_list_in, float_list_out + num_outputs * 2, (VstInt32)count_to_do);

                        Effect[0]->dispatcher(Effect[0], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);
                        Effect[1]->dispatcher(Effect[1], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);
                        Effect[2]->dispatcher(Effect[2], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);

                        idle_run -= count_to_do;
                    }
                }
            }

            VstEvents* events[3] = { 0 };

            if (_EventHead)
            {
                unsigned event_count[3] = { 0 };

                myVstEvent* ev = _EventHead;

                while (ev)
                {
                    event_count[ev->port]++;

                    ev = ev->next;
                }

                if (event_count[0] != 0)
                {
                    //                      events[0] = (VstEvents *) malloc(sizeof(VstInt32) + sizeof(VstIntPtr) + (sizeof(VstEvent *) * event_count[0]));
                    events[0] = (VstEvents*)malloc(offsetof(struct VstEvents, events) - offsetof(struct VstEvents, numEvents) + (sizeof(VstEvent*) * event_count[0]));

                    events[0]->numEvents = (VstInt32)event_count[0];
                    events[0]->reserved = 0;

                    ev = _EventHead;

                    for (unsigned i = 0; ev;)
                    {
                        if (!ev->port)
                            events[0]->events[i++] = (VstEvent*)&ev->ev;

                        ev = ev->next;
                    }

                    Effect[0]->dispatcher(Effect[0], effProcessEvents, 0, 0, events[0], 0);
                }

                if (event_count[1] != 0)
                {
                    //                      events[1] = (VstEvents *) malloc(sizeof(VstInt32) + sizeof(VstIntPtr) + (sizeof(VstEvent *) * event_count[1]));
                    events[1] = (VstEvents*)malloc(offsetof(struct VstEvents, events) - offsetof(struct VstEvents, numEvents) + (sizeof(VstEvent*) * event_count[1]));

                    events[1]->numEvents = (VstInt32)event_count[1];
                    events[1]->reserved = 0;

                    ev = _EventHead;

                    for (unsigned i = 0; ev;)
                    {
                        if (ev->port == 1)
                            events[1]->events[i++] = (VstEvent*)&ev->ev;

                        ev = ev->next;
                    }

                    Effect[1]->dispatcher(Effect[1], effProcessEvents, 0, 0, events[1], 0);
                }

                if (event_count[2] != 0)
                {
                    //                      events[2] = (VstEvents *) malloc(sizeof(VstInt32) + sizeof(VstIntPtr) + (sizeof(VstEvent *) * event_count[2]));
                    events[2] = (VstEvents*)malloc(offsetof(struct VstEvents, events) - offsetof(struct VstEvents, numEvents) + (sizeof(VstEvent*) * event_count[2]));

                    events[2]->numEvents = (VstInt32)event_count[2];
                    events[2]->reserved = 0;

                    ev = _EventHead;

                    for (unsigned i = 0; ev;)
                    {
                        if (ev->port == 2)
                            events[2]->events[i++] = (VstEvent*)&ev->ev;

                        ev = ev->next;
                    }

                    Effect[2]->dispatcher(Effect[2], effProcessEvents, 0, 0, events[2], 0);
                }
            }

            if (need_idle)
            {
                Effect[0]->dispatcher(Effect[0], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);
                Effect[1]->dispatcher(Effect[1], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);
                Effect[2]->dispatcher(Effect[2], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);

                if (!idle_started)
                {
                    if (events[0])
                        Effect[0]->dispatcher(Effect[0], effProcessEvents, 0, 0, events[0], 0);
                    if (events[1])
                        Effect[1]->dispatcher(Effect[1], effProcessEvents, 0, 0, events[1], 0);
                    if (events[2])
                        Effect[2]->dispatcher(Effect[2], effProcessEvents, 0, 0, events[2], 0);

                    idle_started = true;
                }
            }

            uint32_t SampleCount = get_code();

            put_code(0);

            if (float_list_out)
            {
                while (SampleCount)
                {
                    unsigned SamplesToDo = min(SampleCount, BUFFER_SIZE);

                    uint32_t num_outputs = (uint32_t)Effect[0]->numOutputs;
                    //                      unsigned sample_start = 0;

                    Effect[0]->processReplacing(Effect[0], float_list_in, float_list_out, (VstInt32)SamplesToDo);
                    Effect[1]->processReplacing(Effect[1], float_list_in, float_list_out + num_outputs, (VstInt32)SamplesToDo);
                    Effect[2]->processReplacing(Effect[2], float_list_in, float_list_out + num_outputs * 2, (VstInt32)SamplesToDo);

                    float* out = sample_buffer.data();

                    if (max_num_outputs == 2)
                    {
                        for (unsigned i = 0; i < SamplesToDo; ++i)
                        {
                            float sample = (float_out[i] +
                                float_out[i + BUFFER_SIZE * num_outputs] +
                                float_out[i + BUFFER_SIZE * num_outputs * 2]);
                            out[0] = sample;

                            sample = (float_out[i + BUFFER_SIZE] +
                                float_out[i + BUFFER_SIZE + BUFFER_SIZE * num_outputs] +
                                float_out[i + BUFFER_SIZE + BUFFER_SIZE * num_outputs * 2]);

                            out[1] = sample;

                            out += 2;
                        }
                    }
                    else
                    {
                        for (unsigned i = 0; i < SamplesToDo; ++i)
                        {
                            float sample = (float_out[i] +
                                float_out[i + BUFFER_SIZE * num_outputs] +
                                float_out[i + BUFFER_SIZE * num_outputs * 2]);
                            out[0] = sample;

                            out++;
                        }
                    }

                    put_bytes(sample_buffer.data(), SamplesToDo * max_num_outputs * sizeof(float));

                    SampleCount -= SamplesToDo;
                }
            }

            if (events[0])
                free(events[0]);

            if (events[1])
                free(events[1]);

            if (events[2])
                free(events[2]);

            freeChain();
            break;
        }

        case VSTHostCommand::SendMIDIEventWithTimestamp: // Send MIDI Event, with timestamp
        {
            myVstEvent* ev = (myVstEvent*)calloc(sizeof(myVstEvent), 1);

            if (evTail)
                evTail->next = ev;

            evTail = ev;

            if (!_EventHead)
                _EventHead = ev;

            uint32_t b = get_code();
            uint32_t timestamp = get_code();

            ev->port = (b & 0x7F000000) >> 24;

            if (ev->port > 2)
                ev->port = 2;

            ev->ev.midiEvent.type = kVstMidiType;
            ev->ev.midiEvent.byteSize = sizeof(ev->ev.midiEvent);
            memcpy(&ev->ev.midiEvent.midiData, &b, 3);
            ev->ev.midiEvent.deltaFrames = (VstInt32)timestamp;

            put_code(0);
            break;
        }

        case VSTHostCommand::SendSysexEventWithTimestamp: // Send System Exclusive Event, with timestamp
        {
            myVstEvent* ev = (myVstEvent*)calloc(sizeof(myVstEvent), 1);

            if (evTail)
                evTail->next = ev;

            evTail = ev;

            if (!_EventHead)
                _EventHead = ev;

            uint32_t size = get_code();
            uint32_t port = size >> 24;
            size &= 0xFFFFFF;

            uint32_t timestamp = get_code();

            ev->port = port;
            if (ev->port > 2)
                ev->port = 0;
            ev->ev.sysexEvent.type = kVstSysExType;
            ev->ev.sysexEvent.byteSize = sizeof(ev->ev.sysexEvent);
            ev->ev.sysexEvent.dumpBytes = (VstInt32)size;
            ev->ev.sysexEvent.sysexDump = (char*)malloc(size);
            ev->ev.sysexEvent.deltaFrames = (VstInt32)timestamp;

            get_bytes(ev->ev.sysexEvent.sysexDump, size);

            put_code(0);
            break;
        }

        default:
        {
            code = 12;
            goto exit;
        }
        }
    }

exit:
    if (Effect[2])
    {
        if (State.size())
            Effect[2]->dispatcher(Effect[2], effStopProcess, 0, 0, 0, 0);

        Effect[2]->dispatcher(Effect[2], effClose, 0, 0, 0, 0);
    }

    if (Effect[1])
    {
        if (State.size())
            Effect[1]->dispatcher(Effect[1], effStopProcess, 0, 0, 0, 0);

        Effect[1]->dispatcher(Effect[1], effClose, 0, 0, 0, 0);
    }

    if (Effect[0])
    {
        if (State.size())
            Effect[0]->dispatcher(Effect[0], effStopProcess, 0, 0, 0, 0);

        Effect[0]->dispatcher(Effect[0], effClose, 0, 0, 0, 0);
    }

    freeChain();

    if (hDll)
        FreeLibrary(hDll);

    CoUninitialize();

    put_code(code);

    if (null_file)
    {
        CloseHandle(null_file);

        SetStdHandle(STD_INPUT_HANDLE, pipe_in);
        SetStdHandle(STD_OUTPUT_HANDLE, pipe_out);
    }

    return (int)code;
}
