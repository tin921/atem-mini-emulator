// vcam.cpp — ATEM Emulator Virtual Camera (DirectShow push-source, no strmbase)
// Registers itself under HKCU — no admin required.
// Shares frames with the main exe via named shared memory + event.

#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
// initguid.h must come first so DEFINE_GUID defines real objects
#include <initguid.h>
#include <windows.h>
#include <dshow.h>
#include "vcam_shared.h"

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

// CLSID_VideoInputDeviceCategory {860BB310-5D01-11d0-BD3B-00A0C911CE86}
static const GUID MY_CLSID_VideoInputDeviceCategory =
    {0x860bb310,0x5d01,0x11d0,{0xbd,0x3b,0x00,0xa0,0xc9,0x11,0xce,0x86}};

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

static HMODULE g_hMod   = nullptr;
static long    g_locks  = 0;

// ── Media type (1280x720 RGB32 @ 30fps) ───────────────────────────────────────

static const int kFPS      = 30;
static const REFERENCE_TIME kFrameLen = 10000000LL / kFPS;  // 100-ns units

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
    g_vih.bmiHeader.biSizeImage   = kVCamWidth * kVCamHeight * 4;

    g_mt.majortype            = MEDIATYPE_Video;
    g_mt.subtype              = MEDIASUBTYPE_RGB32;
    g_mt.bFixedSizeSamples    = TRUE;
    g_mt.bTemporalCompression = FALSE;
    g_mt.lSampleSize          = kVCamWidth * kVCamHeight * 4;
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

// ── IEnumMediaTypes ────────────────────────────────────────────────────────────

struct EnumMT : IEnumMediaTypes {
    long ref = 1;
    int  pos = 0;
    EnumMT(int startPos = 0) : pos(startPos) {}

