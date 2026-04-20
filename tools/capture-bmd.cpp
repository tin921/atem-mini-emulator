/**
 * capture-bmd.cpp — ATEM capture tool via BMDSwitcherAPI COM SDK
 *
 * Connects to an ATEM Mini (USB or Ethernet), dumps all state the SDK
 * exposes, and probes macro run/stop behaviour.  Output goes to both
 * stdout and captured-output.log (same file consumed by run.py update
 * workflow).
 *
 * Usage:
 *   capture.exe              — USB auto-detect
 *   capture.exe 192.168.10.240 — Ethernet / manual IP
 *
 * Build: see CMakeLists.txt in this directory.
 */

#define INITGUID
#include <guiddef.h>

// ── BMD GUIDs not shipped in the SDK (same as bmd-guids.cpp) ──────────────────
// CLSID_CBMDSwitcherDiscovery  "3EFEA8DB-282F-4C23-B218-FC8A2FF0861E"
DEFINE_GUID(CLSID_CBMDSwitcherDiscovery,
    0x3EFEA8DB, 0x282F, 0x4C23, 0xB2, 0x18, 0xFC, 0x8A, 0x2F, 0xF0, 0x86, 0x1E);

// IID_IBMDSwitcherMacroPoolCallback  "E29294A0-FB4C-418D-9AE1-C6CBA288104F"
DEFINE_GUID(IID_IBMDSwitcherMacroPoolCallback,
    0xE29294A0, 0xFB4C, 0x418D, 0x9A, 0xE1, 0xC6, 0xCB, 0xA2, 0x88, 0x10, 0x4F);

#include <windows.h>
#include <comutil.h>

#include "BMDSwitcherAPI.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ── Logging ───────────────────────────────────────────────────────────────────

static std::ofstream g_logFile;

static std::string timestamp()
{
    auto now  = std::chrono::system_clock::now();
    auto t    = std::chrono::system_clock::to_time_t(now);
    auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()) % 1000;
    struct tm tm_buf;
    localtime_s(&tm_buf, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, (int)ms.count());
    return buf;
}

static void log(const std::string& line)
{
    std::string out = "[" + timestamp() + "] " + line;
    std::cout << out << "\n";
    if (g_logFile.is_open())
        g_logFile << out << "\n" << std::flush;
}

static void log_raw(const std::string& line)
{
    std::cout << line << "\n";
    if (g_logFile.is_open())
        g_logFile << line << "\n" << std::flush;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string bstr_to_utf8(BSTR bstr)
{
    if (!bstr) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, bstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, bstr, -1, s.data(), len, nullptr, nullptr);
    return s;
}

static std::string wstr_to_utf8(const wchar_t* w)
{
    if (!w) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

static std::wstring utf8_to_wstr(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

static std::string hr_str(HRESULT hr)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)hr);
    return buf;
}

// ── Macro info ────────────────────────────────────────────────────────────────

struct MacroInfo {
    uint32_t    index       = 0;
    std::string name;
    std::string description;
    bool        isUsed      = false;
    bool        hasUnsupported = false;
};

// ── MacroPoolCallback ─────────────────────────────────────────────────────────

struct MacroEvent {
    BMDSwitcherMacroPoolEventType type;
    unsigned int                  index;
    std::string                   typeName;
};

class MacroCallback : public IBMDSwitcherMacroPoolCallback {
public:
    std::atomic<ULONG>      m_ref{1};
    std::vector<MacroEvent> m_events;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override
    {
        if (iid == IID_IUnknown || iid == IID_IBMDSwitcherMacroPoolCallback) {
            *ppv = static_cast<IBMDSwitcherMacroPoolCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE Notify(
        BMDSwitcherMacroPoolEventType eventType,
        unsigned int                  index,
        IBMDSwitcherTransferMacro*    /*macroTransfer*/) override
    {
        std::string name;
        switch (eventType) {
        case bmdSwitcherMacroPoolEventTypeValidChanged:       name = "ValidChanged";       break;
        case bmdSwitcherMacroPoolEventTypeNameChanged:        name = "NameChanged";        break;
        case bmdSwitcherMacroPoolEventTypeDescriptionChanged: name = "DescriptionChanged"; break;
        default: name = "Event(" + std::to_string((int)eventType) + ")"; break;
        }
        log("  [CALLBACK] MacroPool event=" + name +
            " index=" + std::to_string(index));
        m_events.push_back({eventType, index, name});
        return S_OK;
    }
};

// ── COM message pump ──────────────────────────────────────────────────────────

static void pump(int ms)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ── Main capture logic ────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    // Open log file
    std::string logPath;
    {
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string ep(exePath);
        auto slash = ep.find_last_of("\\/");
        std::string dir = (slash != std::string::npos) ? ep.substr(0, slash) : ".";
        logPath = dir + "\\captured-output.log";
    }
    g_logFile.open(logPath, std::ios::out | std::ios::trunc);

    log_raw("# ============================================================");
    log_raw("# capture-bmd - ATEM capture via BMDSwitcherAPI COM SDK");
    log_raw("# ============================================================");
    log_raw("");

    std::string ip;
    if (argc >= 2) ip = argv[1];

    // ── Step 1: COM init + create discovery ──────────────────────────────────
    log_raw("# -- STEP 1: COM init + IBMDSwitcherDiscovery -------------------");
    log_raw("");

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        log("ERROR CoInitializeEx failed " + hr_str(hr));
        return 1;
    }
    log("COM initialized (STA)");

