#include "MainWindow.h"
#include "Logger.h"
#include "vcam_shared.h"
#include "commands.h"
#include <windows.h>
#include <QApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QStatusBar>
#include <QFrame>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMessageBox>
#include <cmath>

// ── Sources ───────────────────────────────────────────────────────────────────
static const struct { quint16 id; const char* hw; const char* full; } kSrc[] = {
    { Atem::SRC_BLACK, "BLK",  "Black"    },
    { Atem::SRC_CAM1,  "1",    "Camera 1" },
    { Atem::SRC_CAM2,  "2",    "Camera 2" },
    { Atem::SRC_CAM3,  "3",    "Camera 3" },
    { Atem::SRC_CAM4,  "4",    "Camera 4" },
    { Atem::SRC_BARS,  "BARS", "Bars"     },
};
constexpr int kN = 6;
static const QColor kCamColors[] = {
    {30,100,200}, {20,160,70}, {210,80,30}, {150,30,170}
};

// ── SMPTE colour bars source ──────────────────────────────────────────────────
class BarsSource : public InputSource {
public:
    BarsSource() : InputSource() {
        static const QColor cols[] = {
            {192,192,192}, {192,192,0}, {0,192,192}, {0,192,0},
            {192,0,192},   {192,0,0},   {0,0,192}
        };
        m_img = QImage(1280, 720, QImage::Format_RGB32);
        QPainter p(&m_img);
        int n = 7, w = 1280 / n;
        for (int i = 0; i < n; ++i)
            p.fillRect(i*w, 0, (i==n-1) ? 1280-i*w : w, 720, cols[i]);
        p.end();
    }
    QImage  currentFrame() const override { return m_img; }
    QString label()        const override { return "Bars"; }
private:
    QImage m_img;
};