    STDMETHODIMP QueryInterface(REFIID r, void** p) {
        if (r == IID_IUnknown || r == IID_IEnumMediaTypes)
            { *p = this; AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() { long r = InterlockedDecrement(&ref); if (!r) delete this; return r; }

    STDMETHODIMP Next(ULONG n, AM_MEDIA_TYPE** pmt, ULONG* fetched) {
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
    STDMETHODIMP Clone(IEnumMediaTypes** pp) { *pp = new EnumMT(pos); return S_OK; }
};

// ── Forward declarations ───────────────────────────────────────────────────────

struct CVCamFilter;
struct CVCamPin;

// ── IEnumPins ─────────────────────────────────────────────────────────────────

struct CVEnumPins : IEnumPins {
    long      ref = 1;
    int       pos = 0;
    CVCamPin* pin;
    explicit CVEnumPins(CVCamPin* p, int startPos = 0) : pin(p), pos(startPos) {}

    STDMETHODIMP QueryInterface(REFIID r, void** p) {
        if (r == IID_IUnknown || r == IID_IEnumPins)
            { *p = this; AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() { long r = InterlockedDecrement(&ref); if (!r) delete this; return r; }

    STDMETHODIMP Next(ULONG n, IPin** pp, ULONG* fetched);
    STDMETHODIMP Skip(ULONG n) { pos += (int)n; return S_OK; }
    STDMETHODIMP Reset()       { pos = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumPins** pp) { *pp = new CVEnumPins(pin, pos); return S_OK; }
};

// ── CVCamPin ───────────────────────────────────────────────────────────────────

struct CVCamPin : IPin, IAMStreamConfig, IMyKsPropertySet {
    long           ref       = 1;
    CVCamFilter*   filter;
    IPin*          connected = nullptr;
    IMemInputPin*  memInput  = nullptr;
    IMemAllocator* allocator = nullptr;

    HANDLE thread    = nullptr;
    HANDLE stopEvt   = nullptr;
    bool   streaming = false;

    HANDLE           hSharedMem = nullptr;
    HANDLE           hFrameEvt  = nullptr;
    VCamSharedFrame* pFrame     = nullptr;

    explicit CVCamPin(CVCamFilter* f) : filter(f) {
        stopEvt = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    }
    ~CVCamPin() {
        if (stopEvt)    CloseHandle(stopEvt);
        if (pFrame)     UnmapViewOfFile(pFrame);
        if (hSharedMem) CloseHandle(hSharedMem);
        if (hFrameEvt)  CloseHandle(hFrameEvt);
        if (memInput)   memInput->Release();
        if (allocator)  allocator->Release();
        if (connected)  connected->Release();
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID r, void** p) {
        if (r == IID_IUnknown || r == IID_IPin)
            { *p = (IPin*)this; AddRef(); return S_OK; }
        if (r == IID_IAMStreamConfig)
            { *p = (IAMStreamConfig*)this; AddRef(); return S_OK; }
        if (r == MY_IID_IKsPropertySet)
            { *p = (IMyKsPropertySet*)this; AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() { long r = InterlockedDecrement(&ref); if (!r) delete this; return r; }

    // IPin — Connect
    STDMETHODIMP Connect(IPin* pReceiver, const AM_MEDIA_TYPE* pmt) {
        if (connected) return VFW_E_ALREADY_CONNECTED;
        if (pmt) {
            static const GUID kNull = {};
            if (pmt->majortype != kNull && pmt->majortype != MEDIATYPE_Video)
                return VFW_E_TYPE_NOT_ACCEPTED;
            if (pmt->subtype   != kNull && pmt->subtype   != MEDIASUBTYPE_RGB32)
                return VFW_E_TYPE_NOT_ACCEPTED;
        }

        HRESULT hr = pReceiver->ReceiveConnection((IPin*)this, &g_mt);
        if (FAILED(hr)) return hr;

        hr = pReceiver->QueryInterface(IID_IMemInputPin, (void**)&memInput);
        if (FAILED(hr)) { pReceiver->Disconnect(); return hr; }

        IMemAllocator* pAlloc = nullptr;
        memInput->GetAllocator(&pAlloc);
        if (!pAlloc) {
            static const GUID CLSID_MemAlloc =
                {0x1e651cc0,0xb199,0x11d0,{0x82,0x12,0x00,0xc0,0x4f,0xc3,0x2c,0x45}};
            CoCreateInstance(CLSID_MemAlloc, nullptr, CLSCTX_INPROC_SERVER,
                             IID_IMemAllocator, (void**)&pAlloc);
        }
        if (!pAlloc) { pReceiver->Disconnect(); memInput->Release(); memInput = nullptr; return E_OUTOFMEMORY; }

        ALLOCATOR_PROPERTIES props = { 2, (long)(kVCamWidth * kVCamHeight * 4), 4, 0 };
        ALLOCATOR_PROPERTIES actual;
        pAlloc->SetProperties(&props, &actual);
        pAlloc->Commit();
        if (allocator) allocator->Release();
        allocator = pAlloc;

        memInput->NotifyAllocator(allocator, FALSE);
        connected = pReceiver; connected->AddRef();
        return S_OK;
    }

    STDMETHODIMP ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) { return E_UNEXPECTED; }

    STDMETHODIMP Disconnect() {
        if (!connected) return S_FALSE;
        connected->Release(); connected = nullptr;
        if (memInput)  { memInput->Release(); memInput = nullptr; }
        if (allocator) { allocator->Decommit(); allocator->Release(); allocator = nullptr; }
        return S_OK; }

    STDMETHODIMP ConnectedTo(IPin** pp) {
        if (!connected) { *pp = nullptr; return VFW_E_NOT_CONNECTED; }
        *pp = connected; connected->AddRef(); return S_OK; }

    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
        if (!connected) return VFW_E_NOT_CONNECTED;
        *pmt = g_mt;
        pmt->pbFormat = (BYTE*)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
        memcpy(pmt->pbFormat, &g_vih, sizeof(VIDEOINFOHEADER));
        return S_OK; }

    STDMETHODIMP QueryPinInfo(PIN_INFO* pi);
    STDMETHODIMP QueryDirection(PIN_DIRECTION* pd) { *pd = PINDIR_OUTPUT; return S_OK; }
    STDMETHODIMP QueryId(LPWSTR* id) {
        *id = (LPWSTR)CoTaskMemAlloc(4);
        if (!*id) return E_OUTOFMEMORY;
        wcscpy(*id, L"0"); return S_OK; }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) {
        if (pmt->majortype != MEDIATYPE_Video)    return S_FALSE;
        if (pmt->subtype   != MEDIASUBTYPE_RGB32) return S_FALSE;
        return S_OK; }
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** pp) { *pp = new EnumMT; return S_OK; }
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG* n) { *n = 0; return E_NOTIMPL; }
    STDMETHODIMP EndOfStream()    { return S_OK; }
    STDMETHODIMP BeginFlush()     { return S_OK; }
    STDMETHODIMP EndFlush()       { return S_OK; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) { return S_OK; }

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE*) { return S_OK; }
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) { *ppmt = AllocMT(); return *ppmt ? S_OK : E_OUTOFMEMORY; }
    STDMETHODIMP GetNumberOfCapabilities(int* count, int* size) {
        *count = 1; *size = sizeof(VIDEO_STREAM_CONFIG_CAPS); return S_OK; }
    STDMETHODIMP GetStreamCaps(int i, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
        if (i != 0) return S_FALSE;
        *ppmt = AllocMT();
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
    STDMETHODIMP Get(REFGUID propSet, DWORD id, LPVOID, DWORD, LPVOID pData, DWORD, DWORD* pBytes) {
        if (propSet != MY_AMPROPSETID_Pin)         return 0x80070057L; // E_PROP_SET_UNSUPPORTED
        if (id      != 0 /*AMPROPERTY_PIN_CATEGORY*/) return 0x80070490L; // E_PROP_ID_UNSUPPORTED
        if (!pData) return E_POINTER;
        *(GUID*)pData = MY_PIN_CATEGORY_CAPTURE;
        if (pBytes) *pBytes = sizeof(GUID);
        return S_OK; }
    STDMETHODIMP QuerySupported(REFGUID propSet, DWORD id, DWORD* support) {
        if (propSet != MY_AMPROPSETID_Pin)            return 0x80070057L;
        if (id      != 0 /*AMPROPERTY_PIN_CATEGORY*/) return 0x80070490L;
        *support = 1; // KSPROPERTY_SUPPORT_GET
        return S_OK; }

    // ── Streaming thread ────────────────────────────────────────────────────────

    void StartStreaming() {
        if (streaming) return;
        streaming = true;
        ResetEvent(stopEvt);

        hSharedMem = OpenFileMappingA(FILE_MAP_READ, FALSE, kSharedMemName);
        if (hSharedMem)
            pFrame = (VCamSharedFrame*)MapViewOfFile(hSharedMem, FILE_MAP_READ, 0, 0, 0);
        hFrameEvt = OpenEventA(SYNCHRONIZE, FALSE, kSharedEventName);

        thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    }

    void StopStreaming() {
        if (!streaming) return;
        streaming = false;
        SetEvent(stopEvt);
        if (thread) { WaitForSingleObject(thread, 3000); CloseHandle(thread); thread = nullptr; }
        if (allocator) allocator->Decommit();
        if (pFrame)     { UnmapViewOfFile(pFrame);  pFrame     = nullptr; }
        if (hSharedMem) { CloseHandle(hSharedMem);  hSharedMem = nullptr; }
        if (hFrameEvt)  { CloseHandle(hFrameEvt);   hFrameEvt  = nullptr; }
    }

    static DWORD WINAPI ThreadProc(LPVOID p) { ((CVCamPin*)p)->Loop(); return 0; }

    void Loop() {
        REFERENCE_TIME ts     = 0;
        long           lastId = -1;
        const DWORD stride    = kVCamWidth * 4;

        while (streaming) {
            // Wait up to 100ms for a new frame signal (or stop)
            if (hFrameEvt) {
                HANDLE h[2] = { stopEvt, hFrameEvt };
                DWORD w = WaitForMultipleObjects(2, h, FALSE, 100);
                if (w == WAIT_OBJECT_0) break; // stop
            } else {
                if (WaitForSingleObject(stopEvt, 33) == WAIT_OBJECT_0) break;
            }
            if (!streaming) break;

            // Check for new frame
            if (pFrame) {
                long fid = pFrame->frameId; // volatile, aligned — safe on x86
                if (fid == lastId) continue;
                lastId = fid;
            }

            if (!connected || !allocator || !memInput) continue;

            IMediaSample* sample = nullptr;
            if (FAILED(allocator->GetBuffer(&sample, nullptr, nullptr, 0))) continue;

            BYTE* pDst = nullptr;
            sample->GetPointer(&pDst);

            if (pFrame && pDst) {
                // Qt stores top-down; DirectShow RGB32 with positive biHeight is bottom-up
                // Flip vertically on copy
                const BYTE* src = pFrame->bgra;
                for (int y = 0; y < kVCamHeight; ++y) {
                    memcpy(pDst + (DWORD)(kVCamHeight - 1 - y) * stride,
                           src  + (DWORD)y * stride, stride);
                }
            } else if (pDst) {
                ZeroMemory(pDst, kVCamWidth * kVCamHeight * 4);
            }

            sample->SetActualDataLength(kVCamWidth * kVCamHeight * 4);
            sample->SetSyncPoint(TRUE);
            REFERENCE_TIME te = ts + kFrameLen;
            sample->SetTime(&ts, &te);
            ts = te;

            memInput->Receive(sample);
            sample->Release();
        }
    }
};

// ── CVCamFilter ────────────────────────────────────────────────────────────────

struct CVCamFilter : IBaseFilter {
    long             ref   = 1;
    FILTER_STATE     state = State_Stopped;
    IReferenceClock* clock = nullptr;
    IFilterGraph*    graph = nullptr;
    CVCamPin*        pin;
    wchar_t          name[128] = {};