    IBMDSwitcherDiscovery* discovery = nullptr;
    hr = CoCreateInstance(CLSID_CBMDSwitcherDiscovery, nullptr,
                          CLSCTX_ALL, __uuidof(IBMDSwitcherDiscovery),
                          reinterpret_cast<void**>(&discovery));
    if (FAILED(hr) || !discovery) {
        log("ERROR CoCreateInstance(IBMDSwitcherDiscovery) failed " + hr_str(hr));
        log("  -> Install ATEM Software Control (registers BMDSwitcherAPI64.dll)");
        CoUninitialize();
        return 1;
    }
    log("IBMDSwitcherDiscovery created OK");
    log_raw("");

    // ── Step 2: Connect ───────────────────────────────────────────────────────
    log_raw("# -- STEP 2: Connect --------------------------------------------");
    log_raw("");

    IBMDSwitcher* switcher = nullptr;
    BMDSwitcherConnectToFailure failReason;

    if (ip.empty()) {
        log("# TX  ConnectTo(nullptr)  [USB auto-detect]");
        hr = discovery->ConnectTo(nullptr, &switcher, &failReason);
    } else {
        std::wstring wip = utf8_to_wstr(ip);
        BSTR bip = SysAllocString(wip.c_str());
        log("# TX  ConnectTo(\"" + ip + "\")  [Ethernet]");
        hr = discovery->ConnectTo(bip, &switcher, &failReason);
        SysFreeString(bip);
    }

    if (FAILED(hr) || !switcher) {
        std::string reason;
        switch (failReason) {
        case bmdSwitcherConnectToFailureNoResponse:         reason = "NoResponse"; break;
        case bmdSwitcherConnectToFailureIncompatibleFirmware: reason = "IncompatibleFirmware"; break;
        default: reason = "Unknown(" + std::to_string((int)failReason) + ")"; break;
        }
        log("ERROR ConnectTo failed hr=" + hr_str(hr) + " reason=" + reason);
        if (failReason == bmdSwitcherConnectToFailureNoResponse) {
            log("  -> Check USB cable, or supply an IP address: capture.exe 192.168.10.240");
        }
        discovery->Release();
        CoUninitialize();
        return 1;
    }
    log("# RX  Connected - IBMDSwitcher obtained");
    log_raw("");

    // ── Step 3: Product info ──────────────────────────────────────────────────
    log_raw("# -- STEP 3: Product / firmware info ----------------------------");
    log_raw("");

    {
        BSTR modelName = nullptr;
        hr = switcher->GetProductName(&modelName);
        if (SUCCEEDED(hr) && modelName) {
            std::string name = bstr_to_utf8(modelName);
            log("_pin  ProductName = \"" + name + "\"");
            SysFreeString(modelName);
        } else {
            log("_pin  GetProductName failed " + hr_str(hr));
        }
    }

    // Firmware version via GetString with bmdSwitcherFirmwareVersion if available,
    // or via the IBMDSwitcherFirmwareUpdater interface.
    // NOTE: The SDK does not expose raw _ver/_top/_MeC/_mpl bytes —
    //       those UDP fields are only visible via capture.py over Ethernet.
    log("NOTE  _ver/_top/_MeC/_mpl bytes not exposed by SDK.");
    log("      Use capture.py over Ethernet to obtain those fields for run.py.");
    log_raw("");

    // ── Step 3b: Input enumeration ────────────────────────────────────────────
    log_raw("# -- STEP 3b: Input enumeration (InPr / _top source count) ------");
    log_raw("");