// ── Stylesheet ────────────────────────────────────────────────────────────────
static const char* kSS = R"(
* { font-family:"Segoe UI",Arial,sans-serif; font-size:11px; }
QMainWindow,QWidget   { background:#111; color:#ccc; }
QCheckBox             { color:#bbb; spacing:6px; }
QCheckBox::indicator  { width:13px; height:13px; background:#1a1a1a;
                         border:1px solid #333; border-radius:3px; }
QCheckBox::indicator:checked { background:#1a5acc; border-color:#3377ff; }
QLineEdit             { background:#161616; border:1px solid #2a2a2a; color:#ccc;
                         padding:2px 5px; border-radius:4px; }
QLineEdit:focus       { border-color:#1a5acc; }
QSpinBox {
    background:#161616; border:1px solid #2a2a2a; color:#ccc;
    padding:2px 5px; border-radius:4px; font-size:12px; }
QSpinBox:focus { border-color:#1a5acc; }
QSpinBox::up-button,QSpinBox::down-button { width:0; height:0; border:0; }
QListWidget           { background:#141414; border:1px solid #222; color:#bbb;
                         border-radius:4px; outline:0; }
QListWidget::item     { padding:3px 8px; border-radius:3px; }
QListWidget::item:selected { background:#0c1e42; color:#88aaff; }
QListWidget::item:hover    { background:#1a1a1a; }
QTextEdit             { background:#0d0d0d; border:0; color:#559955;
                         font-family:Consolas,"Courier New",monospace; font-size:12px; }
QLabel                { color:#999; }
QScrollBar:vertical   { background:#0d0d0d; width:5px; border:0; }
QScrollBar::handle:vertical { background:#252525; border-radius:3px; min-height:16px; }
QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height:0; }
QScrollArea           { border:0; }
QStatusBar            { background:#0a0a0a; color:#333; font-size:10px;
                         border-top:1px solid #181818; }
)";

// ── Button helpers ────────────────────────────────────────────────────────────

static QPushButton* makeSmallBtn(const char* lbl,
                                  const char* bg, const char* bd,
                                  const char* fg, const char* hov)
{
    auto* b = new QPushButton(lbl);
    b->setStyleSheet(QString(
        "QPushButton{background:%1;border:1px solid %2;border-radius:4px;"
        "color:%3;font-size:10px;font-weight:600;padding:3px 10px;}"
        "QPushButton:hover{background:%4;}").arg(bg,bd,fg,hov));
    return b;
}

// ── Section header bar ────────────────────────────────────────────────────────

/*static*/ QWidget* MainWindow::sectionHeader(const char* title)
{
    auto* bar = new QWidget;
    bar->setFixedHeight(22);
    bar->setStyleSheet("QWidget{background:#0a0a0a;border-top:1px solid #1a1a1a;}");
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(10, 0, 10, 0);
    auto* l = new QLabel(title);
    l->setStyleSheet("color:#bbb;font-size:9px;font-weight:700;"
                     "letter-spacing:2px;background:transparent;border:0;");
    h->addWidget(l); h->addStretch();
    return bar;
}

// ── Spinbox factory ───────────────────────────────────────────────────────────

/*static*/ QSpinBox* MainWindow::makeSpin(int lo, int hi, int val, const QString& suffix)
{
    auto* s = new QSpinBox;
    s->setRange(lo, hi);
    s->setValue(val);
    s->setFixedWidth(78);
    s->setFocusPolicy(Qt::WheelFocus);
    if (!suffix.isEmpty()) s->setSuffix(suffix);
    return s;
}

// ── Macro persistence ─────────────────────────────────────────────────────────
//
// The macro pool lives in the switcher (the emulator core), like on a real
// ATEM: clients see it, run it, record into it. The window saves it with the
// extras the ATEM has no place for (camera pictures, size lock, rotation,
// opacity) in macros.json:
//   { "version": 2, "macros": [ { index, name, description, steps, extras } ] }
// A version-1 file (an array of actions + snapshot) is converted on load and
// kept as macros-v1.json.

/*static*/ QString MainWindow::macroDataPath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dir);
    return dir + "/macros.json";
}

static QJsonArray inputsToJson(const Atem::InputSnap (&inputs)[4])
{
    QJsonArray a;
    for (const auto& in : inputs)
        a.append(QJsonObject{{"mode",(int)in.mode},{"argb",(qint64)in.argb},{"path",in.path}});
    return a;
}

static void inputsFromJson(const QJsonArray& a, Atem::InputSnap (&inputs)[4])
{
    for (int j = 0; j < 4 && j < a.size(); ++j) {
        QJsonObject o = a[j].toObject();
        inputs[j].mode = (Atem::InputMode)o["mode"].toInt();
        inputs[j].argb = (quint32)(qint64)o["argb"].toDouble();
        inputs[j].path = o["path"].toString();
    }
}

void MainWindow::saveMacros()
{
    if (m_options.reference) return;
    QJsonArray arr;
    for (int i = 0; i < m_device.macroCount(); ++i) {
        const emu::Macro& mac = m_device.macro(i);
        if (!mac.used) continue;
        const MacroExtras& x = m_macroExtras[i];
        arr.append(QJsonObject{
            {"index",       i},
            {"name",        mac.name},
            {"description", mac.description},
            {"steps",       mac.opsToJson()},
            {"extras", QJsonObject{
                {"captured", x.captured},
                {"lockSize", x.lockSize},
                {"rotation", x.rotation},
                {"opacity",  x.opacity},
                {"inputs",   inputsToJson(x.inputs)}}}});
    }
    QFile f(macroDataPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(QJsonObject{{"version", 2}, {"macros", arr}}).toJson());
}

// A version-1 macro (snapshot + actions) as switcher commands.
static QList<emu::MacroOp> convertV1Macro(const QJsonObject& o, double frameMs)
{
    using namespace emu::cmd;
    QList<emu::MacroOp> ops;
    auto op = [&](const char* name, const QByteArray& data) { ops.append({emu::MacroOp::Command, name, data, 0}); };
    QJsonObject sj = o["snapshot"].toObject();
    if (sj["captured"].toBool()) {
        QJsonObject dj = sj["dve"].toObject();
        op("CPgI", programInput(0, (quint16)sj["programSource"].toInt()));
        op("CKTp", keyType(0, 0, 3));
        op("CKeF", keyFill(0, 0, (quint16)dj["fillSrc"].toInt()));
        DveParams p;
        p.sizeX = dj["sizeX"].toInt() / 1000.0;
        p.sizeY = dj["sizeY"].toInt() / 1000.0;
        p.positionX = dj["posX"].toInt() / 1000.0;
        p.positionY = dj["posY"].toInt() / 1000.0;
        p.border = dj["border"].toInt() > 0;
        p.borderWidth = dj["border"].toInt() / 3.125;
        float h, sat, l;
        QColor::fromRgba((quint32)(qint64)dj["borderArgb"].toDouble()).getHslF(&h, &sat, &l);
        p.borderHue = h < 0 ? 0 : h * 360;
        p.borderSaturation = sat;
        p.borderLuma = l;
        p.maskTop = dj["cropTop"].toInt() * 0.18;
        p.maskBottom = dj["cropBottom"].toInt() * 0.18;
        p.maskLeft = dj["cropLeft"].toInt() * 0.32;
        p.maskRight = dj["cropRight"].toInt() * 0.32;
        p.masked = p.maskTop > 0 || p.maskBottom > 0 || p.maskLeft > 0 || p.maskRight > 0;
        op("CKDV", dve(0, 0, SizeX | SizeY | PositionX | PositionY | Border | BorderWidth | BorderHue |
                             BorderSaturation | BorderLuma | Masked | MaskTop | MaskBottom | MaskLeft | MaskRight, p));
        op("CKOn", keyOnAir(0, 0, dj["enabled"].toBool()));
    }
    for (const QJsonValue& av : o["actions"].toArray()) {
        int type = av.toObject()["type"].toInt(), param = av.toObject()["param"].toInt();
        if (type == 0) op("CPgI", programInput(0, (quint16)param));          // SwitchProgram
        else if (type == 1) op("CPvI", previewInput(0, (quint16)param));     // SwitchPreview
        else if (type == 2) op("CKOn", keyOnAir(0, 0, param != 0));          // KeyerEnable
        else if (type == 3) ops.append({emu::MacroOp::Wait, {}, {}, std::max(1, (int)std::lround(param / frameMs))});
    }
    return ops;
}

bool MainWindow::loadMacros()
{
    QFile f(macroDataPath());
    if (!f.open(QIODevice::ReadOnly)) return false;
    QByteArray raw = f.readAll();
    f.close();
    QJsonDocument doc = QJsonDocument::fromJson(raw);

    QJsonArray entries;
    bool v1 = doc.isArray();
    if (v1) {
        entries = doc.array();
        QFile::remove(QFileInfo(f).absolutePath() + "/macros-v1.json");
        QFile::copy(macroDataPath(), QFileInfo(f).absolutePath() + "/macros-v1.json");
    } else if (doc.isObject() && doc.object()["version"].toInt() == 2) {
        entries = doc.object()["macros"].toArray();
    } else {
        return false;
    }

    // The saved pool replaces the profile's macros.
    for (int i = 0; i < m_device.macroCount(); ++i) {
        if (m_device.macro(i).used) m_device.setMacro(i, emu::Macro());
        m_macroExtras[i] = MacroExtras();
    }
    for (const QJsonValue& v : entries) {
        QJsonObject o = v.toObject();
        int i = o["index"].toInt();
        if (i < 0 || i >= m_device.macroCount()) continue;
        if (v1 && !o["isUsed"].toBool()) continue;
        emu::Macro mac;
        mac.used = true;
        mac.name = o["name"].toString();
        mac.description = o["description"].toString();
        MacroExtras& x = m_macroExtras[i];
        if (v1) {
            mac.ops = convertV1Macro(o, m_device.frameIntervalMs());
            QJsonObject sj = o["snapshot"].toObject();
            QJsonObject dj = sj["dve"].toObject();
            x.captured = sj["captured"].toBool();
            x.lockSize = sj["lockSize"].toBool(true);
            x.rotation = dj["rotation"].toInt() / 100;
            x.opacity = dj.contains("opacity") ? dj["opacity"].toInt() : 100;
            inputsFromJson(sj["inputs"].toArray(), x.inputs);
        } else {
            mac.ops = emu::Macro::opsFromJson(o["steps"].toArray());
            QJsonObject xj = o["extras"].toObject();
            x.captured = xj["captured"].toBool();
            x.lockSize = xj["lockSize"].toBool(true);
            x.rotation = xj["rotation"].toInt();
            x.opacity = xj.contains("opacity") ? xj["opacity"].toInt() : 100;
            inputsFromJson(xj["inputs"].toArray(), x.inputs);
        }
        m_device.setMacro(i, mac);
    }
    if (v1) saveMacros();
    return true;
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    saveMacros();
    QMainWindow::closeEvent(e);
}

// ── Macro thumb path ──────────────────────────────────────────────────────────

/*static*/ QString MainWindow::macroThumbPath(int slot)
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                  + "/macros";
    QDir().mkpath(dir);
    return dir + QString("/thumb_%1.jpg").arg(slot);
}

// ── Macro list delegate ───────────────────────────────────────────────────────

class MacroDelegate : public QStyledItemDelegate
{
    static constexpr int kW = 64, kH = 36, kPad = 5;
public:
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override
    { return QSize(0, kH + kPad * 2); }

    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& idx) const override
    {
        p->save();
        if (opt.state & QStyle::State_Selected)
            p->fillRect(opt.rect, QColor(12, 30, 66));
        else if (opt.state & QStyle::State_MouseOver)
            p->fillRect(opt.rect, QColor(26, 26, 26));

        QRect r = opt.rect.adjusted(kPad, kPad, -kPad, -kPad);
        QPixmap thumb = idx.data(Qt::DecorationRole).value<QIcon>().pixmap(kW, kH);
        QRect tR(r.left(), r.top(), kW, kH);
        if (!thumb.isNull())
            p->drawPixmap(tR, thumb.scaled(tR.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        else {
            p->fillRect(tR, QColor(18, 18, 18));
            p->setPen(QColor(40, 40, 40));
            p->drawRect(tR.adjusted(0,0,-1,-1));
        }

        QRect textR = r.adjusted(kW + kPad, 0, 0, 0);
        QFont nf = p->font(); nf.setBold(true); nf.setPointSize(9); p->setFont(nf);
        p->setPen((opt.state & QStyle::State_Selected) ? QColor(136,170,255) : QColor(200,200,200));
        p->drawText(textR, Qt::AlignLeft | Qt::AlignTop, idx.data(Qt::DisplayRole).toString());

        QFont df = nf; df.setBold(false); df.setPointSize(8); p->setFont(df);
        p->setPen(QColor(90,90,90));
        p->drawText(textR.adjusted(0,15,0,0), Qt::AlignLeft | Qt::AlignTop,
                    idx.data(Qt::UserRole+1).toString().left(40));
        p->restore();
    }
};

// ── Constructor ───────────────────────────────────────────────────────────────

MainWindow::MainWindow(const EmulatorOptions& options, QWidget* parent)
    : QMainWindow(parent)
    , m_options(options)
    , m_server(&m_device)
{
    setWindowTitle(m_options.reference ? "ATEM Mini Emulator (reference)" : "ATEM Mini Emulator");
    setStyleSheet(kSS);
    resize(1080, 640);
    setMinimumSize(900, 520);

    for (int i = 0; i < 4; ++i) {
        m_sources[i]      = new SolidColorSource(kCamColors[i], this);
        m_videoSources[i] = new VideoFileSource(this);
        m_photoSources[i] = new StaticImageSource(this);
    }

    // The switcher: the state recorded from a real ATEM Mini.
    QString error;
    if (!m_device.load(m_options.profileDir, &error)) {
        QMessageBox::critical(nullptr, "ATEM Mini Emulator", "Cannot load the switcher profile:\n" + error);
        return;
    }
    m_ready = true;
    m_macroExtras.resize(m_device.macroCount());
    if (!m_options.reference) loadMacros();

    buildUi();

    connect(&m_device, &emu::Device::log, this, &MainWindow::onLogMessage);
    connect(&m_server, &emu::Server::log, this, &MainWindow::onLogMessage);
    connect(&m_server, &emu::Server::clientCountChanged, this, &MainWindow::onClientCount);
    connect(&m_server, &emu::Server::commandsReceived, this, [this](const QString& client, const QStringList& cmds) {
        onLogMessage(client + "  \u2192  " + cmds.join(' '));
    });

    // Every change (from this window, a client, a running transition or
    // macro) refreshes the window once per event-loop pass.
    m_syncTimer.setSingleShot(true);
    m_syncTimer.setInterval(0);
    connect(&m_syncTimer, &QTimer::timeout, this, &MainWindow::syncFromDevice);
    connect(&m_device, &emu::Device::stateChanged, &m_syncTimer, qOverload<>(&QTimer::start));
    connect(&m_device, &emu::Device::macroStarted,  this, &MainWindow::onMacroStarted);
    connect(&m_device, &emu::Device::macroFinished, this, &MainWindow::onMacroFinished);
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(500);
    connect(&m_saveTimer, &QTimer::timeout, this, [this]{ saveMacros(); syncMacroList(); });
    connect(&m_device, &emu::Device::macroPoolChanged, &m_saveTimer, qOverload<>(&QTimer::start));

    m_refreshTimer = new QTimer(this);
    connect(m_refreshTimer, &QTimer::timeout, this, &MainWindow::onRefreshPreview);
    m_refreshTimer->start(33);

    onNetworkToggle(true);
    updateVCamButtons();
    syncMacroList();
    syncFromDevice();
    uiLog(QString("Switcher: %1 (%2)").arg(m_device.productName(), QDir::toNativeSeparators(m_options.profileDir)));
}

MainWindow::~MainWindow()
{
    m_server.close();
    // Ensure virtual cam is cleaned up (same path as toggle-off)
    if (m_webcamActive) onWebcamToggle();
}

// ── Log helpers ───────────────────────────────────────────────────────────────

static void appendLog(QTextEdit* view, const QString& msg)
{
    if (!view) return;
    view->append(QString("[%1]  %2")
        .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), msg));
    if (view->document()->blockCount() > 500) {
        auto c = view->textCursor();
        c.movePosition(QTextCursor::Start);
        c.select(QTextCursor::BlockUnderCursor);
        c.removeSelectedText(); c.deleteChar();
    }
}

void MainWindow::uiLog(const QString& msg)
{
    appendLog(m_uiLogView, msg);
    LOG_APP(msg);
}

// ── Toggle button group helper ────────────────────────────────────────────────

static void applyToggleGroup(QPushButton* offBtn, QPushButton* onBtn, bool isOn,
                              const char* onBg, const char* onBd, const char* onFg)
{
    // Left button (OFF) — rounded left corners only
    static const char* leftR = "border-top-left-radius:3px;border-bottom-left-radius:3px;"
                               "border-top-right-radius:0;border-bottom-right-radius:0;";
    // Right button (ON) — rounded right corners only
    static const char* rightR= "border-top-right-radius:3px;border-bottom-right-radius:3px;"
                               "border-top-left-radius:0;border-bottom-left-radius:0;";

    if (!isOn) {
        // OFF is active
        offBtn->setStyleSheet(QString("QPushButton{background:#2a0a0a;border:1px solid #501515;"
            "%1color:#cc4444;font-size:9px;font-weight:700;padding:2px 8px;}"
            "QPushButton:hover{background:#3a1010;}").arg(leftR));
        onBtn->setStyleSheet(QString("QPushButton{background:#141414;border:1px solid #1e1e1e;"
            "border-left:0;%1color:#2a2a2a;font-size:9px;font-weight:700;padding:2px 8px;}"
            "QPushButton:hover{background:#1a1a1a;color:#555;}").arg(rightR));
    } else {
        // ON is active
        offBtn->setStyleSheet(QString("QPushButton{background:#141414;border:1px solid #1e1e1e;"
            "%1color:#2a2a2a;font-size:9px;font-weight:700;padding:2px 8px;}"
            "QPushButton:hover{background:#1a1a1a;color:#555;}").arg(leftR));
        onBtn->setStyleSheet(QString("QPushButton{background:%1;border:1px solid %2;"
            "border-left:0;%3color:%4;font-size:9px;font-weight:700;padding:2px 8px;}"
            "QPushButton:hover{background:%1;}").arg(onBg, onBd, rightR, onFg));
    }
}

void MainWindow::updateVCamButtons()
{
    if (m_vcamOnBtn && m_vcamOffBtn)
        applyToggleGroup(m_vcamOffBtn, m_vcamOnBtn, m_webcamActive,
                         "#0a2a0a", "#155015", "#44bb44");
}

void MainWindow::updateNetButtons()
{
    if (m_netOnBtn && m_netOffBtn)
        applyToggleGroup(m_netOffBtn, m_netOnBtn, m_netActive,
                         "#0a1a2a", "#155055", "#44aacc");
}

// ── Left panel ────────────────────────────────────────────────────────────────

QWidget* MainWindow::buildLeftPanel()
{
    auto* panel = new QWidget;
    panel->setStyleSheet("QWidget{background:#0a0a0a;}");
    auto* v = new QVBoxLayout(panel);
    v->setSpacing(0); v->setContentsMargins(0,0,0,0);

    m_preview = new PreviewWidget;
    QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    m_preview->setSizePolicy(sp);

    // ── Control bar: Virtual Camera + Network Binding toggles ──────────────────
    auto* ctrlBar = new QWidget;
    ctrlBar->setStyleSheet("QWidget{background:#0d0d0d;border-top:1px solid #1a1a1a;}");
    auto* ctrlH = new QHBoxLayout(ctrlBar);
    ctrlH->setContentsMargins(8, 4, 8, 4); ctrlH->setSpacing(16);

    auto makeCtrlGroup = [&](const char* lbl, QPushButton*& offBtn, QPushButton*& onBtn) {
        auto* grp = new QHBoxLayout; grp->setSpacing(0); grp->setContentsMargins(0,0,0,0);
        auto* label = new QLabel(lbl);
        label->setStyleSheet("color:#555;font-size:9px;background:transparent;border:0;"
                             "letter-spacing:1px;font-weight:600;");
        offBtn = new QPushButton("OFF"); offBtn->setFixedHeight(17);
        onBtn  = new QPushButton("ON");  onBtn->setFixedHeight(17);
        offBtn->setFocusPolicy(Qt::NoFocus);
        onBtn->setFocusPolicy(Qt::NoFocus);
        grp->addWidget(label);
        grp->addSpacing(6);
        grp->addWidget(offBtn);
        grp->addWidget(onBtn);
        ctrlH->addLayout(grp);
    };

    makeCtrlGroup("VIRTUAL CAMERA", m_vcamOffBtn, m_vcamOnBtn);
    makeCtrlGroup("NETWORK BINDING", m_netOffBtn, m_netOnBtn);
    ctrlH->addStretch();

    connect(m_vcamOffBtn, &QPushButton::clicked, this, [this]{ if ( m_webcamActive) onWebcamToggle(); });
    connect(m_vcamOnBtn,  &QPushButton::clicked, this, [this]{ if (!m_webcamActive) onWebcamToggle(); });
    connect(m_netOffBtn,  &QPushButton::clicked, this, [this]{ onNetworkToggle(false); });
    connect(m_netOnBtn,   &QPushButton::clicked, this, [this]{ onNetworkToggle(true);  });

    auto makeLogHdr = [](const char* title) {
        auto* h = new QLabel(title); h->setFixedHeight(18);
        h->setStyleSheet("background:#0a0a0a;color:#bbb;font-size:9px;"
                         "font-weight:700;letter-spacing:2px;"
                         "border-top:1px solid #181818;border-bottom:1px solid #161616;"
                         "padding:0 6px;");
        return h;
    };
    m_netLogView = new QTextEdit; m_netLogView->setReadOnly(true);
    m_uiLogView  = new QTextEdit; m_uiLogView->setReadOnly(true);

    v->addWidget(m_preview, 0);
    v->addWidget(ctrlBar, 0);
    v->addWidget(makeLogHdr("  NETWORK ACTIVITY"), 0);
    v->addWidget(m_netLogView, 1);
    v->addWidget(makeLogHdr("  APP ACTIVITY"), 0);
    v->addWidget(m_uiLogView, 1);
    return panel;
}

// ── MAIN bus ──────────────────────────────────────────────────────────────────

QWidget* MainWindow::buildPgmBus()
{
    auto* panel = new QWidget;
    panel->setStyleSheet("QWidget{background:#0a0a0a;border-bottom:1px solid #1e1e1e;}");
    auto* h = new QHBoxLayout(panel);
    h->setSpacing(5); h->setContentsMargins(8,8,8,6);

    for (int i = 0; i < kN; ++i) {
        auto* col = new QWidget; col->setStyleSheet("background:transparent;");
        auto* cv  = new QVBoxLayout(col);
        cv->setSpacing(3); cv->setContentsMargins(0,0,0,0);

        auto* btn = new SourceButton(kSrc[i].hw, SourceButton::ModePgm);
        btn->setMinimumHeight(55);
        connect(btn, &QPushButton::clicked, this, [this,i]{ onProgramButton(kSrc[i].id); });
        m_pgmBtns[i] = btn;
        cv->addWidget(btn);

        if (i >= 1 && i <= 4) {
            int ci = i - 1;
            auto* row = new QHBoxLayout; row->setSpacing(3); row->setContentsMargins(0,0,0,0);
            m_pgmColorBtn[ci] = new QPushButton;
            m_pgmColorBtn[ci]->setFixedHeight(14);
            m_pgmColorBtn[ci]->setFocusPolicy(Qt::NoFocus);
            updateColorBtnStyle(ci);
            connect(m_pgmColorBtn[ci], &QPushButton::clicked, this, [this,ci]{ onInputColorPick(ci); });

            m_pgmMediaBtn[ci] = new QPushButton("Media");
            m_pgmMediaBtn[ci]->setFixedHeight(14);
            m_pgmMediaBtn[ci]->setFocusPolicy(Qt::NoFocus);
            m_pgmMediaBtn[ci]->setStyleSheet(
                "QPushButton{background:#181818;border:1px solid #282828;border-radius:2px;"
                "color:#555;font-size:8px;padding:0 3px;}"
                "QPushButton:hover{background:#222;color:#888;}");
            connect(m_pgmMediaBtn[ci], &QPushButton::clicked, this, [this,ci]{ onInputMediaBrowse(ci); });
            row->addWidget(m_pgmColorBtn[ci], 1);
            row->addWidget(m_pgmMediaBtn[ci], 1);
            cv->addLayout(row);
        }
        h->addWidget(col);
    }
    return panel;
}

// ── DVE / PiP section ────────────────────────────────────────────────────────
//
// The PiP is upstream key 1 as a DVE key, exactly as on the ATEM Mini. The
// controls send the switcher the same commands the SDK would; the values
// shown come back from the switcher (so ATEM Software Control, the SDK and
// this window always agree).

QWidget* MainWindow::buildDveSection()
{
    using namespace emu::cmd;
    auto* w = new QWidget; w->setStyleSheet("QWidget{background:#141414;}");
    auto* root = new QVBoxLayout(w);
    root->setSpacing(8); root->setContentsMargins(10, 8, 10, 8);

    // ── Fill source buttons ────────────────────────────────────────────────────
    auto* fillRow = new QHBoxLayout; fillRow->setSpacing(4);
    auto* fillLbl = new QLabel("Fill:");
    fillLbl->setStyleSheet("color:#888;font-size:10px;background:transparent;border:0;");
    fillLbl->setFixedWidth(28);
    fillRow->addWidget(fillLbl);
    for (int i = 0; i < kN; ++i) {
        auto* btn = new SourceButton(kSrc[i].hw, SourceButton::ModeFill);
        btn->setMinimumHeight(55);
        connect(btn, &QPushButton::clicked, this, [this,i]{ onFillBtn(i); });
        m_fillBtns[i] = btn;
        fillRow->addWidget(btn);
    }
    root->addLayout(fillRow);

    // ── Two-column control grid ────────────────────────────────────────────────
    auto* cols = new QHBoxLayout; cols->setSpacing(16); cols->setContentsMargins(0,4,0,0);

    auto rl = [](const char* t, int fw = 0) {
        auto* l = new QLabel(t);
        l->setStyleSheet("color:#888;font-size:10px;background:transparent;border:0;");
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (fw) l->setFixedWidth(fw); return l;
    };

    struct Filter : public QObject {
        std::function<void()> cb;
        bool eventFilter(QObject*, QEvent* e) override {
            if (e->type() == QEvent::FocusOut) cb(); return false;
        }
    };
    auto onBlur = [](QSpinBox* sp, std::function<void()> fn) {
        auto* f = new Filter; f->cb = fn; f->setParent(sp);
        sp->installEventFilter(f);
    };

    // ── LEFT column: Size / Position / Rotation+Opacity ───────────────────────
    auto* lg = new QGridLayout;
    lg->setHorizontalSpacing(5); lg->setVerticalSpacing(7); lg->setContentsMargins(0,0,0,0);

    int r = 0;

    m_sizeXSpin = makeSpin(5, 200, 100, "%");
    m_sizeYSpin = makeSpin(5, 200, 100, "%");
    m_lockSize  = new QCheckBox("Lock"); m_lockSize->setChecked(true);
    m_lockSize->setStyleSheet("color:#888;font-size:10px;spacing:4px;");

    connect(m_sizeXSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v){
        if (m_lockSize->isChecked()) {
            m_sizeYSpin->blockSignals(true); m_sizeYSpin->setValue(v); m_sizeYSpin->blockSignals(false);
        }
        sendSize(v, m_sizeYSpin->value());
    });
    connect(m_sizeYSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v){
        if (m_lockSize->isChecked()) {
            m_sizeXSpin->blockSignals(true); m_sizeXSpin->setValue(v); m_sizeXSpin->blockSignals(false);
        }
        sendSize(m_sizeXSpin->value(), v);
    });
    onBlur(m_sizeXSpin, [this]{ uiLog(QString("PiP Size X: %1%").arg(m_sizeXSpin->value())); });
    onBlur(m_sizeYSpin, [this]{ uiLog(QString("PiP Size Y: %1%").arg(m_sizeYSpin->value())); });

    lg->addWidget(rl("Size", 52), r, 0);
    lg->addWidget(rl("X"),  r, 1); lg->addWidget(m_sizeXSpin, r, 2);
    lg->addWidget(rl("Y"),  r, 3); lg->addWidget(m_sizeYSpin, r, 4);
    lg->addWidget(m_lockSize, r, 5); ++r;

    // Position in 100ths of the ATEM's frame units: +-1600 / +-900 is the
    // frame edge. The switcher accepts far larger values (a PiP parked off
    // screen), so the boxes go to +-200 units and always show the real value.
    m_posXSpin = makeSpin(-20000, 20000, 0);
    m_posYSpin = makeSpin(-20000, 20000, 0);
    connect(m_posXSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int v){ DveParams p; p.positionX = v / 100.0; sendDve(PositionX, p); });
    connect(m_posYSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int v){ DveParams p; p.positionY = v / 100.0; sendDve(PositionY, p); });
    onBlur(m_posXSpin, [this]{ uiLog(QString("PiP Pos X: %1").arg(m_posXSpin->value() / 100.0)); });
    onBlur(m_posYSpin, [this]{ uiLog(QString("PiP Pos Y: %1").arg(m_posYSpin->value() / 100.0)); });

    lg->addWidget(rl("Position", 52), r, 0);
    lg->addWidget(rl("X"), r, 1); lg->addWidget(m_posXSpin, r, 2);
    lg->addWidget(rl("Y"), r, 3); lg->addWidget(m_posYSpin, r, 4); ++r;

    // Rotation and opacity are the emulator's own: an ATEM Mini can't do either.
    m_rotationSpin = makeSpin(0, 359, 0, "\xc2\xb0");
    m_opacitySpin  = makeSpin(0, 100, 100, "%");
    m_rotationSpin->setToolTip("Emulator only: an ATEM Mini can't rotate the PiP");
    m_opacitySpin->setToolTip("Emulator only: an ATEM Mini can't fade the PiP");
    connect(m_rotationSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int v){ m_rotation = v; });
    connect(m_opacitySpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int v){ m_opacity = v; });
    onBlur(m_rotationSpin, [this]{ uiLog(QString("PiP Rotation: %1\xc2\xb0").arg(m_rotation)); });
    onBlur(m_opacitySpin,  [this]{ uiLog(QString("PiP Opacity: %1%").arg(m_opacity)); });

    lg->addWidget(rl("Rotation", 52), r, 0);
    lg->addWidget(rl(""),         r, 1); lg->addWidget(m_rotationSpin, r, 2);
    lg->addWidget(rl("Opacity"),  r, 3); lg->addWidget(m_opacitySpin, r, 4); ++r;

    lg->setColumnStretch(6, 1);

    // ── RIGHT column: Border / Crop ───────────────────────────────────────────
    auto* rg = new QGridLayout;
    rg->setHorizontalSpacing(5); rg->setVerticalSpacing(7); rg->setContentsMargins(0,0,0,0);

    r = 0;

    // Border in px at 1280 wide; the ATEM's border width runs 0-16 (x3.125).
    m_borderSpin     = makeSpin(0, 50, 0, "px");
    m_borderColorBtn = new QPushButton;
    m_borderColorBtn->setFixedSize(28, 28);
    m_borderColorBtn->setToolTip("Border colour");
    connect(m_borderSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v){
        DveParams p; p.border = v > 0; p.borderWidth = v / 3.125;
        sendDve(Border | BorderWidth, p);
    });
    connect(m_borderColorBtn, &QPushButton::clicked, this, &MainWindow::onKeyerBorderColorPick);
    onBlur(m_borderSpin, [this]{ uiLog(QString("PiP Border: %1px").arg(m_borderSpin->value())); });

    rg->addWidget(rl("Border", 44), r, 0);
    rg->addWidget(rl(""),           r, 1); rg->addWidget(m_borderSpin,     r, 2);
    rg->addWidget(m_borderColorBtn, r, 3); ++r;

    // Crop in percent of the picture; sent as the DVE mask.
    m_cropLSpin = makeSpin(0, 50, 0, "%");
    m_cropRSpin = makeSpin(0, 50, 0, "%");
    m_cropTSpin = makeSpin(0, 50, 0, "%");
    m_cropBSpin = makeSpin(0, 50, 0, "%");
    for (QSpinBox* sp : { m_cropLSpin, m_cropRSpin, m_cropTSpin, m_cropBSpin })
        connect(sp, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int){ sendCrop(); });
    onBlur(m_cropLSpin, [this]{ uiLog(QString("PiP Crop L: %1%").arg(m_cropLSpin->value())); });
    onBlur(m_cropRSpin, [this]{ uiLog(QString("PiP Crop R: %1%").arg(m_cropRSpin->value())); });
    onBlur(m_cropTSpin, [this]{ uiLog(QString("PiP Crop T: %1%").arg(m_cropTSpin->value())); });
    onBlur(m_cropBSpin, [this]{ uiLog(QString("PiP Crop B: %1%").arg(m_cropBSpin->value())); });

    rg->addWidget(rl("Crop", 44), r, 0);
    rg->addWidget(rl("L"),        r, 1); rg->addWidget(m_cropLSpin, r, 2);
    rg->addWidget(rl("R"),        r, 3); rg->addWidget(m_cropRSpin, r, 4); ++r;

    rg->addWidget(rl("", 44),  r, 0);
    rg->addWidget(rl("T"),     r, 1); rg->addWidget(m_cropTSpin, r, 2);
    rg->addWidget(rl("B"),     r, 3); rg->addWidget(m_cropBSpin, r, 4); ++r;

    rg->setColumnStretch(5, 1);

    // vertical separator
    auto* sep = new QFrame; sep->setFrameShape(QFrame::VLine);
    sep->setStyleSheet("color:#222;");

    cols->addLayout(lg, 1);
    cols->addWidget(sep);
    cols->addLayout(rg, 1);
    root->addLayout(cols);
    return w;
}

