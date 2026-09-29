// vcam.cpp — ATEM Emulator Virtual Camera (DirectShow push-source, no strmbase)
// Registers itself under HKCU — no admin required.
// Shares frames with the main exe via named shared memory + event (vcam_shared.h).
//
// This DLL runs inside the camera consumer (OBS, Zoom, ...): a fault here
// takes that program down, so every pointer and size from outside is checked
// and object lifetimes follow the COM rules strictly:
//  - the pin has no reference count of its own: it lives exactly as long as
//    its filter (AddRef/Release go to the filter), and enumerators keep the
//    filter alive;
//  - every object is counted, so DllCanUnloadNow says yes only when none is
//    left, and the frame thread holds the DLL loaded until it has returned.

#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
// initguid.h must come first so DEFINE_GUID defines real objects
#include <initguid.h>
#include <windows.h>
#include <dshow.h>
#include "vcam_shared.h"
#include <atomic>
#include <mutex>
#include <new>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")

// ── GUIDs ─────────────────────────────────────────────────────────────────────

// {A4B65A7C-82CE-4B08-9C56-45F68F2E30D8}
DEFINE_GUID(CLSID_AtemVirtualCam,
    0xa4b65a7c,0x82ce,0x4b08,0x9c,0x56,0x45,0xf6,0x8f,0x2e,0x30,0xd8);

// IKsPropertySet {31EFAC30-515C-11d0-A9AA-00AA0061BE93}
static const GUID MY_IID_IKsPropertySet =
    {0x31efac30,0x515c,0x11d0,{0xa9,0xaa,0x00,0xaa,0x00,0x61,0xbe,0x93}};

// AMPROPSETID_Pin {9B00F101-1567-11d1-B3F1-00AA003761C5}
static const GUID MY_AMPROPSETID_Pin =
    {0x9b00f101,0x1567,0x11d1,{0xb3,0xf1,0x00,0xaa,0x00,0x37,0x61,0xc5}};

// PIN_CATEGORY_CAPTURE {FB6C4281-0353-11d1-905F-0000C0CC16BA}
static const GUID MY_PIN_CATEGORY_CAPTURE =
    {0xfb6c4281,0x0353,0x11d1,{0x90,0x5f,0x00,0x00,0xc0,0xcc,0x16,0xba}};

// ── IKsPropertySet (forward-declared, avoids ksproxy.h dependency) ─────────────

struct __declspec(uuid("31EFAC30-515C-11d0-A9AA-00AA0061BE93"))
IMyKsPropertySet : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Set(
        REFGUID,DWORD,LPVOID,DWORD,LPVOID,DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE Get(
        REFGUID PropSet,DWORD Id,LPVOID InstanceData,DWORD InstanceLen,
        LPVOID PropertyData,DWORD DataLen,DWORD* BytesReturned) = 0;
    virtual HRESULT STDMETHODCALLTYPE QuerySupported(
        REFGUID,DWORD,DWORD*) = 0;
};

// ── Module globals ─────────────────────────────────────────────────────────────

static HMODULE g_hMod = nullptr;
static std::atomic<long> g_locks{ 0 };     // IClassFactory::LockServer
static std::atomic<long> g_objects{ 0 };   // live COM objects of this DLL

// Counts a live object from construction to destruction.
struct Counted {
    Counted() { ++g_objects; }
    ~Counted() { --g_objects; }
    Counted(const Counted&) = delete;
    Counted& operator=(const Counted&) = delete;
};

// ── Media type (1280x720 RGB32 @ 30fps) ───────────────────────────────────────

static const int kFPS = 30;
static const REFERENCE_TIME kFrameLen = 10000000LL / kFPS;  // 100-ns units
static const long kFrameBytes = kVCamWidth * kVCamHeight * 4;

static VIDEOINFOHEADER g_vih = {};
static AM_MEDIA_TYPE   g_mt  = {};

