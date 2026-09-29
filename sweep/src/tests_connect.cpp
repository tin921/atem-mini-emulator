// Connection and switcher-level properties.
#include "tests_common.h"
#include "wireproxy.h"

#include <QElapsedTimer>
#include <map>

namespace {

void connectOther(Ctx& c, const QString& address) {
    Switcher other;
    BMDSwitcherConnectToFailure fail = bmdSwitcherConnectToFailureNoResponse;
    QElapsedTimer t;
    t.start();
    HRESULT hr = other.connect(address, &fail);
    // When nothing answers, the SDK falls back to an ATEM on this PC's USB and
    // connects to that (read-only here: connected, then disconnected). The
    // outcome depends on what is plugged in, not on the switcher under test:
    // recorded, not compared.
    c.observe("connect (informational)", hrText(hr));
    if (FAILED(hr)) c.observe("failReason (informational)", fourcc(static_cast<uint32_t>(fail)));
    c.observe("connected (informational)", other.connected());
    c.observe("note (informational)", other.connected() ? "connected through the SDK's USB fallback" : "no connection");
    c.observe("ms (informational)", static_cast<double>(t.elapsed()));
}

const std::vector<std::pair<BMDSwitcherVideoMode, const char*>> kVideoModes = {
    { bmdSwitcherVideoMode525i5994NTSC, "525i5994" }, { bmdSwitcherVideoMode625i50PAL, "625i50" },
    { bmdSwitcherVideoMode720p50, "720p50" }, { bmdSwitcherVideoMode720p5994, "720p5994" },
    { bmdSwitcherVideoMode720p60, "720p60" }, { bmdSwitcherVideoMode1080i50, "1080i50" },
    { bmdSwitcherVideoMode1080i5994, "1080i5994" }, { bmdSwitcherVideoMode1080i60, "1080i60" },
    { bmdSwitcherVideoMode1080p2398, "1080p2398" }, { bmdSwitcherVideoMode1080p24, "1080p24" },
    { bmdSwitcherVideoMode1080p25, "1080p25" }, { bmdSwitcherVideoMode1080p2997, "1080p2997" },
    { bmdSwitcherVideoMode1080p30, "1080p30" }, { bmdSwitcherVideoMode1080p50, "1080p50" },
    { bmdSwitcherVideoMode1080p5994, "1080p5994" }, { bmdSwitcherVideoMode1080p60, "1080p60" },
    { bmdSwitcherVideoMode4KHDp2398, "4Kp2398" },
};

} // namespace