// ── Macros section ────────────────────────────────────────────────────────────

QWidget* MainWindow::buildMacroSection()
{
    auto* w = new QWidget; w->setStyleSheet("QWidget{background:#141414;}");
    auto* root = new QHBoxLayout(w);
    root->setSpacing(8); root->setContentsMargins(10,8,10,8);

    m_macroList = new QListWidget;
    m_macroList->setItemDelegate(new MacroDelegate);
    m_macroList->setIconSize(QSize(64, 36));
    m_macroList->setFixedWidth(220);
    m_macroList->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_macroList->setMinimumHeight(60);
    connect(m_macroList, &QListWidget::itemSelectionChanged,
            this, &MainWindow::onMacroSelectionChanged);
    root->addWidget(m_macroList);

    auto* rightCol = new QWidget; rightCol->setStyleSheet("background:transparent;");
    rightCol->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* rv = new QVBoxLayout(rightCol);
    rv->setSpacing(5); rv->setContentsMargins(0,0,0,0);

    // Name row — fixed height
    auto* nameRow = new QHBoxLayout; nameRow->setSpacing(5);
    auto* nameLbl = new QLabel("Name");
    nameLbl->setStyleSheet("color:#666;font-size:10px;background:transparent;border:0;");
    nameLbl->setFixedWidth(34);
    m_macroNameEdit = new QLineEdit; m_macroNameEdit->setPlaceholderText("Macro name");
    nameRow->addWidget(nameLbl); nameRow->addWidget(m_macroNameEdit);
    rv->addLayout(nameRow, 0);

    auto makeDescHdr = [](const char* t) {
        auto* l = new QLabel(t);
        l->setStyleSheet("color:#666;font-size:9px;background:transparent;border:0;");
        return l;
    };

    // Desc + State row — expands with window height
    auto* descRow = new QHBoxLayout; descRow->setSpacing(6);

    auto* descCol = new QVBoxLayout;
    descCol->setSpacing(2);
    descCol->addWidget(makeDescHdr("Description"), 0);
    m_macroDescEdit = new QTextEdit;
    m_macroDescEdit->setPlaceholderText("Notes...");
    m_macroDescEdit->setMinimumHeight(50);
    m_macroDescEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_macroDescEdit->setStyleSheet(
        "QTextEdit{background:#161616;border:1px solid #2a2a2a;color:#ccc;"
        "font-family:'Segoe UI',Arial,sans-serif;font-size:11px;"
        "padding:3px 6px;border-radius:4px;}");
    descCol->addWidget(m_macroDescEdit, 1);

    auto* snapCol = new QVBoxLayout;
    snapCol->setSpacing(2);
    snapCol->addWidget(makeDescHdr("Saved State"), 0);
    m_snapshotView = new QTextEdit;
    m_snapshotView->setReadOnly(true);
    m_snapshotView->setMinimumHeight(50);
    m_snapshotView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_snapshotView->setStyleSheet(
        "QTextEdit{background:#0a0f0a;border:1px solid #1a2a1a;color:#44aa44;"
        "font-family:Consolas,'Courier New',monospace;font-size:9px;"
        "padding:3px 6px;border-radius:4px;}");
    m_snapshotView->setPlaceholderText("(no saved state)");
    snapCol->addWidget(m_snapshotView, 1);

    descRow->addLayout(descCol, 1);
    descRow->addLayout(snapCol, 1);
    rv->addLayout(descRow, 1);   // stretch=1: grows with height

    auto* btnsRow = new QHBoxLayout; btnsRow->setSpacing(5);
    auto* runBtn    = makeSmallBtn("\u25b6 Play",         "#0a1e0a","#155015","#449944","#102010");
    auto* updateBtn = makeSmallBtn("Update",              "#1a1a0a","#3a3a15","#aaaa44","#202010");
    auto* saveBtn   = makeSmallBtn("\u2299 Save Output",  "#0d1e0d","#1a3a1a","#3a8a3a","#102010");
    m_macroRunBtn = runBtn;
    m_macroStatus = new QLabel("IDLE");
    m_macroStatus->setStyleSheet("color:#333;font-weight:700;font-size:10px;"
                                 "letter-spacing:1px;background:transparent;border:0;");
    connect(runBtn,    &QPushButton::clicked, this, &MainWindow::onMacroRun);
    connect(updateBtn, &QPushButton::clicked, this, &MainWindow::onMacroUpdate);
    connect(saveBtn,   &QPushButton::clicked, this, &MainWindow::onMacroSaveOutput);
    btnsRow->addWidget(runBtn); btnsRow->addWidget(updateBtn);
    btnsRow->addWidget(saveBtn); btnsRow->addStretch();
    btnsRow->addWidget(m_macroStatus);
    rv->addLayout(btnsRow);

    root->addWidget(rightCol, 1);
    return w;
}