static void InitMediaType()
{
    g_vih.rcSource            = { 0, 0, kVCamWidth, kVCamHeight };
    g_vih.rcTarget            = g_vih.rcSource;
    g_vih.dwBitRate           = kVCamWidth * kVCamHeight * 4 * 8 * kFPS;
    g_vih.AvgTimePerFrame     = kFrameLen;
    g_vih.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    g_vih.bmiHeader.biWidth       = kVCamWidth;
    g_vih.bmiHeader.biHeight      = kVCamHeight; // positive = bottom-up in DirectShow
    g_vih.bmiHeader.biPlanes      = 1;
    g_vih.bmiHeader.biBitCount    = 32;
    g_vih.bmiHeader.biCompression = BI_RGB;
    g_vih.bmiHeader.biSizeImage   = kFrameBytes;

    g_mt.majortype            = MEDIATYPE_Video;
    g_mt.subtype              = MEDIASUBTYPE_RGB32;
    g_mt.bFixedSizeSamples    = TRUE;
    g_mt.bTemporalCompression = FALSE;
    g_mt.lSampleSize          = kFrameBytes;
    g_mt.formattype           = FORMAT_VideoInfo;
    g_mt.cbFormat             = sizeof(VIDEOINFOHEADER);
    g_mt.pbFormat             = (BYTE*)&g_vih;
}

static AM_MEDIA_TYPE* AllocMT()
{
    auto* mt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    if (!mt) return nullptr;
    *mt = g_mt;
    mt->pbFormat = (BYTE*)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
    if (!mt->pbFormat) { CoTaskMemFree(mt); return nullptr; }
    memcpy(mt->pbFormat, &g_vih, sizeof(VIDEOINFOHEADER));
    return mt;
}

// The only format this camera produces: 1280x720 RGB32, bottom-up.
static bool IsOurFormat(const AM_MEDIA_TYPE* mt)
{
    if (!mt || mt->majortype != MEDIATYPE_Video || mt->subtype != MEDIASUBTYPE_RGB32) return false;
    if (mt->formattype != FORMAT_VideoInfo || !mt->pbFormat || mt->cbFormat < sizeof(VIDEOINFOHEADER)) return false;
    const auto* vih = (const VIDEOINFOHEADER*)mt->pbFormat;
    return vih->bmiHeader.biWidth == kVCamWidth && vih->bmiHeader.biHeight == kVCamHeight &&
           vih->bmiHeader.biBitCount == 32 && vih->bmiHeader.biCompression == BI_RGB;
}

// ── IEnumMediaTypes ────────────────────────────────────────────────────────────

struct EnumMT : IEnumMediaTypes, Counted {
    long ref = 1;
    int  pos = 0;
    explicit EnumMT(int startPos = 0) : pos(startPos) {}