    struct InputInfo {
        BMDSwitcherInputId          id        = 0;
        std::string                 shortName;
        std::string                 longName;
        BMDSwitcherPortType         portType  = bmdSwitcherPortTypeExternal;
        BMDSwitcherExternalPortType extType   = bmdSwitcherExternalPortTypeSDI;
        BMDSwitcherInputAvailability avail    = (BMDSwitcherInputAvailability)0;
    };
    std::vector<InputInfo> inputs;

    {
        IBMDSwitcherInputIterator* inputIter = nullptr;
        hr = switcher->CreateIterator(__uuidof(IBMDSwitcherInputIterator),
                                      reinterpret_cast<void**>(&inputIter));
        if (FAILED(hr) || !inputIter) {
            log("WARNING CreateIterator(IBMDSwitcherInputIterator) failed " + hr_str(hr));
        } else {
            IBMDSwitcherInput* inp = nullptr;
            while (inputIter->Next(&inp) == S_OK) {
                InputInfo info;
                inp->GetInputId(&info.id);

                BSTR bShort = nullptr;
                if (SUCCEEDED(inp->GetShortName(&bShort)) && bShort) {
                    info.shortName = bstr_to_utf8(bShort);
                    SysFreeString(bShort);
                }
                BSTR bLong = nullptr;
                if (SUCCEEDED(inp->GetLongName(&bLong)) && bLong) {
                    info.longName = bstr_to_utf8(bLong);
                    SysFreeString(bLong);
                }

                inp->GetPortType(&info.portType);
                if (info.portType == bmdSwitcherPortTypeExternal)
                    inp->GetCurrentExternalPortType(&info.extType);
                inp->GetInputAvailability(&info.avail);

                char availHex[16];
                std::snprintf(availHex, sizeof(availHex), "0x%02X", (unsigned)info.avail);

                log("  InPr  id=" + std::to_string((long long)info.id) +
                    "  short=\"" + info.shortName + "\"" +
                    "  long=\"" + info.longName + "\"" +
                    "  portType=" + std::to_string((int)info.portType) +
                    "  extType=" + std::to_string((int)info.extType) +
                    "  avail=" + availHex);

                inputs.push_back(info);
                inp->Release();
            }
            inputIter->Release();
        }
        log(std::to_string(inputs.size()) + " inputs found");
    }
    log_raw("");

    // ── Step 3c: M/E block program/preview ───────────────────────────────────
    log_raw("# -- STEP 3c: M/E blocks (PrgI / PrvI source IDs) ---------------");
    log_raw("");

    struct MEInfo {
        int                index = 0;
        BMDSwitcherInputId pgmId = 0;
        BMDSwitcherInputId pvwId = 0;
    };
    std::vector<MEInfo> meBlocks;

    {
        IBMDSwitcherMixEffectBlockIterator* meIter = nullptr;
        hr = switcher->CreateIterator(__uuidof(IBMDSwitcherMixEffectBlockIterator),
                                      reinterpret_cast<void**>(&meIter));
        if (FAILED(hr) || !meIter) {
            log("WARNING CreateIterator(IBMDSwitcherMixEffectBlockIterator) failed " + hr_str(hr));
        } else {
            IBMDSwitcherMixEffectBlock* me = nullptr;
            int idx = 0;
            while (meIter->Next(&me) == S_OK) {
                MEInfo info;
                info.index = idx++;
                me->GetProgramInput(&info.pgmId);
                me->GetPreviewInput(&info.pvwId);
                log("  M/E " + std::to_string(info.index) +
                    "  program=" + std::to_string((long long)info.pgmId) +
                    "  preview=" + std::to_string((long long)info.pvwId));
                meBlocks.push_back(info);
                me->Release();
            }
            meIter->Release();
            log(std::to_string(meBlocks.size()) + " M/E blocks found");
        }
    }
    log_raw("");

    // ── Step 3d: Downstream key count ────────────────────────────────────────
    log_raw("# -- STEP 3d: Downstream key count (_top DSK byte) --------------");
    log_raw("");
    int dskCount = 0;
    {
        IBMDSwitcherDownstreamKeyIterator* dskIter = nullptr;
        hr = switcher->CreateIterator(__uuidof(IBMDSwitcherDownstreamKeyIterator),
                                      reinterpret_cast<void**>(&dskIter));
        if (FAILED(hr) || !dskIter) {
            log("WARNING CreateIterator(IBMDSwitcherDownstreamKeyIterator) failed " + hr_str(hr));
        } else {
            IBMDSwitcherDownstreamKey* dsk = nullptr;
            while (dskIter->Next(&dsk) == S_OK) { dskCount++; dsk->Release(); }
            dskIter->Release();
            log("Downstream key count = " + std::to_string(dskCount));
        }
    }
    log_raw("");