// ── Right panel ───────────────────────────────────────────────────────────────

QWidget* MainWindow::buildRightPanel()
{
    auto* panel = new QWidget; panel->setStyleSheet("QWidget{background:#111;}");
    auto* v = new QVBoxLayout(panel);
    v->setSpacing(0); v->setContentsMargins(0,0,0,0);

    // Fixed-height sections (stretch=0)
    v->addWidget(sectionHeader("MAIN"),                               0);
    v->addWidget(buildPgmBus(),                                       0);
    v->addWidget(sectionHeader("PIP \u2014 PICTURE-IN-PICTURE OVERLAY"), 0);
    v->addWidget(buildDveSection(),                                   0);

    // Expanding section (stretch=1) — grows as window height increases
    v->addWidget(sectionHeader("MACROS"),                             0);
    v->addWidget(buildMacroSection(),                                 1);
    return panel;
}

// ── Main assembly ─────────────────────────────────────────────────────────────

void MainWindow::buildUi()
{
    auto* central = new QWidget; setCentralWidget(central);
    auto* h = new QHBoxLayout(central);
    h->setSpacing(0); h->setContentsMargins(0,0,0,0);
    auto* div = new QFrame; div->setFrameShape(QFrame::VLine);
    div->setStyleSheet("color:#1a1a1a;");
    h->addWidget(buildLeftPanel(), 1);
    h->addWidget(div);
    h->addWidget(buildRightPanel(), 2);
    m_statusLabel = new QLabel;
    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->setStyleSheet("background:#0a0a0a;color:#333;"
                               "font-size:10px;border-top:1px solid #181818;");
}