void registerConnectTests() {
    addTest("connect.discovery", "Discovery object is created", [](Ctx& c) {
        c.observe("created", c.s.discovery != nullptr);
        c.expect(c.s.discovery != nullptr, "CBMDSwitcherDiscovery not registered (ATEM Software Control installed?)");
    });

    addTest("connect.bad-address", "ConnectTo a malformed address", [](Ctx& c) {
        connectOther(c, "not-an-address");
    });

    addTest("connect.unreachable", "ConnectTo an address nobody answers", [](Ctx& c) {
        connectOther(c, "192.0.2.1");   // TEST-NET-1: never routed
    });

    addTest("connect.main", "Connect (keeps this connection for the sweep)", [](Ctx& c) {
        size_t wireMark = c.wire ? c.wire->mark() : 0;
        BMDSwitcherConnectToFailure fail = bmdSwitcherConnectToFailureNoResponse;
        QElapsedTimer t;
        t.start();
        HRESULT hr = c.s.connect(c.opt.connectAddress, &fail);
        c.hr("connect", hr);
        c.observe("ms (informational)", static_cast<double>(t.elapsed()));
        if (FAILED(hr)) {
            c.observe("failReason", fourcc(static_cast<uint32_t>(fail)));
            c.expect(false, "cannot connect: " + fourcc(static_cast<uint32_t>(fail)));
            return;
        }
        BSTR name = nullptr;
        if (SUCCEEDED(SDK_CALL(IBMDSwitcher, c.s.sw, GetProductName, &name)))
            c.observe("productName", takeBstr(name));
        c.observe("interfaces", QJsonObject{
            { "mixEffect", c.s.me != nullptr }, { "transition", c.s.trans != nullptr },
            { "key", c.s.key != nullptr }, { "keyFly", c.s.fly != nullptr }, { "keyDVE", c.s.dve != nullptr },
            { "downstreamKey", c.s.dsk != nullptr }, { "macroPool", c.s.pool != nullptr },
            { "macroControl", c.s.macros != nullptr }, { "inputs", static_cast<int>(c.s.inputs.size()) },
        });

        // The initial state dump, field by field: exactly what an emulator must send.
        if (c.wire) {
            std::map<QString, int> counts;
            QString version;
            for (const auto& p : c.wire->since(wireMark)) {
                if (p.toDevice) continue;
                for (const auto& f : p.fields()) {
                    ++counts[f.name];
                    if (f.name == "_ver") version = QString::fromLatin1(f.data.toHex());
                }
            }
            QJsonObject dump;
            for (const auto& [k, v] : counts) dump[k] = v;
            c.observe("dumpFields", dump);
            c.observe("protocolVersion", version);
        }
    });

    addTest("connect.second-client", "A second connection at the same time", [](Ctx& c) {
        c.needConnection();
        if (c.opt.target.isEmpty()) c.skip("USB allows one control connection at a time");
        connectOther(c, c.opt.connectAddress);
        BSTR name = nullptr;
        c.observe("firstStillWorks", SUCCEEDED(SDK_CALL(IBMDSwitcher, c.s.sw, GetProductName, &name)));
        takeBstr(name);
    });

    addTest("switcher.info", "Switcher properties (video modes, power, time code)", [](Ctx& c) {
        c.needConnection();
        IBMDSwitcher* sw = c.s.sw;
        BSTR name = nullptr;
        if (SUCCEEDED(SDK_CALL(IBMDSwitcher, sw, GetProductName, &name))) c.observe("productName", takeBstr(name));
        BMDSwitcherVideoMode mode = read<BMDSwitcherVideoMode>(c, "videoMode", [&](BMDSwitcherVideoMode* v) {
            return SDK_CALL(IBMDSwitcher, sw, GetVideoMode, v);
        });

        QJsonObject supported;
        for (const auto& [m, label] : kVideoModes) {
            BOOL ok = FALSE;
            HRESULT hr = SDK_CALL(IBMDSwitcher, sw, DoesSupportVideoMode, m, &ok);
            supported[label] = SUCCEEDED(hr) ? QJsonValue(ok != FALSE) : QJsonValue(hrText(hr));
        }
        c.observe("supportsVideoMode", supported);

        read<BMDSwitcherVideoMode>(c, "downConvertedHDVideoMode", [&](BMDSwitcherVideoMode* v) {
            return SDK_CALL(IBMDSwitcher, sw, GetDownConvertedHDVideoMode, mode, v);
        });
        read<BMDSwitcherVideoMode>(c, "multiViewVideoMode", [&](BMDSwitcherVideoMode* v) {
            return SDK_CALL(IBMDSwitcher, sw, GetMultiViewVideoMode, mode, v);
        });
        read<BMDSwitcherPowerStatus>(c, "powerStatus", [&](BMDSwitcherPowerStatus* v) {
            return SDK_CALL(IBMDSwitcher, sw, GetPowerStatus, v);
        });
        read<BOOL>(c, "supportsAutoVideoMode", [&](BOOL* v) { return SDK_CALL(IBMDSwitcher, sw, DoesSupportAutoVideoMode, v); });
        read<BOOL>(c, "autoVideoMode", [&](BOOL* v) { return SDK_CALL(IBMDSwitcher, sw, GetAutoVideoMode, v); });
        read<BOOL>(c, "timeCodeLocked", [&](BOOL* v) { return SDK_CALL(IBMDSwitcher, sw, GetTimeCodeLocked, v); });
        read<BMDSwitcherTimeCodeMode>(c, "timeCodeMode", [&](BMDSwitcherTimeCodeMode* v) {
            return SDK_CALL(IBMDSwitcher, sw, GetTimeCodeMode, v);
        });

        unsigned char h = 0, m = 0, s = 0, f = 0;
        BOOL drop = FALSE;
        HRESULT hr = SDK_CALL(IBMDSwitcher, sw, GetTimeCode, &h, &m, &s, &f, &drop);
        c.hr("getTimeCode", hr);
        c.observe("timeCodeDropFrame", drop != FALSE);   // the time itself keeps moving: not recorded
    });
}