    STDMETHODIMP QueryInterface(REFIID r, void** p) {
        if (!p) return E_POINTER;
        if (r == IID_IUnknown || r == IID_IEnumMediaTypes)
            { *p = this; AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() { long r = InterlockedDecrement(&ref); if (!r) delete this; return r; }

    STDMETHODIMP Next(ULONG n, AM_MEDIA_TYPE** pmt, ULONG* fetched) {
        if (!pmt || (n > 1 && !fetched)) return E_POINTER;
        ULONG got = 0;
        while (got < n && pos == 0) {
            pmt[got] = AllocMT();
            if (!pmt[got]) break;
            ++got; ++pos;
        }
        if (fetched) *fetched = got;
        return (got == n) ? S_OK : S_FALSE; }
    STDMETHODIMP Skip(ULONG n) { pos += (int)n; return (pos <= 1) ? S_OK : S_FALSE; }
    STDMETHODIMP Reset()       { pos = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumMediaTypes** pp) {
        if (!pp) return E_POINTER;
        *pp = new (std::nothrow) EnumMT(pos);
        return *pp ? S_OK : E_OUTOFMEMORY; }
};

// ── Forward declarations ───────────────────────────────────────────────────────

struct CVCamFilter;
struct CVCamPin;

// ── IEnumPins ─────────────────────────────────────────────────────────────────

// Holds a reference to the pin (that is, its filter) for as long as it lives.
struct CVEnumPins : IEnumPins, Counted {
    long      ref = 1;
    int       pos = 0;
    CVCamPin* pin;
    CVEnumPins(CVCamPin* p, int startPos = 0);
    ~CVEnumPins();

    STDMETHODIMP QueryInterface(REFIID r, void** p) {
        if (!p) return E_POINTER;
        if (r == IID_IUnknown || r == IID_IEnumPins)
            { *p = this; AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() { long r = InterlockedDecrement(&ref); if (!r) delete this; return r; }

    STDMETHODIMP Next(ULONG n, IPin** pp, ULONG* fetched);
    STDMETHODIMP Skip(ULONG n) { pos += (int)n; return (pos <= 1) ? S_OK : S_FALSE; }
    STDMETHODIMP Reset()       { pos = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumPins** pp) {
        if (!pp) return E_POINTER;
        *pp = new (std::nothrow) CVEnumPins(pin, pos);
        return *pp ? S_OK : E_OUTOFMEMORY; }
};

// ── CVCamPin ───────────────────────────────────────────────────────────────────

struct CVCamPin : IPin, IAMStreamConfig, IMyKsPropertySet {
    CVCamFilter*   filter;              // owns this pin; AddRef/Release go to it
    IPin*          connected = nullptr;
    IMemInputPin*  memInput  = nullptr;
    IMemAllocator* allocator = nullptr;

    HANDLE thread    = nullptr;
    HANDLE stopEvt   = nullptr;
    std::atomic<bool> streaming{ false };
    std::atomic<long> generation{ 0 };   // one per StartStreaming: a thread left over from an earlier start ends

    // The shared frame, opened by the frame thread when the emulator's
    // camera is on and kept until the pin goes (a thread may outlive a stop).
    std::mutex       attachLock;
    HANDLE           hSharedMem = nullptr;
    HANDLE           hFrameEvt  = nullptr;
    VCamSharedFrame* pFrame     = nullptr;

    // The graph's clock and start time for this run (timestamps).
    IReferenceClock* runClock = nullptr;
    REFERENCE_TIME   runStart = 0;

    explicit CVCamPin(CVCamFilter* f) : filter(f) {
        stopEvt = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    }
    ~CVCamPin() {
        if (thread)     CloseHandle(thread);
        if (stopEvt)    CloseHandle(stopEvt);
        if (pFrame)     UnmapViewOfFile(pFrame);
        if (hSharedMem) CloseHandle(hSharedMem);
        if (hFrameEvt)  CloseHandle(hFrameEvt);
        if (runClock)   runClock->Release();
        if (memInput)   memInput->Release();
        if (allocator)  allocator->Release();
        if (connected)  connected->Release();
    }

    // IUnknown: the filter's reference count
    STDMETHODIMP QueryInterface(REFIID r, void** p) {
        if (!p) return E_POINTER;
        if (r == IID_IUnknown || r == IID_IPin)
            { *p = (IPin*)this; AddRef(); return S_OK; }
        if (r == IID_IAMStreamConfig)
            { *p = (IAMStreamConfig*)this; AddRef(); return S_OK; }
        if (r == MY_IID_IKsPropertySet)
            { *p = (IMyKsPropertySet*)this; AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

    // IPin — Connect
    STDMETHODIMP Connect(IPin* pReceiver, const AM_MEDIA_TYPE* pmt) {
        if (!pReceiver) return E_POINTER;
        if (connected) return VFW_E_ALREADY_CONNECTED;
        if (pmt) {
            // A partial type (GUID_NULL parts) is fine; what is given must be ours.
            static const GUID kNull = {};
            if (pmt->majortype != kNull && pmt->majortype != MEDIATYPE_Video)
                return VFW_E_TYPE_NOT_ACCEPTED;
            if (pmt->subtype   != kNull && pmt->subtype   != MEDIASUBTYPE_RGB32)
                return VFW_E_TYPE_NOT_ACCEPTED;
            if (pmt->formattype == FORMAT_VideoInfo && pmt->pbFormat && !IsOurFormat(pmt))
                return VFW_E_TYPE_NOT_ACCEPTED;
        }

        HRESULT hr = pReceiver->ReceiveConnection((IPin*)this, &g_mt);
        if (FAILED(hr)) return hr;

        IMemInputPin* input = nullptr;
        hr = pReceiver->QueryInterface(IID_IMemInputPin, (void**)&input);
        if (FAILED(hr) || !input) { pReceiver->Disconnect(); return FAILED(hr) ? hr : E_NOINTERFACE; }

        IMemAllocator* pAlloc = nullptr;
        input->GetAllocator(&pAlloc);
        if (!pAlloc) {
            static const GUID CLSID_MemAlloc =
                {0x1e651cc0,0xb199,0x11d0,{0x82,0x12,0x00,0xc0,0x4f,0xc3,0x2c,0x45}};
            CoCreateInstance(CLSID_MemAlloc, nullptr, CLSCTX_INPROC_SERVER,
                             IID_IMemAllocator, (void**)&pAlloc);
        }
        auto fail = [&](HRESULT why) {
            if (pAlloc) pAlloc->Release();
            input->Release();
            pReceiver->Disconnect();
            return why;
        };
        if (!pAlloc) return fail(E_OUTOFMEMORY);

        // The downstream allocator must really give buffers a whole frame fits in.
        ALLOCATOR_PROPERTIES props = { 2, kFrameBytes, 4, 0 };
        ALLOCATOR_PROPERTIES actual = {};
        hr = pAlloc->SetProperties(&props, &actual);
        if (FAILED(hr)) return fail(hr);
        if (actual.cbBuffer < kFrameBytes || actual.cBuffers < 1) return fail(VFW_E_SIZENOTSET);
        hr = pAlloc->Commit();
        if (FAILED(hr)) return fail(hr);
        hr = input->NotifyAllocator(pAlloc, FALSE);
        if (FAILED(hr)) { pAlloc->Decommit(); return fail(hr); }

        if (allocator) allocator->Release();
        if (memInput) memInput->Release();
        allocator = pAlloc;
        memInput = input;
        connected = pReceiver; connected->AddRef();
        return S_OK;
    }

    STDMETHODIMP ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) { return E_UNEXPECTED; }

    STDMETHODIMP Disconnect() {
        if (!connected) return S_FALSE;
        if (streaming) return VFW_E_NOT_STOPPED;
        connected->Release(); connected = nullptr;
        if (memInput)  { memInput->Release(); memInput = nullptr; }
        if (allocator) { allocator->Decommit(); allocator->Release(); allocator = nullptr; }
        return S_OK; }

    STDMETHODIMP ConnectedTo(IPin** pp) {
        if (!pp) return E_POINTER;
        if (!connected) { *pp = nullptr; return VFW_E_NOT_CONNECTED; }
        *pp = connected; connected->AddRef(); return S_OK; }

    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
        if (!pmt) return E_POINTER;
        if (!connected) return VFW_E_NOT_CONNECTED;
        *pmt = g_mt;
        pmt->pbFormat = (BYTE*)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
        if (!pmt->pbFormat) { pmt->cbFormat = 0; return E_OUTOFMEMORY; }
        memcpy(pmt->pbFormat, &g_vih, sizeof(VIDEOINFOHEADER));
        return S_OK; }

    STDMETHODIMP QueryPinInfo(PIN_INFO* pi);
    STDMETHODIMP QueryDirection(PIN_DIRECTION* pd) {
        if (!pd) return E_POINTER;
        *pd = PINDIR_OUTPUT; return S_OK; }
    STDMETHODIMP QueryId(LPWSTR* id) {
        if (!id) return E_POINTER;
        *id = (LPWSTR)CoTaskMemAlloc(2 * sizeof(wchar_t));
        if (!*id) return E_OUTOFMEMORY;
        wcscpy_s(*id, 2, L"0"); return S_OK; }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) {
        if (!pmt) return E_POINTER;
        return IsOurFormat(pmt) ? S_OK : S_FALSE; }
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** pp) {
        if (!pp) return E_POINTER;
        *pp = new (std::nothrow) EnumMT;
        return *pp ? S_OK : E_OUTOFMEMORY; }
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG* n) { if (n) *n = 0; return E_NOTIMPL; }
    STDMETHODIMP EndOfStream()    { return S_OK; }
    STDMETHODIMP BeginFlush()     { return S_OK; }
    STDMETHODIMP EndFlush()       { return S_OK; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) { return S_OK; }

    // IAMStreamConfig: one format only
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) {
        if (!pmt) return S_OK;                               // "back to the default": it is
        return IsOurFormat(pmt) ? S_OK : VFW_E_INVALIDMEDIATYPE; }
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) {
        if (!ppmt) return E_POINTER;
        *ppmt = AllocMT(); return *ppmt ? S_OK : E_OUTOFMEMORY; }
    STDMETHODIMP GetNumberOfCapabilities(int* count, int* size) {
        if (!count || !size) return E_POINTER;
        *count = 1; *size = sizeof(VIDEO_STREAM_CONFIG_CAPS); return S_OK; }
    STDMETHODIMP GetStreamCaps(int i, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
        if (!ppmt || !pSCC) return E_POINTER;
        if (i != 0) return S_FALSE;
        *ppmt = AllocMT();
        if (!*ppmt) return E_OUTOFMEMORY;
        auto* caps = (VIDEO_STREAM_CONFIG_CAPS*)pSCC;
        ZeroMemory(caps, sizeof(*caps));
        caps->guid              = FORMAT_VideoInfo;
        caps->InputSize         = { kVCamWidth, kVCamHeight };
        caps->MinCroppingSize   = caps->MaxCroppingSize  = caps->InputSize;
        caps->CropGranularityX  = caps->CropGranularityY = 1;
        caps->MinOutputSize     = caps->MaxOutputSize    = caps->InputSize;
        caps->OutputGranularityX= caps->OutputGranularityY = 1;
        caps->MinFrameInterval  = caps->MaxFrameInterval = kFrameLen;
        caps->MinBitsPerSecond  = caps->MaxBitsPerSecond = (LONG)(kVCamWidth*kVCamHeight*4*8*kFPS);
        return S_OK; }

    // IKsPropertySet — tells OBS this is a capture pin
    STDMETHODIMP Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) { return E_NOTIMPL; }
    STDMETHODIMP Get(REFGUID propSet, DWORD id, LPVOID, DWORD, LPVOID pData, DWORD dataLen, DWORD* pBytes) {
        if (propSet != MY_AMPROPSETID_Pin)            return 0x80070492L; // E_PROP_SET_UNSUPPORTED
        if (id      != 0 /*AMPROPERTY_PIN_CATEGORY*/) return 0x80070490L; // E_PROP_ID_UNSUPPORTED
        if (!pData) {                                  // a size query
            if (!pBytes) return E_POINTER;
            *pBytes = sizeof(GUID);
            return S_OK;
        }
        if (dataLen < sizeof(GUID)) return E_UNEXPECTED;   // never write past the caller's buffer
        *(GUID*)pData = MY_PIN_CATEGORY_CAPTURE;
        if (pBytes) *pBytes = sizeof(GUID);
        return S_OK; }
    STDMETHODIMP QuerySupported(REFGUID propSet, DWORD id, DWORD* support) {
        if (propSet != MY_AMPROPSETID_Pin)            return 0x80070492L;
        if (id      != 0 /*AMPROPERTY_PIN_CATEGORY*/) return 0x80070490L;
        if (support) *support = 1; // KSPROPERTY_SUPPORT_GET
        return S_OK; }

    // ── Streaming ────────────────────────────────────────────────────────────

    void StartStreaming(IReferenceClock* clock, REFERENCE_TIME start);
    void StopStreaming() {
        if (!streaming) return;
        streaming = false;
        SetEvent(stopEvt);
        if (allocator) allocator->Decommit();          // wakes a thread waiting in GetBuffer
        if (thread) {
            // A paused renderer can hold the thread in Receive until the graph
            // runs or stops, so don't wait forever: the thread ends by itself
            // once Receive returns, holding the filter (and so the mapped
            // frame) and the DLL until it has.
            WaitForSingleObject(thread, 3000);
            CloseHandle(thread);
            thread = nullptr;
        }
    }

    struct ThreadStart { CVCamPin* pin; long gen; HMODULE module; };
    static DWORD WINAPI ThreadProc(LPVOID p) {
        auto* start = static_cast<ThreadStart*>(p);
        CVCamPin* pin = start->pin;
        const long gen = start->gen;
        HMODULE module = start->module;
        delete start;
        pin->Loop(gen);
        pin->Release();                                // may destroy the filter
        FreeLibraryAndExitThread(module, 0);           // the DLL stays loaded until here
    }

    bool Current(long gen) const { return streaming && generation == gen; }

    // Opens the shared frame once the emulator's camera is on: only one of
    // the right version and size, set up by the producer.
    void Attach() {
        std::lock_guard<std::mutex> lock(attachLock);
        if (pFrame) return;
        if (!hSharedMem) hSharedMem = OpenFileMappingA(FILE_MAP_READ, FALSE, kSharedMemName);
        if (!hSharedMem) return;
        auto* view = (VCamSharedFrame*)MapViewOfFile(hSharedMem, FILE_MAP_READ, 0, 0, 0);
        MEMORY_BASIC_INFORMATION info = {};
        bool ok = view && VirtualQuery(view, &info, sizeof(info)) && info.RegionSize >= sizeof(VCamSharedFrame) &&
                  view->magic == kVCamMagic && view->version == kVCamVersion && view->size == sizeof(VCamSharedFrame) &&
                  view->width == (unsigned)kVCamWidth && view->height == (unsigned)kVCamHeight;
        if (!ok) {
            // Not (yet) a frame this DLL can read: try again later.
            if (view) UnmapViewOfFile(view);
            CloseHandle(hSharedMem);
            hSharedMem = nullptr;
            return;
        }
        pFrame = view;
        if (!hFrameEvt) hFrameEvt = OpenEventA(SYNCHRONIZE, FALSE, kSharedEventName);
    }

    // The frame is mapped read-only here: the sequence is read with a plain
    // load and a barrier (an interlocked operation would write, and fault).
    long Sequence() const {
        const long s = pFrame->sequence;
        MemoryBarrier();
        return s;
    }

    // Copies a complete frame, flipped (Qt is top-down, RGB32 with positive
    // biHeight is bottom-up). False if the producer kept writing meanwhile.
    bool CopyFrame(BYTE* dst, long* seq) {
        const DWORD stride = kVCamWidth * 4;
        for (int tries = 0; tries < 3; ++tries) {
            const long before = Sequence();
            if (before & 1) { Sleep(1); continue; }
            const BYTE* src = pFrame->bgra;
            for (int y = 0; y < kVCamHeight; ++y)
                memcpy(dst + (DWORD)(kVCamHeight - 1 - y) * stride, src + (DWORD)y * stride, stride);
            MemoryBarrier();
            if (Sequence() == before) {
                *seq = before;
                return true;
            }
        }
        return false;
    }

    void Loop(long gen) {
        long lastSeq = -1;
        bool first = true;
        REFERENCE_TIME counted = 0;          // timestamps without a graph clock
        ULONGLONG lastSent = 0, lastAttach = 0;

        while (Current(gen)) {
            if (!pFrame && GetTickCount64() - lastAttach >= 1000) {   // the emulator's camera may come on later
                lastAttach = GetTickCount64();
                Attach();
            }
            // A new frame is signalled; also look every 15 ms, since the
            // event wakes one waiter and several cameras may be open.
            HANDLE h[2] = { stopEvt, hFrameEvt };
            DWORD w = WaitForMultipleObjects(hFrameEvt ? 2 : 1, h, FALSE, 15);
            if (w == WAIT_OBJECT_0 || !Current(gen)) break;

            const bool fresh = pFrame && Sequence() != lastSeq;
            const bool due = GetTickCount64() - lastSent >= 1000 / kFPS;   // repeat the last frame at 30 fps
            if (!fresh && !due) continue;
            if (!connected || !allocator || !memInput) continue;

            IMediaSample* sample = nullptr;
            if (FAILED(allocator->GetBuffer(&sample, nullptr, nullptr, 0)) || !sample) continue;
            BYTE* dst = nullptr;
            if (FAILED(sample->GetPointer(&dst)) || !dst || sample->GetSize() < kFrameBytes) {
                sample->Release();
                continue;
            }
            if (pFrame) {
                long seq = lastSeq;
                if (!CopyFrame(dst, &seq)) { sample->Release(); continue; }
                lastSeq = seq;
            } else {
                ZeroMemory(dst, kFrameBytes);          // no emulator camera yet: black
            }

            // Stream time from the graph's clock when there is one.
            REFERENCE_TIME t0 = counted, now = 0;
            if (runClock && SUCCEEDED(runClock->GetTime(&now))) t0 = now - runStart;
            if (t0 < 0) t0 = 0;
            REFERENCE_TIME t1 = t0 + kFrameLen;
            counted = t1;
            sample->SetActualDataLength(kFrameBytes);
            sample->SetSyncPoint(TRUE);
            sample->SetDiscontinuity(first ? TRUE : FALSE);
            sample->SetTime(&t0, &t1);
            first = false;

            memInput->Receive(sample);
            sample->Release();
            lastSent = GetTickCount64();
        }
    }
};