// ── Virtual Camera (DirectShow, shared memory) ────────────────────────────────

void MainWindow::onWebcamToggle()
{
    if (!m_webcamActive) {
        // Create shared memory that the DLL (loaded in OBS/Zoom) will read
        HANDLE hMem = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
                                         PAGE_READWRITE, 0,
                                         sizeof(VCamSharedFrame),
                                         kSharedMemName);
        if (!hMem) {
            uiLog("VCam: failed to create shared memory");
            return;
        }
        m_vcamSharedMem = hMem;
        m_vcamFrame = MapViewOfFile(hMem, FILE_MAP_WRITE, 0, 0, 0);
        if (!m_vcamFrame) {
            uiLog("VCam: failed to map shared memory");
            CloseHandle(hMem); m_vcamSharedMem = nullptr; return;
        }

        m_vcamEvent = CreateEventA(nullptr, FALSE, FALSE, kSharedEventName);

        // Register DLL (writes HKCU registry — no admin required)
        QString dllPath = QCoreApplication::applicationDirPath() + "/AtemVirtualCam.dll";
        HMODULE hDll = LoadLibraryW((const wchar_t*)dllPath.utf16());
        if (hDll) {
            typedef HRESULT (STDAPICALLTYPE *PFN_Register)();
            auto fn = (PFN_Register)GetProcAddress(hDll, "DllRegisterServer");
            if (fn && SUCCEEDED(fn())) m_vcamRegistered = true;
            FreeLibrary(hDll);
        }
        if (!m_vcamRegistered)
            uiLog("VCam: DLL registration failed — OBS may not see the camera");

        m_webcamActive = true;
        updateVCamButtons();
        uiLog("Virtual camera started \u2014 add as Video Capture Device in OBS/Zoom");

    } else {
        // Unregister DLL
        if (m_vcamRegistered) {
            QString dllPath = QCoreApplication::applicationDirPath() + "/AtemVirtualCam.dll";
            HMODULE hDll = LoadLibraryW((const wchar_t*)dllPath.utf16());
            if (hDll) {
                typedef HRESULT (STDAPICALLTYPE *PFN_Unreg)();
                auto fn = (PFN_Unreg)GetProcAddress(hDll, "DllUnregisterServer");
                if (fn) fn();
                FreeLibrary(hDll);
            }
            m_vcamRegistered = false;
        }

        if (m_vcamFrame)    { UnmapViewOfFile(m_vcamFrame);             m_vcamFrame     = nullptr; }
        if (m_vcamSharedMem){ CloseHandle((HANDLE)m_vcamSharedMem);     m_vcamSharedMem = nullptr; }
        if (m_vcamEvent)    { CloseHandle((HANDLE)m_vcamEvent);         m_vcamEvent     = nullptr; }

        m_webcamActive = false;
        updateVCamButtons();
        uiLog("Virtual camera stopped");
    }
}