    // ── Step 4: Macro pool — enumerate macros ─────────────────────────────────
    log_raw("# -- STEP 4: IBMDSwitcherMacroPool - enumerate macros -----------");
    log_raw("");

    IBMDSwitcherMacroPool* macroPool = nullptr;
    hr = switcher->QueryInterface(__uuidof(IBMDSwitcherMacroPool),
                                  reinterpret_cast<void**>(&macroPool));
    if (FAILED(hr) || !macroPool) {
        log("ERROR QueryInterface(IBMDSwitcherMacroPool) failed " + hr_str(hr));
        switcher->Release();
        discovery->Release();
        CoUninitialize();
        return 1;
    }
    log("IBMDSwitcherMacroPool obtained");

    uint32_t maxMacros = 0;
    hr = macroPool->GetMaxCount(&maxMacros);
    if (SUCCEEDED(hr))
        log("MaxMacroCount = " + std::to_string(maxMacros));
    else
        log("GetMaxCount failed " + hr_str(hr));
    log_raw("");

    std::vector<MacroInfo> macros;
    log("# Enumerating macro slots 0.." + std::to_string(maxMacros - 1) + " ...");
    log_raw("");

    for (uint32_t i = 0; i < maxMacros; i++) {
        MacroInfo info;
        info.index = i;

        BOOL isValid = FALSE;
        hr = macroPool->IsValid(i, &isValid);
        if (FAILED(hr)) {
            log("  [" + std::to_string(i) + "] IsValid failed " + hr_str(hr));
            continue;
        }
        info.isUsed = (isValid != FALSE);
        if (!info.isUsed) continue;   // skip empty slots

        BSTR bName = nullptr;
        hr = macroPool->GetName(i, &bName);
        if (SUCCEEDED(hr) && bName) {
            info.name = bstr_to_utf8(bName);
            SysFreeString(bName);
        }

        BSTR bDesc = nullptr;
        hr = macroPool->GetDescription(i, &bDesc);
        if (SUCCEEDED(hr) && bDesc) {
            info.description = bstr_to_utf8(bDesc);
            SysFreeString(bDesc);
        }

        BOOL hasUnsup = FALSE;
        hr = macroPool->HasUnsupportedOps(i, &hasUnsup);
        if (SUCCEEDED(hr)) info.hasUnsupported = (hasUnsup != FALSE);

        log("  MPrp  slot=" + std::to_string(i) +
            "  name=\"" + info.name + "\"" +
            "  desc=\"" + info.description + "\"" +
            "  unsupported=" + (info.hasUnsupported ? "1" : "0"));

        macros.push_back(info);
    }
    log_raw("");
    log(std::to_string(macros.size()) + " macros found");
    log_raw("");

    // ── Step 5: Register callback ─────────────────────────────────────────────
    log_raw("# -- STEP 5: Register IBMDSwitcherMacroPoolCallback -------------");
    log_raw("");

    MacroCallback* cb = new MacroCallback();
    hr = macroPool->AddCallback(cb);
    if (FAILED(hr))
        log("WARNING AddCallback failed " + hr_str(hr));
    else
        log("MacroPoolCallback registered");
    log_raw("");

    // ── Step 6: IBMDSwitcherMacroControl ──────────────────────────────────────
    log_raw("# -- STEP 6: IBMDSwitcherMacroControl - run/stop probes ---------");
    log_raw("");