    CVCamFilter()  { pin = new CVCamPin(this); }
    ~CVCamFilter() { if (clock) clock->Release(); pin->Release(); }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID r, void** p) {
        if (r == IID_IUnknown || r == IID_IBaseFilter ||
            r == IID_IMediaFilter || r == IID_IPersist)
            { *p = (IBaseFilter*)this; AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() { long r = InterlockedDecrement(&ref); if (!r) delete this; return r; }

    // IPersist
    STDMETHODIMP GetClassID(CLSID* c) { *c = CLSID_AtemVirtualCam; return S_OK; }

    // IMediaFilter
    STDMETHODIMP Stop() {
        state = State_Stopped; pin->StopStreaming(); return S_OK; }
    STDMETHODIMP Pause() {
        state = State_Paused; pin->StopStreaming(); return S_OK; }
    STDMETHODIMP Run(REFERENCE_TIME) {
        state = State_Running; pin->StartStreaming(); return S_OK; }
    STDMETHODIMP GetState(DWORD, FILTER_STATE* s) { *s = state; return S_OK; }
    STDMETHODIMP SetSyncSource(IReferenceClock* c) {
        if (clock) clock->Release(); clock = c; if (clock) clock->AddRef(); return S_OK; }
    STDMETHODIMP GetSyncSource(IReferenceClock** c) {
        *c = clock; if (clock) clock->AddRef(); return S_OK; }

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** pp)    { *pp = new CVEnumPins(pin); return S_OK; }
    STDMETHODIMP FindPin(LPCWSTR id, IPin** pp) {
        if (wcscmp(id, L"0") == 0) { *pp = (IPin*)pin; pin->AddRef(); return S_OK; }
        *pp = nullptr; return VFW_E_NOT_FOUND; }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* fi) {
        wcsncpy_s(fi->achName, 128, name, _TRUNCATE);
        fi->pGraph = graph; if (graph) graph->AddRef(); return S_OK; }
    STDMETHODIMP JoinFilterGraph(IFilterGraph* g, LPCWSTR n) {
        graph = g; if (n) wcsncpy_s(name, 128, n, _TRUNCATE); return S_OK; }
    STDMETHODIMP QueryVendorInfo(LPWSTR*) { return E_NOTIMPL; }
};