void MainWindow::pushWebcamFrame(const QImage& img)
{
    if (!m_vcamFrame) return;
    auto* f = static_cast<VCamSharedFrame*>(m_vcamFrame);

    // Scale to 1280×720 in Qt's Format_RGB32 (BGRA on x86 — matches DirectShow RGB32)
    QImage src = img.scaled(kVCamWidth, kVCamHeight,
                            Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                    .convertToFormat(QImage::Format_RGB32);

    f->width  = kVCamWidth;
    f->height = kVCamHeight;
    memcpy(f->bgra, src.constBits(), qMin((int)sizeof(f->bgra), (int)src.sizeInBytes()));
    InterlockedIncrement(&f->frameId);  // atomic increment — DLL reads this
    if (m_vcamEvent) SetEvent((HANDLE)m_vcamEvent);
}

// ── Commands to the switcher ──────────────────────────────────────────────────
//
// Everything this window changes goes through the switcher's command handlers,
// exactly as if ATEM Software Control or the SDK had sent it; the switcher
// then tells every connected client.

void MainWindow::apply(const char* command, const QByteArray& data)
{
    m_device.apply(QByteArray(command), data);
}

void MainWindow::sendDve(quint32 mask, const emu::cmd::DveParams& p)
{
    apply("CKDV", emu::cmd::dve(0, 0, mask, p));
}

void MainWindow::sendSize(int percentX, int percentY)
{
    emu::cmd::DveParams p;
    p.sizeX = percentX / 100.0;
    p.sizeY = percentY / 100.0;
    sendDve(emu::cmd::SizeX | emu::cmd::SizeY, p);
}

// Crop spin boxes are percent of the picture; the DVE mask is in frame units
// (top/bottom of 18, left/right of 32). Masking is on while any edge is > 0.
void MainWindow::sendCrop()
{
    using namespace emu::cmd;
    DveParams p;
    p.maskTop    = m_cropTSpin->value() * 0.18;
    p.maskBottom = m_cropBSpin->value() * 0.18;
    p.maskLeft   = m_cropLSpin->value() * 0.32;
    p.maskRight  = m_cropRSpin->value() * 0.32;
    p.masked = p.maskTop > 0 || p.maskBottom > 0 || p.maskLeft > 0 || p.maskRight > 0;
    sendDve(Masked | MaskTop | MaskBottom | MaskLeft | MaskRight, p);
}

QString MainWindow::sourceName(quint16 id) const
{
    for (const auto& in : m_device.inputs())
        if (in.id == id) return in.longName;
    return QString("input %1").arg(id);
}

static QColor hslColor(double hue, double saturation, double luma, double alpha = 1.0)
{
    double h = std::fmod(hue, 360.0) / 360.0;
    if (h < 0) h += 1.0;
    return QColor::fromHslF(float(h), float(qBound(0.0, saturation, 1.0)),
                            float(qBound(0.0, luma, 1.0)), float(qBound(0.0, alpha, 1.0)));
}

// ── Slots ─────────────────────────────────────────────────────────────────────

void MainWindow::setProgramSource(quint16 src)
{
    apply("CPgI", emu::cmd::programInput(0, src));
}

void MainWindow::onProgramButton(int id)
{
    uiLog(QString("PGM → %1").arg(sourceName((quint16)id)));
    setProgramSource((quint16)id);
}

// Button labels follow the switcher's input names once they are renamed
// (e.g. in ATEM Software Control).
void MainWindow::syncProgramButtons()
{
    emu::SwitcherView v = m_device.view();
    QList<emu::InputInfo> inputs = m_device.inputs();
    for (int i = 0; i < kN; ++i) {
        QString label = kSrc[i].hw;
        for (const auto& in : inputs)
            if (in.id == kSrc[i].id && !in.namesDefault && !in.shortName.isEmpty()) label = in.shortName;
        if (m_pgmBtns[i]) {
            m_pgmBtns[i]->setText(label);
            m_pgmBtns[i]->setActive(kSrc[i].id == v.program);
        }
        if (m_fillBtns[i]) m_fillBtns[i]->setText(label);
    }
}

// Fill buttons: pressing the lit one takes the PiP off air; another one makes
// it the PiP source (as a DVE key) and puts the PiP on air.
void MainWindow::onFillBtn(int idx)
{
    quint16 src = kSrc[idx].id;
    emu::SwitcherView v = m_device.view();
    if (v.keyOnAir && v.keyType == 3 && v.keyFill == src) {
        apply("CKOn", emu::cmd::keyOnAir(0, 0, false));
        uiLog("PiP: Off");
        return;
    }
    if (v.keyType != 3) {
        if (!v.keyCanUseDve)
            uiLog("PiP: the DVE transition is using the DVE — set the next transition to Mix first");
        apply("CKTp", emu::cmd::keyType(0, 0, 3));
    }
    apply("CKeF", emu::cmd::keyFill(0, 0, src));
    apply("CKOn", emu::cmd::keyOnAir(0, 0, true));
    uiLog(QString("PiP Fill → %1").arg(sourceName(src)));
}

void MainWindow::onKeyerBorderColorPick()
{
    emu::SwitcherView v = m_device.view();
    QColor c = QColorDialog::getColor(hslColor(v.borderHue, v.borderSaturation, v.borderLuma),
                                      this, "PiP Border Colour");
    if (!c.isValid()) return;
    float h, s, l;
    c.getHslF(&h, &s, &l);
    emu::cmd::DveParams p;
    p.borderHue = h < 0 ? 0 : h * 360;
    p.borderSaturation = s;
    p.borderLuma = l;
    sendDve(emu::cmd::BorderHue | emu::cmd::BorderSaturation | emu::cmd::BorderLuma, p);
    uiLog(QString("PiP Border Colour: %1").arg(c.name()));
}

void MainWindow::updateBorderColorBtn()
{
    if (!m_borderColorBtn) return;
    emu::SwitcherView v = m_device.view();
    QColor c = hslColor(v.borderHue, v.borderSaturation, v.borderLuma);
    m_borderColorBtn->setStyleSheet(
        QString("QPushButton{background:%1;border:1px solid %2;border-radius:3px;}"
                "QPushButton:hover{background:%3;}")
        .arg(c.name(), c.lighter(130).name(), c.lighter(115).name()));
}

void MainWindow::syncKeyerUi()
{
    auto blk = [](QSpinBox* w, int v) {
        if (!w || w->value() == v) return;
        w->blockSignals(true); w->setValue(v); w->blockSignals(false);
    };
    emu::SwitcherView v = m_device.view();
    bool pipOn = v.keyOnAir && v.keyType == 3;
    for (int i = 0; i < kN; ++i)
        if (m_fillBtns[i]) m_fillBtns[i]->setActive(pipOn && v.keyFill == kSrc[i].id);

    blk(m_sizeXSpin, qRound(v.sizeX * 100));
    blk(m_sizeYSpin, qRound(v.sizeY * 100));
    blk(m_posXSpin,  qRound(v.positionX * 100));
    blk(m_posYSpin,  qRound(v.positionY * 100));
    blk(m_borderSpin, v.borderEnabled ? qRound(v.borderWidth * 3.125) : 0);
    blk(m_cropTSpin, v.masked ? qRound(v.maskTop / 0.18) : 0);
    blk(m_cropBSpin, v.masked ? qRound(v.maskBottom / 0.18) : 0);
    blk(m_cropLSpin, v.masked ? qRound(v.maskLeft / 0.32) : 0);
    blk(m_cropRSpin, v.masked ? qRound(v.maskRight / 0.32) : 0);
    blk(m_rotationSpin, m_rotation);
    blk(m_opacitySpin, m_opacity);
    updateBorderColorBtn();
}

void MainWindow::syncFromDevice()
{
    emu::SwitcherView v = m_device.view();
    if (!v.fadeInTransition) m_fadeFromBlack = v.fadeFullyBlack;
    syncProgramButtons();
    syncKeyerUi();
    updateMacroStatus();
}

void MainWindow::updateColorBtnStyle(int i)
{
    if (!m_pgmColorBtn[i]) return;
    QColor c = m_sources[i]->color();
    bool dark = c.lightness() < 130;
    m_pgmColorBtn[i]->setStyleSheet(
        QString("QPushButton{background:%1;border:1px solid %2;border-radius:2px;color:%3;font-size:8px;}"
                "QPushButton:hover{background:%4;}")
        .arg(c.name(), c.lighter(140).name(), dark?"#fff":"#000", c.lighter(115).name()));
}

void MainWindow::onInputColorPick(int i)
{
    QColor c = QColorDialog::getColor(m_sources[i]->color(), this,
                                      QString("Camera %1 Colour").arg(i+1));
    if (!c.isValid()) return;
    m_sources[i]->setColor(c); m_useVideo[i] = false; m_usePhoto[i] = false;
    updateColorBtnStyle(i);
    if (m_pgmMediaBtn[i]) m_pgmMediaBtn[i]->setText("Media");
    uiLog(QString("CAM%1 colour → %2").arg(i+1).arg(c.name()));
}

void MainWindow::onInputMediaBrowse(int i)
{
    QString path = QFileDialog::getOpenFileName(this,
        QString("Media for Camera %1").arg(i+1), {},
        "Media Files (*.jpg *.jpeg *.png *.bmp *.webp *.tiff *.tif "
                     "*.mp4 *.mov *.avi *.mkv *.wmv *.m4v);;All Files (*)");
    if (path.isEmpty()) return;
    static const QStringList imgs = {"jpg","jpeg","png","bmp","webp","tiff","tif"};
    QString ext = QFileInfo(path).suffix().toLower();
    QString fn  = QFileInfo(path).fileName();
    if (imgs.contains(ext)) {
        if (!m_photoSources[i]->loadFile(path)) { uiLog(QString("CAM%1: failed to load image").arg(i+1)); return; }
        m_usePhoto[i] = true; m_useVideo[i] = false;
        uiLog(QString("CAM%1 photo → %2").arg(i+1).arg(fn));
    } else {
        m_videoSources[i]->loadFile(path); m_useVideo[i] = true; m_usePhoto[i] = false;
        uiLog(QString("CAM%1 video → %2").arg(i+1).arg(fn));
    }
    if (m_pgmMediaBtn[i]) m_pgmMediaBtn[i]->setText(fn.left(8));
}

// ── Macros ────────────────────────────────────────────────────────────────────
//
// The list is the switcher's macro pool: macros recorded in ATEM Software
// Control show up here, and macros saved here show up there and in the SDK.

void MainWindow::onMacroRun()
{
    emu::SwitcherView v = m_device.view();
    if (v.macroRunning) {                            // the button is "Stop" while a macro runs
        apply("MAct", emu::cmd::macroAction(0xffff, 1));
        uiLog("Macro stopped");
        return;
    }
    auto* sel = m_macroList->currentItem(); if (!sel) return;
    int slot = sel->data(Qt::UserRole).toInt();
    if (!m_device.macro(slot).used) { uiLog(QString("Macro slot %1 is empty").arg(slot + 1)); return; }
    uiLog(QString("Macro run: \"%1\"").arg(m_device.macro(slot).name));
    apply("MAct", emu::cmd::macroAction((quint16)slot, 0));
}

void MainWindow::onMacroUpdate()
{
    auto* sel = m_macroList->currentItem(); if (!sel) return;
    int slot = sel->data(Qt::UserRole).toInt();
    QString name = m_macroNameEdit->text().trimmed();
    QString desc = m_macroDescEdit->toPlainText().trimmed();
    if (m_device.macro(slot).used) {
        apply("CMPr", emu::cmd::macroProperties((quint16)slot, name, desc));
    } else {
        emu::Macro mac;
        mac.used = true; mac.name = name; mac.description = desc;
        m_device.setMacro(slot, mac);
    }
    saveMacros();
    syncMacroList();
    uiLog(QString("Macro %1 updated: \"%2\"").arg(slot+1).arg(name));
}

// Stores the current picture as a macro: program, PiP source, DVE settings
// and PiP on/off, as switcher commands (so it runs anywhere the switcher's
// macros run), plus the camera pictures for this emulator.
void MainWindow::onMacroSaveOutput()
{
    using namespace emu::cmd;
    auto* sel = m_macroList->currentItem();
    if (!sel) { uiLog("Save Output: no macro selected"); return; }
    int slot = sel->data(Qt::UserRole).toInt();
    emu::SwitcherView v = m_device.view();

    emu::Macro mac = m_device.macro(slot);
    if (!mac.used) {
        mac.name = m_macroNameEdit->text().trimmed();
        mac.description = m_macroDescEdit->toPlainText().trimmed();
    }
    if (mac.name.isEmpty()) mac.name = QString("Macro %1").arg(slot + 1);
    mac.used = true;
    mac.ops.clear();
    auto op = [&](const char* name, const QByteArray& data) { mac.ops.append({emu::MacroOp::Command, name, data, 0}); };
    op("CPgI", programInput(0, v.program));
    op("CKTp", keyType(0, 0, v.keyType));
    op("CKeF", keyFill(0, 0, v.keyFill));
    DveParams p;
    p.sizeX = v.sizeX; p.sizeY = v.sizeY; p.positionX = v.positionX; p.positionY = v.positionY;
    p.border = v.borderEnabled; p.borderWidth = v.borderWidth; p.borderOpacity = v.borderOpacity;
    p.borderHue = v.borderHue; p.borderSaturation = v.borderSaturation; p.borderLuma = v.borderLuma;
    p.masked = v.masked; p.maskTop = v.maskTop; p.maskBottom = v.maskBottom;
    p.maskLeft = v.maskLeft; p.maskRight = v.maskRight;
    op("CKDV", dve(0, 0, SizeX | SizeY | PositionX | PositionY | Border | BorderWidth | BorderOpacity |
                         BorderHue | BorderSaturation | BorderLuma | Masked | MaskTop | MaskBottom |
                         MaskLeft | MaskRight, p));
    op("CKOn", keyOnAir(0, 0, v.keyOnAir));
    m_device.setMacro(slot, mac);

    MacroExtras& x = m_macroExtras[slot];
    x.captured = true;
    x.lockSize = m_lockSize ? m_lockSize->isChecked() : true;
    x.rotation = m_rotation;
    x.opacity  = m_opacity;
    for (int i = 0; i < 4; ++i) {
        if (m_useVideo[i])       x.inputs[i] = { Atem::InputMode::Video, 0, m_videoSources[i]->filePath() };
        else if (m_usePhoto[i])  x.inputs[i] = { Atem::InputMode::Photo, 0, m_photoSources[i]->path() };
        else                     x.inputs[i] = { Atem::InputMode::SolidColor, m_sources[i]->color().rgba(), {} };
    }
    saveMacros();

    QImage frame = m_compositor.compose(sourceForId(v.program)->currentFrame(),
                                        sourceForId(v.keyFill)->currentFrame(), pipState(v));
    frame.scaled(320,180, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
         .save(macroThumbPath(slot), "JPEG", 80);

    syncMacroList();
    updateSnapshotDisplay(slot);
    uiLog(QString("Macro \"%1\" saved: PGM=%2, PiP=%3")
          .arg(mac.name).arg(sourceName(v.program))
          .arg(v.keyOnAir ? "On" : "Off"));

    m_macroStatus->setText("✓ Saved");
    m_macroStatus->setStyleSheet("color:#44bb44;font-weight:700;font-size:10px;"
                                 "letter-spacing:1px;background:transparent;border:0;");
    QTimer::singleShot(2000, this, [this]{ updateMacroStatus(); });
}

void MainWindow::onMacroSelectionChanged()
{
    auto* sel = m_macroList->currentItem(); if (!sel) return;
    int slot = sel->data(Qt::UserRole).toInt();
    m_macroNameEdit->setText(m_device.macro(slot).name);
    m_macroDescEdit->setPlainText(m_device.macro(slot).description);
    updateSnapshotDisplay(slot);
}

// Camera pictures, size lock, rotation and opacity saved with a macro are
// applied when the macro runs (from here or from any client).
void MainWindow::applyMacroExtras(int slot)
{
    if (slot < 0 || slot >= m_macroExtras.size()) return;
    const MacroExtras& x = m_macroExtras[slot];
    if (!x.captured) return;
    if (m_lockSize) m_lockSize->setChecked(x.lockSize);
    m_rotation = x.rotation;
    m_opacity = x.opacity;
    for (int i = 0; i < 4; ++i) {
        const auto& inp = x.inputs[i];
        if (inp.mode == Atem::InputMode::SolidColor) {
            m_sources[i]->setColor(QColor::fromRgba(inp.argb));
            m_useVideo[i] = false; m_usePhoto[i] = false;
            updateColorBtnStyle(i);
            if (m_pgmMediaBtn[i]) m_pgmMediaBtn[i]->setText("Media");
        } else if (inp.mode == Atem::InputMode::Photo) {
            if (m_photoSources[i]->loadFile(inp.path))
                { m_usePhoto[i] = true; m_useVideo[i] = false;
                  if (m_pgmMediaBtn[i]) m_pgmMediaBtn[i]->setText(QFileInfo(inp.path).fileName().left(8)); }
        } else {
            m_videoSources[i]->loadFile(inp.path); m_useVideo[i] = true; m_usePhoto[i] = false;
            if (m_pgmMediaBtn[i]) m_pgmMediaBtn[i]->setText(QFileInfo(inp.path).fileName().left(8));
        }
    }
    syncKeyerUi();
}

// Human-readable steps of a macro.
QString MainWindow::describeMacro(int slot) const
{
    const emu::Macro& mac = m_device.macro(slot);
    if (!mac.used) return "(empty slot)";
    static const char* keyTypes[] = { "luma", "chroma", "pattern", "DVE" };
    QStringList lines;
    for (const emu::MacroOp& op : mac.ops) {
        const QByteArray& d = op.data;
        switch (op.kind) {
        case emu::MacroOp::Wait:     lines << QString("Wait %1 frames").arg(op.frames); continue;
        case emu::MacroOp::UserWait: lines << "Wait for user"; continue;
        case emu::MacroOp::Patch:    lines << QString("Set %1 (recorded)").arg(QString::fromLatin1(op.name)); continue;
        case emu::MacroOp::Command:  break;
        }
        if (op.name == "CPgI")      lines << "Program → " + sourceName(emu::u16(d, 2));
        else if (op.name == "CPvI") lines << "Preview → " + sourceName(emu::u16(d, 2));
        else if (op.name == "CKeF") lines << "PiP source → " + sourceName(emu::u16(d, 2));
        else if (op.name == "CKOn") lines << QString("PiP %1").arg(emu::u8(d, 2) ? "on" : "off");
        else if (op.name == "CKTp") lines << QString("Key type → %1").arg(keyTypes[qMin<int>(emu::u8(d, 3), 3)]);
        else if (op.name == "DCut") lines << "Cut";
        else if (op.name == "DAut") lines << "Auto transition";
        else if (op.name == "FtbA") lines << "Fade to black";
        else if (op.name == "CKDV") {
            quint32 mask = emu::u32(d, 0);
            QStringList parts;
            if (mask & (emu::cmd::SizeX | emu::cmd::SizeY))
                parts << QString("size %1%").arg(qRound(emu::i32(d, 8) / 10.0));
            if (mask & (emu::cmd::PositionX | emu::cmd::PositionY))
                parts << QString("pos %1, %2").arg(emu::i32(d, 16) / 1000.0).arg(emu::i32(d, 20) / 1000.0);
            if (mask & emu::cmd::Masked) parts << QString("crop %1").arg(emu::u8(d, 51) ? "on" : "off");
            if (mask & emu::cmd::Border) parts << QString("border %1").arg(emu::u8(d, 28) ? "on" : "off");
            lines << "PiP " + (parts.isEmpty() ? QString("settings") : parts.join(", "));
        }
        else lines << QString::fromLatin1(op.name);
    }
    if (slot < m_macroExtras.size() && m_macroExtras[slot].captured) {
        const MacroExtras& x = m_macroExtras[slot];
        lines << "─────────────";
        for (int i = 0; i < 4; ++i) {
            const auto& inp = x.inputs[i];
            QString val = (inp.mode == Atem::InputMode::SolidColor)
                ? QColor::fromRgba(inp.argb).name()
                : ((inp.mode == Atem::InputMode::Photo ? "img:" : "vid:") + QFileInfo(inp.path).fileName().left(16));
            lines << QString("CAM%1: %2").arg(i+1).arg(val);
        }
    }
    return lines.isEmpty() ? "(no steps)" : lines.join('\n');
}

void MainWindow::updateSnapshotDisplay(int slot)
{
    if (m_snapshotView) m_snapshotView->setPlainText(describeMacro(slot));
}

void MainWindow::syncMacroList()
{
    if (!m_macroList) return;
    int selSlot = m_macroList->currentItem()
        ? m_macroList->currentItem()->data(Qt::UserRole).toInt() : -1;
    m_macroList->clear();
    for (int i = 0; i < m_device.macroCount(); ++i) {
        const emu::Macro& mac = m_device.macro(i);
        auto* item = new QListWidgetItem;
        item->setData(Qt::DisplayRole, mac.used && !mac.name.isEmpty() ? mac.name
                                       : mac.used ? QString("Macro %1").arg(i+1) : QString("Slot %1").arg(i+1));
        item->setData(Qt::UserRole, i);
        item->setData(Qt::UserRole+1, mac.description);
        if (mac.used && QFile::exists(macroThumbPath(i))) item->setIcon(QIcon(macroThumbPath(i)));
        m_macroList->addItem(item);
    }
    for (int i = 0; i < m_macroList->count(); ++i)
        if (m_macroList->item(i)->data(Qt::UserRole).toInt() == selSlot)
            { m_macroList->setCurrentRow(i); break; }
    if (selSlot >= 0) updateSnapshotDisplay(selSlot);
}

void MainWindow::updateMacroStatus()
{
    if (!m_macroStatus) return;
    emu::SwitcherView v = m_device.view();
    QString text = "IDLE", color = "#333";
    if (v.macroRecording) {
        text = QString("● REC  %1").arg(v.macroRecordingIndex + 1);
        color = "#cc4444";
    } else if (v.macroRunning && v.macroIndex >= 0 && v.macroIndex < m_device.macroCount()) {
        text = QString(v.macroWaiting ? "⏸  %1" : "▶  %1").arg(m_device.macro(v.macroIndex).name);
        color = "#44bb44";
    }
    if (m_macroStatus->text() != "✓ Saved") {
        m_macroStatus->setText(text);
        m_macroStatus->setStyleSheet(QString("color:%1;font-weight:700;font-size:10px;"
                                             "letter-spacing:1px;background:transparent;border:0;").arg(color));
    }
    if (m_macroRunBtn) m_macroRunBtn->setText(v.macroRunning ? "■ Stop" : "▶ Play");
}

void MainWindow::onMacroStarted(int index)
{
    applyMacroExtras(index);
    updateMacroStatus();
}

void MainWindow::onMacroFinished(int)
{
    updateMacroStatus();
}

// ── Preview & thumbnails ──────────────────────────────────────────────────────

void MainWindow::updateSourceThumbs()
{
    for (int i = 0; i < kN; ++i) {
        QImage f = sourceForId(kSrc[i].id)->currentFrame()
                   .scaled(240, 135, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        if (m_pgmBtns[i])  m_pgmBtns[i]->setThumb(f);
        if (m_fillBtns[i]) m_fillBtns[i]->setThumb(f);
    }
}

// The PiP as the switcher has it: upstream key 1 on air as a DVE key.
Atem::KeDVState MainWindow::pipState(const emu::SwitcherView& v) const
{
    Atem::KeDVState d;
    d.enabled = v.keyOnAir && v.keyType == 3;
    d.fillSrc = v.keyFill;
    d.posX = (qint32)std::lround(v.positionX * 1000);
    d.posY = (qint32)std::lround(v.positionY * 1000);
    d.sizeX = (quint32)std::lround(qMax(0.0, v.sizeX) * 1000);
    d.sizeY = (quint32)std::lround(qMax(0.0, v.sizeY) * 1000);
    if (v.masked) {
        d.cropTop    = (quint32)std::lround(v.maskTop * 1000);
        d.cropBottom = (quint32)std::lround(v.maskBottom * 1000);
        d.cropLeft   = (quint32)std::lround(v.maskLeft * 1000);
        d.cropRight  = (quint32)std::lround(v.maskRight * 1000);
    }
    d.border = v.borderEnabled ? (quint32)std::lround(v.borderWidth * 3.125) : 0;
    d.borderArgb = hslColor(v.borderHue, v.borderSaturation, v.borderLuma, v.borderOpacity).rgba();
    d.rotation = m_rotation * 100;
    d.opacity = (quint32)m_opacity;
    return d;
}

void MainWindow::onRefreshPreview()
{
    if (!m_ready) return;
    emu::SwitcherView v = m_device.view();

    // Program, mixed towards preview while a transition runs.
    QImage pgm = sourceForId(v.program)->currentFrame().convertToFormat(QImage::Format_RGB32);
    if (v.inTransition && v.transitionPosition > 0) {
        QPainter p(&pgm);
        p.setOpacity(v.transitionPosition);
        p.drawImage(pgm.rect(), sourceForId(v.preview)->currentFrame());
    }
    QImage frame = m_compositor.compose(pgm, sourceForId(v.keyFill)->currentFrame(), pipState(v));

    // Fade to black.
    double black = 0;
    if (v.fadeFullyBlack) black = 1;
    else if (v.fadeInTransition && v.fadeRate > 0) {
        double remaining = qBound(0.0, double(v.fadeFramesRemaining) / v.fadeRate, 1.0);
        black = m_fadeFromBlack ? remaining : 1 - remaining;
    }
    if (black > 0) {
        QPainter p(&frame);
        p.fillRect(frame.rect(), QColor(0, 0, 0, qRound(black * 255)));
    }

    m_preview->setFrame(frame);
    if (m_webcamActive) pushWebcamFrame(frame);
    updateSourceThumbs();
}

// ── Network binding ───────────────────────────────────────────────────────────

void MainWindow::onNetworkToggle(bool on)
{
    if (on == m_netActive) return;
    if (on) {
        QString error;
        m_netActive = m_server.listen(QHostAddress(m_options.listenAddress), 9910, &error);
        if (m_netActive) {
            m_statusLabel->setText(QString("  Listening  ·  UDP %1:9910").arg(m_options.listenAddress));
            m_statusLabel->setStyleSheet("color:#333;font-size:10px;");
            uiLog("Network binding started");
        } else {
            m_statusLabel->setText("  ✗  UDP port 9910 in use — close other instances");
            m_statusLabel->setStyleSheet("color:#cc4444;font-size:10px;");
            uiLog("Network: failed to bind UDP port 9910: " + error);
        }
    } else {
        m_server.close();
        m_netActive = false;
        m_statusLabel->setText("  Network  ·  OFF");
        m_statusLabel->setStyleSheet("color:#555;font-size:10px;");
        uiLog("Network binding stopped");
    }
    updateNetButtons();
}

// ── Status ────────────────────────────────────────────────────────────────────

void MainWindow::onClientCount(int n)
{
    if (n == 0) {
        m_statusLabel->setText(QString("  No clients  ·  UDP %1:9910").arg(m_options.listenAddress));
        m_statusLabel->setStyleSheet("color:#333;font-size:10px;");
    } else {
        m_statusLabel->setText(QString("  ●  %1 client%2  ·  UDP %3:9910")
            .arg(n).arg(n!=1?"s":"").arg(m_options.listenAddress));
        m_statusLabel->setStyleSheet("color:#338833;font-size:10px;");
    }
    uiLog(QString("Clients connected: %1").arg(n));
}

void MainWindow::onLogMessage(const QString& msg)
{
    appendLog(m_netLogView, msg);
    LOG_NET(msg);
}

// ── Source helpers ────────────────────────────────────────────────────────────

InputSource* MainWindow::sourceForId(quint16 id) const
{
    int c = -1;
    switch (id) {
    case Atem::SRC_CAM1: c=0; break; case Atem::SRC_CAM2: c=1; break;
    case Atem::SRC_CAM3: c=2; break; case Atem::SRC_CAM4: c=3; break;
    default: break;
    }
    if (c >= 0) {
        if (m_usePhoto[c]) return m_photoSources[c];
        if (m_useVideo[c]) return (InputSource*)m_videoSources[c];
        return m_sources[c];
    }
    static BarsSource    bars;
    static SolidColorSource blk(Qt::black);
    if (id == Atem::SRC_COLOR1 || id == Atem::SRC_COLOR2) {   // the switcher's colour generators
        static SolidColorSource colors[2];
        int i = id - Atem::SRC_COLOR1;
        double h, s, l;
        if (m_device.colorGenerator(i + 1, &h, &s, &l)) {
            QColor col = hslColor(h, s, l);
            if (colors[i].color() != col) colors[i].setColor(col);
        }
        return &colors[i];
    }
    return (id == Atem::SRC_BARS) ? (InputSource*)&bars : (InputSource*)&blk;
}