// ── CVCamFilter ────────────────────────────────────────────────────────────────

struct CVCamFilter : IBaseFilter, Counted {
    long             ref   = 1;
    FILTER_STATE     state = State_Stopped;
    IReferenceClock* clock = nullptr;
    IFilterGraph*    graph = nullptr;   // not referenced (the graph owns the filter)
    CVCamPin         pin{ this };
    wchar_t          name[128] = {};

    ~CVCamFilter() { if (clock) clock->Release(); }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID r, void** p) {
        if (!p) return E_POINTER;
        if (r == IID_IUnknown || r == IID_IBaseFilter ||
            r == IID_IMediaFilter || r == IID_IPersist)
            { *p = (IBaseFilter*)this; AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() { long r = InterlockedDecrement(&ref); if (!r) delete this; return r; }

    // IPersist
    STDMETHODIMP GetClassID(CLSID* c) {
        if (!c) return E_POINTER;
        *c = CLSID_AtemVirtualCam; return S_OK; }

    // IMediaFilter
    STDMETHODIMP Stop() {
        state = State_Stopped; pin.StopStreaming(); return S_OK; }
    STDMETHODIMP Pause() {
        state = State_Paused; pin.StopStreaming(); return S_OK; }
    STDMETHODIMP Run(REFERENCE_TIME start) {
        state = State_Running; pin.StartStreaming(clock, start); return S_OK; }
    STDMETHODIMP GetState(DWORD, FILTER_STATE* s) {
        if (!s) return E_POINTER;
        *s = state; return S_OK; }
    STDMETHODIMP SetSyncSource(IReferenceClock* c) {
        if (c) c->AddRef();
        if (clock) clock->Release();
        clock = c; return S_OK; }
    STDMETHODIMP GetSyncSource(IReferenceClock** c) {
        if (!c) return E_POINTER;
        *c = clock; if (clock) clock->AddRef(); return S_OK; }

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** pp) {
        if (!pp) return E_POINTER;
        *pp = new (std::nothrow) CVEnumPins(&pin);
        return *pp ? S_OK : E_OUTOFMEMORY; }
    STDMETHODIMP FindPin(LPCWSTR id, IPin** pp) {
        if (!id || !pp) return E_POINTER;
        if (wcscmp(id, L"0") == 0) { *pp = (IPin*)&pin; pin.AddRef(); return S_OK; }
        *pp = nullptr; return VFW_E_NOT_FOUND; }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* fi) {
        if (!fi) return E_POINTER;
        wcsncpy_s(fi->achName, 128, name, _TRUNCATE);
        fi->pGraph = graph; if (graph) graph->AddRef(); return S_OK; }
    STDMETHODIMP JoinFilterGraph(IFilterGraph* g, LPCWSTR n) {
        graph = g;
        if (n) wcsncpy_s(name, 128, n, _TRUNCATE);
        else name[0] = 0;
        return S_OK; }
    STDMETHODIMP QueryVendorInfo(LPWSTR*) { return E_NOTIMPL; }
};