// ── Deferred inline implementations ───────────────────────────────────────────

STDMETHODIMP CVEnumPins::Next(ULONG n, IPin** pp, ULONG* fetched) {
    ULONG got = 0;
    while (got < n && pos == 0) { pp[got++] = (IPin*)pin; pin->AddRef(); ++pos; }
    if (fetched) *fetched = got;
    return (got == n) ? S_OK : S_FALSE; }

STDMETHODIMP CVCamPin::QueryPinInfo(PIN_INFO* pi) {
    pi->pFilter = (IBaseFilter*)filter; filter->AddRef();
    wcscpy_s(pi->achName, 128, L"Output");
    pi->dir = PINDIR_OUTPUT;
    return S_OK; }

// ── COM class factory ──────────────────────────────────────────────────────────

struct CVCamFactory : IClassFactory {
    long ref = 1;
    STDMETHODIMP QueryInterface(REFIID r, void** p) {
        if (r == IID_IUnknown || r == IID_IClassFactory)
            { *p = this; AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() { long r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID r, void** p) {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* f = new CVCamFilter;
        HRESULT hr = f->QueryInterface(r, p);
        f->Release(); return hr; }
    STDMETHODIMP LockServer(BOOL lock) {
        if (lock) InterlockedIncrement(&g_locks);
        else      InterlockedDecrement(&g_locks);
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
    if (rclsid != CLSID_AtemVirtualCam) return CLASS_E_CLASSNOTAVAILABLE;
    auto* f = new CVCamFactory;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release(); return hr;
}

STDAPI DllCanUnloadNow() { return (g_locks == 0) ? S_OK : S_FALSE; }

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
    GetModuleFileNameW(g_hMod, dllPath, MAX_PATH);

    // HKCU\Software\Classes\CLSID\{clsid}  — COM class registration
    wchar_t clsKey[128];
    swprintf_s(clsKey, L"Software\\Classes\\CLSID\\%s", kClsidStr);
    RegSetSZ(HKEY_CURRENT_USER, clsKey, nullptr, kFriendly);

    wchar_t inprocKey[192];
    swprintf_s(inprocKey, L"%s\\InprocServer32", clsKey);
    RegSetSZ(HKEY_CURRENT_USER, inprocKey, nullptr, dllPath);
    RegSetSZ(HKEY_CURRENT_USER, inprocKey, L"ThreadingModel", L"Both");

    // HKCU\Software\Classes\CLSID\{cat}\Instance\{clsid}  — device enumeration
    wchar_t instKey[256];
    swprintf_s(instKey, L"Software\\Classes\\CLSID\\%s\\Instance\\%s", kCatStr, kClsidStr);
    RegSetSZ(HKEY_CURRENT_USER, instKey, L"FriendlyName", kFriendly);
    RegSetSZ(HKEY_CURRENT_USER, instKey, L"CLSID",        kClsidStr);
    RegSetSZ(HKEY_CURRENT_USER, instKey, L"DevicePath",   L"@device:vcam:AtemEmulator");

    // Legacy ActiveMovie DevEnum (needed by some older consumers)
    wchar_t devKey[256];
    swprintf_s(devKey, L"Software\\Microsoft\\ActiveMovie\\devenum\\%s\\%s", kCatStr, kClsidStr);
    RegSetSZ(HKEY_CURRENT_USER, devKey, L"FriendlyName", kFriendly);
    RegSetSZ(HKEY_CURRENT_USER, devKey, L"CLSID",        kClsidStr);

    return S_OK;
}

STDAPI DllUnregisterServer()
{
    wchar_t key[256];

    swprintf_s(key, L"Software\\Classes\\CLSID\\%s", kClsidStr);
    RegDeleteTreeW(HKEY_CURRENT_USER, key);

    swprintf_s(key, L"Software\\Classes\\CLSID\\%s\\Instance\\%s", kCatStr, kClsidStr);
    RegDeleteTreeW(HKEY_CURRENT_USER, key);

    swprintf_s(key, L"Software\\Microsoft\\ActiveMovie\\devenum\\%s\\%s", kCatStr, kClsidStr);
    RegDeleteKeyW(HKEY_CURRENT_USER, key);

    return S_OK;
}