    IBMDSwitcherMacroControl* macroCtrl = nullptr;
    hr = switcher->QueryInterface(__uuidof(IBMDSwitcherMacroControl),
                                  reinterpret_cast<void**>(&macroCtrl));
    if (FAILED(hr) || !macroCtrl) {
        log("ERROR QueryInterface(IBMDSwitcherMacroControl) failed " + hr_str(hr));
    } else {
        log("IBMDSwitcherMacroControl obtained");

        // Read idle run-status before any run
        {
            BMDSwitcherMacroRunStatus runStatus = bmdSwitcherMacroRunStatusIdle;
            BOOL loop = FALSE;
            unsigned int runIndex = 0;
            hr = macroCtrl->GetRunStatus(&runStatus, &loop, &runIndex);
            if (SUCCEEDED(hr)) {
                log("  MRPr  idle  runStatus=" + std::to_string((int)runStatus) +
                    "  loop=" + std::to_string((int)loop) +
                    "  index=" + std::to_string(runIndex));
                log("  NOTE  idle runStatus value = " + std::to_string((int)runStatus) +
                    "  (use this as the 'idle' sentinel in run.py)");
                log("  NOTE  idle index value = " + std::to_string(runIndex) +
                    "  (0xFFFF = no macro running)");
            } else {
                log("  GetRunStatus failed " + hr_str(hr));
            }
        }
        log_raw("");

        // Run each macro (up to first 5 used slots)
        int probed = 0;
        for (auto& m : macros) {
            if (probed >= 5) break;
            log("# TX  RunMacro(" + std::to_string(m.index) +
                ")  name=\"" + m.name + "\"");

            hr = macroCtrl->Run(m.index);
            if (FAILED(hr)) {
                log("  Run(" + std::to_string(m.index) + ") failed " + hr_str(hr));
                continue;
            }

            // Pump 200 ms - wait for RunStatusChanged callback
            pump(200);

            BMDSwitcherMacroRunStatus runStatus = bmdSwitcherMacroRunStatusIdle;
            BOOL loop = FALSE;
            unsigned int runIndex = 0;
            hr = macroCtrl->GetRunStatus(&runStatus, &loop, &runIndex);
            if (SUCCEEDED(hr)) {
                log("  MRPr  running  runStatus=" + std::to_string((int)runStatus) +
                    "  loop=" + std::to_string((int)loop) +
                    "  index=" + std::to_string(runIndex));
            }

            // Stop
            log("# TX  StopMacro");
            hr = macroCtrl->StopRunning();
            if (FAILED(hr))
                log("  StopRunning failed " + hr_str(hr));

            pump(200);

            hr = macroCtrl->GetRunStatus(&runStatus, &loop, &runIndex);
            if (SUCCEEDED(hr)) {
                log("  MRPr  stopped  runStatus=" + std::to_string((int)runStatus) +
                    "  loop=" + std::to_string((int)loop) +
                    "  index=" + std::to_string(runIndex));
            }
            log_raw("");
            probed++;
        }

        macroCtrl->Release();
    }

    // ── Step 7: Keepalive observation (10 s passive) ──────────────────────────
    log_raw("# -- STEP 7: Passive observation (10 s) -------------------------");
    log_raw("");
    log("Observing callbacks for 10 seconds (keepalive / spontaneous events) ...");
    pump(10000);
    log("Observation complete. Callbacks received: " + std::to_string(cb->m_events.size()));
    log_raw("");

    // ── SUMMARY ───────────────────────────────────────────────────────────────
    log_raw("# ================================================================");
    log_raw("# SUMMARY - for updating run.py");
    log_raw("# ================================================================");
    log_raw("");
    log_raw("# Macros -> update macros.tsv and default_macros in run.py:");
    for (auto& m : macros) {
        log_raw("# MPrp  [" + std::to_string(m.index) + "]  \"" +
                m.name + "\"  desc=\"" + m.description + "\"");
    }
    log_raw("");

    log_raw("# Inputs -> build_input_properties() + _top source_count in run.py:");
    log_raw("# _top  source_count=" + std::to_string(inputs.size()) +
            "  dsk_count=" + std::to_string(dskCount));
    for (auto& inp : inputs) {
        char availHex[16];
        std::snprintf(availHex, sizeof(availHex), "0x%02X", (unsigned)inp.avail);
        log_raw("# InPr  id=" + std::to_string((long long)inp.id) +
                "  short=\"" + inp.shortName + "\"" +
                "  long=\"" + inp.longName + "\"" +
                "  portType=" + std::to_string((int)inp.portType) +
                "  extType=" + std::to_string((int)inp.extType) +
                "  avail=" + availHex);
    }
    log_raw("");

    log_raw("# M/E program/preview -> build_program_input() / build_preview_input() in run.py:");
    for (auto& me : meBlocks) {
        log_raw("# PrgI  me=" + std::to_string(me.index) +
                "  source=" + std::to_string((long long)me.pgmId));
        log_raw("# PrvI  me=" + std::to_string(me.index) +
                "  source=" + std::to_string((long long)me.pvwId));
    }
    log_raw("");

    log_raw("# MRPr idle runStatus value  -> build_macro_run_status() idle sentinel");
    log_raw("# MRPr idle index value = 65535 (0xFFFF = no macro running)");
    log_raw("");
    log_raw("# NOTE: _ver/_top raw bytes, _pin model byte, _MeC, _mpl, VidM");
    log_raw("# are NOT exposed by the SDK. Use capture.py over Ethernet for those.");
    log_raw("");

    // ── Cleanup ───────────────────────────────────────────────────────────────
    macroPool->RemoveCallback(cb);
    cb->Release();
    macroPool->Release();
    switcher->Release();
    discovery->Release();
    CoUninitialize();

    log("Done. Log written to: " + logPath);
    g_logFile.close();
    return 0;
}