// ── Deferred implementations ───────────────────────────────────────────────────

ULONG CVCamPin::AddRef()  { return filter->AddRef(); }
ULONG CVCamPin::Release() { return filter->Release(); }

void CVCamPin::StartStreaming(IReferenceClock* clock, REFERENCE_TIME start) {
    if (streaming) return;
    ResetEvent(stopEvt);
    if (allocator && FAILED(allocator->Commit())) return;   // StopStreaming decommits it
    if (clock) clock->AddRef();
    if (runClock) runClock->Release();
    runClock = clock;
    runStart = start;

    // The thread holds the filter (so the pin and the mapped frame) and the
    // DLL itself until it has returned.
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&CVCamPin::ThreadProc, &module)) return;
    auto* ts = new (std::nothrow) ThreadStart{ this, ++generation, module };
    if (!ts) { FreeLibrary(module); return; }
    streaming = true;
    AddRef();
    thread = CreateThread(nullptr, 0, ThreadProc, ts, 0, nullptr);
    if (!thread) {
        delete ts;
        streaming = false;
        Release();
        FreeLibrary(module);
    }
}

CVEnumPins::CVEnumPins(CVCamPin* p, int startPos) : pos(startPos), pin(p) { pin->AddRef(); }
CVEnumPins::~CVEnumPins() { pin->Release(); }

