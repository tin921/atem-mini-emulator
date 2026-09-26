// Inputs: enumeration, properties, names.
#include "tests_common.h"

namespace {

QString getLong(IBMDSwitcherInput* in) {
    BSTR b = nullptr;
    return SUCCEEDED(SDK_CALL(IBMDSwitcherInput, in, GetLongName, &b)) ? takeBstr(b) : QString();
}

QString getShort(IBMDSwitcherInput* in) {
    BSTR b = nullptr;
    return SUCCEEDED(SDK_CALL(IBMDSwitcherInput, in, GetShortName, &b)) ? takeBstr(b) : QString();
}

void setName(Ctx& c, bool longName, const QString& value, bool good) {
    c.needConnection();
    IBMDSwitcherInput* in = c.s.input(1);
    if (!in) c.skip("no input 1");
    c.observe("value", value);
    BSTR b = makeBstr(value);
    HRESULT hr = longName ? SDK_CALL(IBMDSwitcherInput, in, SetLongName, b)
                          : SDK_CALL(IBMDSwitcherInput, in, SetShortName, b);
    SysFreeString(b);
    c.hr("set", hr);
    c.settle();
    QString back = longName ? getLong(in) : getShort(in);
    c.observe("readback", back);
    BOOL def = FALSE;
    c.hr("areNamesDefault.hr", SDK_CALL(IBMDSwitcherInput, in, AreNamesDefault, &def));
    c.observe("areNamesDefault", def != FALSE);
    if (good) {
        c.expect(SUCCEEDED(hr), "set returned " + hrText(hr));
        c.expect(back == value, QString("read back \"%1\"").arg(back));
    }
}

} // namespace

void registerInputTests() {
    addTest("input.list", "Enumerate inputs", [](Ctx& c) {
        c.needConnection();
        QJsonArray ids;
        for (auto* in : c.s.inputs) {
            BMDSwitcherInputId id = -1;
            SDK_CALL(IBMDSwitcherInput, in, GetInputId, &id);
            ids.append(static_cast<double>(id));
        }
        c.observe("count", static_cast<int>(c.s.inputs.size()));
        c.observe("ids", ids);
    });

    addTest("input.properties", "Every property of every input", [](Ctx& c) {
        c.needConnection();
        QJsonObject all;
        for (auto* in : c.s.inputs) {
            BMDSwitcherInputId id = -1;
            SDK_CALL(IBMDSwitcherInput, in, GetInputId, &id);
            QJsonObject o;
            BMDSwitcherPortType port{};
            if (SUCCEEDED(SDK_CALL(IBMDSwitcherInput, in, GetPortType, &port))) o["portType"] = js(port);
            BMDSwitcherInputAvailability avail{};
            if (SUCCEEDED(SDK_CALL(IBMDSwitcherInput, in, GetInputAvailability, &avail)))
                o["availability"] = static_cast<double>(avail);
            o["shortName"] = getShort(in);
            o["longName"] = getLong(in);
            BOOL b = FALSE;
            if (SUCCEEDED(SDK_CALL(IBMDSwitcherInput, in, AreNamesDefault, &b))) o["namesDefault"] = b != FALSE;
            if (SUCCEEDED(SDK_CALL(IBMDSwitcherInput, in, IsProgramTallied, &b))) o["programTallied"] = b != FALSE;
            if (SUCCEEDED(SDK_CALL(IBMDSwitcherInput, in, IsPreviewTallied, &b))) o["previewTallied"] = b != FALSE;
            BMDSwitcherExternalPortType ext{};
            HRESULT hr = SDK_CALL(IBMDSwitcherInput, in, GetAvailableExternalPortTypes, &ext);
            o["availableExternalPortTypes"] = SUCCEEDED(hr) ? QJsonValue(static_cast<double>(ext)) : QJsonValue(hrText(hr));
            hr = SDK_CALL(IBMDSwitcherInput, in, GetCurrentExternalPortType, &ext);
            o["currentExternalPortType"] = SUCCEEDED(hr) ? QJsonValue(static_cast<double>(ext)) : QJsonValue(hrText(hr));
            all[QString::number(id)] = o;
        }
        c.observe("inputs", all);
    });

    addTest("input.name.long.good", "Camera 1 long name: \"Sweep Cam 1\"", [](Ctx& c) { setName(c, true, "Sweep Cam 1", true); });
    addTest("input.name.long.max", "Camera 1 long name: 20 characters", [](Ctx& c) { setName(c, true, "ABCDEFGHIJKLMNOPQRST", false); });
    addTest("input.name.long.toolong", "Camera 1 long name: 40 characters (bad)", [](Ctx& c) {
        setName(c, true, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcd", false);
    });
    addTest("input.name.long.empty", "Camera 1 long name: empty (bad)", [](Ctx& c) { setName(c, true, "", false); });
    addTest("input.name.long.unicode", "Camera 1 long name: non-ASCII", [](Ctx& c) {
        setName(c, true, QString::fromUtf8("Kamera Ä 講台"), false);
    });
    addTest("input.name.short.good", "Camera 1 short name: \"SWP1\"", [](Ctx& c) { setName(c, false, "SWP1", true); });
    addTest("input.name.short.toolong", "Camera 1 short name: 8 characters (bad)", [](Ctx& c) { setName(c, false, "TOOLONG1", false); });
    addTest("input.name.reset", "Reset Camera 1 names to default", [](Ctx& c) {
        c.needConnection();
        IBMDSwitcherInput* in = c.s.input(1);
        if (!in) c.skip("no input 1");
        c.hr("reset", SDK_CALL(IBMDSwitcherInput, in, ResetNames));
        c.settle();
        c.observe("longName", getLong(in));
        c.observe("shortName", getShort(in));
        BOOL def = FALSE;
        SDK_CALL(IBMDSwitcherInput, in, AreNamesDefault, &def);
        c.observe("areNamesDefault", def != FALSE);
        c.expect(def != FALSE, "names not default after ResetNames");
    });
}