STDMETHODIMP CVEnumPins::Next(ULONG n, IPin** pp, ULONG* fetched) {
    if (!pp || (n > 1 && !fetched)) return E_POINTER;
    ULONG got = 0;
    while (got < n && pos == 0) { pp[got++] = (IPin*)pin; pin->AddRef(); ++pos; }
    if (fetched) *fetched = got;
    return (got == n) ? S_OK : S_FALSE; }

STDMETHODIMP CVCamPin::QueryPinInfo(PIN_INFO* pi) {
    if (!pi) return E_POINTER;
    pi->pFilter = (IBaseFilter*)filter; filter->AddRef();
    wcscpy_s(pi->achName, 128, L"Output");
    pi->dir = PINDIR_OUTPUT;
    return S_OK; }

// ── COM class factory ──────────────────────────────────────────────────────────

struct CVCamFactory : IClassFactory, Counted {
    long ref = 1;
    STDMETHODIMP QueryInterface(REFIID r, void** p) {
        if (!p) return E_POINTER;
        if (r == IID_IUnknown || r == IID_IClassFactory)
            { *p = this; AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() { long r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID r, void** p) {
        if (!p) return E_POINTER;
        *p = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* f = new (std::nothrow) CVCamFilter;
        if (!f) return E_OUTOFMEMORY;
        HRESULT hr = f->QueryInterface(r, p);
        f->Release(); return hr; }
    STDMETHODIMP LockServer(BOOL lock) {
        if (lock) ++g_locks;
        else      --g_locks;
        return S_OK; }
};

// ── DLL entry points ───────────────────────────────────────────────────────────

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hMod = hMod;
        DisableThreadLibraryCalls(hMod);
        InitMediaType();
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (rclsid != CLSID_AtemVirtualCam) return CLASS_E_CLASSNOTAVAILABLE;
    auto* f = new (std::nothrow) CVCamFactory;
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release(); return hr;
}

// Only when no object of this DLL is alive and no server lock is held.
STDAPI DllCanUnloadNow() { return (g_locks == 0 && g_objects == 0) ? S_OK : S_FALSE; }

// ── Registry helpers ───────────────────────────────────────────────────────────

static HRESULT RegSetSZ(HKEY root, const wchar_t* path,
                         const wchar_t* name, const wchar_t* val)
{
    HKEY hk;
    LONG r = RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr);
    if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);
    r = RegSetValueExW(hk, name, 0, REG_SZ,
                       (const BYTE*)val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
    return HRESULT_FROM_WIN32(r);
}

static const wchar_t* kClsidStr = L"{A4B65A7C-82CE-4B08-9C56-45F68F2E30D8}";
static const wchar_t* kCatStr   = L"{860BB310-5D01-11d0-BD3B-00A0C911CE86}";
static const wchar_t* kFriendly = L"ATEM Emulator Virtual Camera";

STDAPI DllRegisterServer()
{
    wchar_t dllPath[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_hMod, dllPath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);

    HRESULT hr = S_OK;
    auto set = [&hr](const wchar_t* path, const wchar_t* name, const wchar_t* val) {
        HRESULT r = RegSetSZ(HKEY_CURRENT_USER, path, name, val);
        if (FAILED(r) && SUCCEEDED(hr)) hr = r;
    };

    // HKCU\Software\Classes\CLSID\{clsid}  — COM class registration
    wchar_t clsKey[128];
    swprintf_s(clsKey, L"Software\\Classes\\CLSID\\%s", kClsidStr);
    set(clsKey, nullptr, kFriendly);

    wchar_t inprocKey[192];
    swprintf_s(inprocKey, L"%s\\InprocServer32", clsKey);
    set(inprocKey, nullptr, dllPath);
    set(inprocKey, L"ThreadingModel", L"Both");

    // HKCU\Software\Classes\CLSID\{cat}\Instance\{clsid}  — device enumeration
    wchar_t instKey[256];
    swprintf_s(instKey, L"Software\\Classes\\CLSID\\%s\\Instance\\%s", kCatStr, kClsidStr);
    set(instKey, L"FriendlyName", kFriendly);
    set(instKey, L"CLSID",        kClsidStr);
    set(instKey, L"DevicePath",   L"@device:vcam:AtemEmulator");

    // Legacy ActiveMovie DevEnum (needed by some older consumers)
    wchar_t devKey[256];
    swprintf_s(devKey, L"Software\\Microsoft\\ActiveMovie\\devenum\\%s\\%s", kCatStr, kClsidStr);
    set(devKey, L"FriendlyName", kFriendly);
    set(devKey, L"CLSID",        kClsidStr);

    return hr;
}

STDAPI DllUnregisterServer()
{
    HRESULT hr = S_OK;
    auto check = [&hr](LONG r) {
        if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND && SUCCEEDED(hr)) hr = HRESULT_FROM_WIN32(r);
    };
    wchar_t key[256];

    swprintf_s(key, L"Software\\Classes\\CLSID\\%s", kClsidStr);
    check(RegDeleteTreeW(HKEY_CURRENT_USER, key));

    swprintf_s(key, L"Software\\Classes\\CLSID\\%s\\Instance\\%s", kCatStr, kClsidStr);
    check(RegDeleteTreeW(HKEY_CURRENT_USER, key));

    swprintf_s(key, L"Software\\Microsoft\\ActiveMovie\\devenum\\%s\\%s", kCatStr, kClsidStr);
    check(RegDeleteKeyW(HKEY_CURRENT_USER, key));

    return hr;
}
