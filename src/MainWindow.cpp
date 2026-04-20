#include "MainWindow.h"
#include "vcam_shared.h"
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

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_server(&m_state)
    , m_macroEngine(&m_state, this)
{
    setWindowTitle("ATEM Mini Emulator");
    setStyleSheet(kSS);
    resize(1080, 640);
    setMinimumSize(900, 520);

    for (int i = 0; i < 4; ++i) {
        m_sources[i]      = new SolidColorSource(kCamColors[i], this);
        m_videoSources[i] = new VideoFileSource(this);
        m_photoSources[i] = new StaticImageSource(this);
    }

    auto addDemo = [&](int i, const char* n, const char* d,
                       Atem::MacroActionType t, int p) {
        m_state.macros[i] = { n, d, true, {{ t, p }}, {} };
    };
    addDemo(0, "Cam 1",   "Switch to Camera 1", Atem::MacroActionType::SwitchProgram, Atem::SRC_CAM1);
    addDemo(1, "Cam 2",   "Switch to Camera 2", Atem::MacroActionType::SwitchProgram, Atem::SRC_CAM2);
    addDemo(2, "PiP On",  "Enable PiP",         Atem::MacroActionType::KeyerEnable,   1);
    addDemo(3, "PiP Off", "Disable PiP",        Atem::MacroActionType::KeyerEnable,   0);

    buildUi();

    connect(&m_server, &Atem::AtemServer::cmdProgramInput,   this, &MainWindow::onCmdProgramInput);
    connect(&m_server, &Atem::AtemServer::cmdPreviewInput,   this, [this](quint16){});
    connect(&m_server, &Atem::AtemServer::cmdCut,            this, &MainWindow::onCmdCut);
    connect(&m_server, &Atem::AtemServer::cmdAuto,           this, &MainWindow::onCmdCut);
    connect(&m_server, &Atem::AtemServer::cmdKeyerOn,        this, &MainWindow::onCmdKeyerOn);
    connect(&m_server, &Atem::AtemServer::cmdKeyerDVE,       this, &MainWindow::onCmdKeyerDVE);
    connect(&m_server, &Atem::AtemServer::cmdMacroRun,       this, &MainWindow::onCmdMacroRun);
    connect(&m_server, &Atem::AtemServer::cmdMacroStop,      this, &MainWindow::onCmdMacroStop);
    connect(&m_server, &Atem::AtemServer::clientConnected,   this, &MainWindow::onClientConnected);
    connect(&m_server, &Atem::AtemServer::clientDisconnected,this, &MainWindow::onClientDisconnected);
    connect(&m_server, &Atem::AtemServer::logMessage,        this, &MainWindow::onLogMessage);
    connect(&m_macroEngine, &MacroEngine::macroStarted,      this, &MainWindow::onMacroStarted);
    connect(&m_macroEngine, &MacroEngine::macroFinished,     this, &MainWindow::onMacroFinished);
    connect(&m_macroEngine, &MacroEngine::applyAction,       this, &MainWindow::onMacroActionApply);

    m_refreshTimer = new QTimer(this);
    connect(m_refreshTimer, &QTimer::timeout, this, &MainWindow::onRefreshPreview);
    m_refreshTimer->start(33);

    m_netActive = m_server.start();
    if (!m_netActive) {
        m_statusLabel->setText("  \u2717  UDP port 9910 in use \u2014 close other instances");
        m_statusLabel->setStyleSheet("color:#cc4444;font-size:10px;");
        uiLog("ERROR: Failed to bind UDP port 9910");
    } else {
        m_statusLabel->setText(
            QString("  Listening  \u00b7  UDP 0.0.0.0:%1").arg(Atem::ATEM_PORT));
    }
    updateVCamButtons();
    updateNetButtons();

    syncMacroList(); syncKeyerUi(); syncProgramButtons();
}

MainWindow::~MainWindow()
{
    m_server.stop();
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

void MainWindow::uiLog(const QString& msg) { appendLog(m_uiLogView, msg); }

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

QWidget* MainWindow::buildDveSection()
{
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
        m_state.dve.sizeX = (quint32)(v * 10);
        if (m_lockSize->isChecked()) {
            m_state.dve.sizeY = m_state.dve.sizeX;
            m_sizeYSpin->blockSignals(true); m_sizeYSpin->setValue(v); m_sizeYSpin->blockSignals(false);
        }
        m_server.broadcastKeDV();
    });
    connect(m_sizeYSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v){
        m_state.dve.sizeY = (quint32)(v * 10);
        if (m_lockSize->isChecked()) {
            m_state.dve.sizeX = m_state.dve.sizeY;
            m_sizeXSpin->blockSignals(true); m_sizeXSpin->setValue(v); m_sizeXSpin->blockSignals(false);
        }
        m_server.broadcastKeDV();
    });
    onBlur(m_sizeXSpin, [this]{ uiLog(QString("PiP Size X: %1%").arg(m_state.dve.sizeX/10)); });
    onBlur(m_sizeYSpin, [this]{ uiLog(QString("PiP Size Y: %1%").arg(m_state.dve.sizeY/10)); });

    lg->addWidget(rl("Size", 52), r, 0);
    lg->addWidget(rl("X"),  r, 1); lg->addWidget(m_sizeXSpin, r, 2);
    lg->addWidget(rl("Y"),  r, 3); lg->addWidget(m_sizeYSpin, r, 4);
    lg->addWidget(m_lockSize, r, 5); ++r;

    m_posXSpin = makeSpin(-1600, 1600, 0);
    m_posYSpin = makeSpin(-900,   900, 0);
    connect(m_posXSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int v){ m_state.dve.posX = v * 10; m_server.broadcastKeDV(); });
    connect(m_posYSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int v){ m_state.dve.posY = v * 10; m_server.broadcastKeDV(); });
    onBlur(m_posXSpin, [this]{ uiLog(QString("PiP Pos X: %1").arg(m_state.dve.posX)); });
    onBlur(m_posYSpin, [this]{ uiLog(QString("PiP Pos Y: %1").arg(m_state.dve.posY)); });

    lg->addWidget(rl("Position", 52), r, 0);
    lg->addWidget(rl("X"), r, 1); lg->addWidget(m_posXSpin, r, 2);
    lg->addWidget(rl("Y"), r, 3); lg->addWidget(m_posYSpin, r, 4); ++r;

    m_rotationSpin = makeSpin(0, 359, 0, "\xc2\xb0");
    m_opacitySpin  = makeSpin(0, 100, 100, "%");
    connect(m_rotationSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int v){ m_state.dve.rotation = v * 100; m_server.broadcastKeDV(); });
    connect(m_opacitySpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int v){ m_state.dve.opacity = (quint32)v; m_server.broadcastKeDV(); });
    onBlur(m_rotationSpin, [this]{ uiLog(QString("PiP Rotation: %1°").arg(m_state.dve.rotation/100)); });
    onBlur(m_opacitySpin,  [this]{ uiLog(QString("PiP Opacity: %1%").arg(m_state.dve.opacity)); });

    lg->addWidget(rl("Rotation", 52), r, 0);
    lg->addWidget(rl(""),         r, 1); lg->addWidget(m_rotationSpin, r, 2);
    lg->addWidget(rl("Opacity"),  r, 3); lg->addWidget(m_opacitySpin, r, 4); ++r;

    lg->setColumnStretch(6, 1);

    // ── RIGHT column: Border / Crop ───────────────────────────────────────────
    auto* rg = new QGridLayout;
    rg->setHorizontalSpacing(5); rg->setVerticalSpacing(7); rg->setContentsMargins(0,0,0,0);

    r = 0;

    m_borderSpin     = makeSpin(0, 50, 0, "px");
    m_borderColorBtn = new QPushButton;
    m_borderColorBtn->setFixedSize(28, 28);
    m_borderColorBtn->setToolTip("Border colour");
    connect(m_borderSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int v){ m_state.dve.border = (quint32)v; m_server.broadcastKeDV(); });
    connect(m_borderColorBtn, &QPushButton::clicked, this, &MainWindow::onKeyerBorderColorPick);
    onBlur(m_borderSpin, [this]{ uiLog(QString("PiP Border: %1px").arg(m_state.dve.border)); });
    updateBorderColorBtn();

    rg->addWidget(rl("Border", 44), r, 0);
    rg->addWidget(rl(""),           r, 1); rg->addWidget(m_borderSpin,     r, 2);
    rg->addWidget(m_borderColorBtn, r, 3); ++r;

    m_cropLSpin = makeSpin(0, 50, 0, "%");
    m_cropRSpin = makeSpin(0, 50, 0, "%");
    connect(m_cropLSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int v){ m_state.dve.cropLeft  = v; m_server.broadcastKeDV(); });
    connect(m_cropRSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int v){ m_state.dve.cropRight = v; m_server.broadcastKeDV(); });
    onBlur(m_cropLSpin, [this]{ uiLog(QString("PiP Crop L: %1%").arg(m_state.dve.cropLeft));  });
    onBlur(m_cropRSpin, [this]{ uiLog(QString("PiP Crop R: %1%").arg(m_state.dve.cropRight)); });

    rg->addWidget(rl("Crop", 44), r, 0);
    rg->addWidget(rl("L"),        r, 1); rg->addWidget(m_cropLSpin, r, 2);
    rg->addWidget(rl("R"),        r, 3); rg->addWidget(m_cropRSpin, r, 4); ++r;

    m_cropTSpin = makeSpin(0, 50, 0, "%");
    m_cropBSpin = makeSpin(0, 50, 0, "%");
    connect(m_cropTSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int v){ m_state.dve.cropTop    = v; m_server.broadcastKeDV(); });
    connect(m_cropBSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int v){ m_state.dve.cropBottom = v; m_server.broadcastKeDV(); });
    onBlur(m_cropTSpin, [this]{ uiLog(QString("PiP Crop T: %1%").arg(m_state.dve.cropTop));    });
    onBlur(m_cropBSpin, [this]{ uiLog(QString("PiP Crop B: %1%").arg(m_state.dve.cropBottom)); });

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

// ── Slots ─────────────────────────────────────────────────────────────────────

void MainWindow::setProgramSource(quint16 src)
{
    m_state.programSource = src; m_state.previewSource = src;
    syncProgramButtons();
    m_server.broadcastPrgI(); m_server.broadcastPrvI(); m_server.broadcastTally();
}

void MainWindow::onProgramButton(int id)
{
    const char* name = "?";
    for (int i = 0; i < kN; ++i) if (kSrc[i].id == id) { name = kSrc[i].full; break; }
    uiLog(QString("PGM \u2192 %1").arg(name));
    setProgramSource((quint16)id);
}

void MainWindow::syncProgramButtons()
{
    for (int i = 0; i < kN; ++i)
        if (m_pgmBtns[i]) m_pgmBtns[i]->setActive(kSrc[i].id == m_state.programSource);
}

void MainWindow::onFillBtn(int idx)
{
    quint16 src = kSrc[idx].id;
    if (m_state.dve.enabled && m_state.dve.fillSrc == src) {
        m_state.dve.enabled = false; m_state.keyerOn = false;
        uiLog("PiP: Off");
    } else {
        m_state.dve.enabled = true; m_state.keyerOn = true; m_state.dve.fillSrc = src;
        uiLog(QString("PiP Fill \u2192 %1").arg(kSrc[idx].full));
    }
    syncKeyerUi(); m_server.broadcastKeOn(); m_server.broadcastKeDV();
}

void MainWindow::onKeyerBorderColorPick()
{
    QColor c = QColorDialog::getColor(QColor::fromRgba(m_state.dve.borderArgb),
                                      this, "PiP Border Colour");
    if (!c.isValid()) return;
    m_state.dve.borderArgb = c.rgba();
    updateBorderColorBtn(); m_server.broadcastKeDV();
    uiLog(QString("PiP Border Colour: %1").arg(c.name()));
}

void MainWindow::updateBorderColorBtn()
{
    if (!m_borderColorBtn) return;
    QColor c = QColor::fromRgba(m_state.dve.borderArgb);
    m_borderColorBtn->setStyleSheet(
        QString("QPushButton{background:%1;border:1px solid %2;border-radius:3px;}"
                "QPushButton:hover{background:%3;}")
        .arg(c.name(), c.lighter(130).name(), c.lighter(115).name()));
}

void MainWindow::syncKeyerUi()
{
    auto blk = [](QSpinBox* w, int v){ if(w){w->blockSignals(true);w->setValue(v);w->blockSignals(false);} };

    for (int i = 0; i < kN; ++i)
        if (m_fillBtns[i])
            m_fillBtns[i]->setActive(m_state.dve.enabled && m_state.dve.fillSrc == kSrc[i].id);

    blk(m_sizeXSpin, (int)m_state.dve.sizeX/10);
    blk(m_sizeYSpin, (int)m_state.dve.sizeY/10);
    blk(m_posXSpin,  m_state.dve.posX/10);
    blk(m_posYSpin,  m_state.dve.posY/10);
    blk(m_rotationSpin, m_state.dve.rotation/100);
    blk(m_borderSpin,   (int)m_state.dve.border);
    blk(m_opacitySpin,  (int)m_state.dve.opacity);
    blk(m_cropLSpin,    (int)m_state.dve.cropLeft);
    blk(m_cropRSpin,    (int)m_state.dve.cropRight);
    blk(m_cropTSpin,    (int)m_state.dve.cropTop);
    blk(m_cropBSpin,    (int)m_state.dve.cropBottom);
    updateBorderColorBtn();
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
    uiLog(QString("CAM%1 colour \u2192 %2").arg(i+1).arg(c.name()));
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
        uiLog(QString("CAM%1 photo \u2192 %2").arg(i+1).arg(fn));
    } else {
        m_videoSources[i]->loadFile(path); m_useVideo[i] = true; m_usePhoto[i] = false;
        uiLog(QString("CAM%1 video \u2192 %2").arg(i+1).arg(fn));
    }
    if (m_pgmMediaBtn[i]) m_pgmMediaBtn[i]->setText(fn.left(8));
}

// ── Macros ────────────────────────────────────────────────────────────────────

void MainWindow::onMacroRun()
{
    auto* sel = m_macroList->currentItem(); if (!sel) return;
    int slot = sel->data(Qt::UserRole).toInt();
    uiLog(QString("Macro run: \"%1\"").arg(m_state.macros[slot].name));
    applyMacroSnapshot(slot);   // restore full DVE state, size, border, lock, inputs
    m_macroEngine.runMacro(slot);
}

void MainWindow::onMacroUpdate()
{
    auto* sel = m_macroList->currentItem(); if (!sel) return;
    int slot = sel->data(Qt::UserRole).toInt();
    m_state.macros[slot].name        = m_macroNameEdit->text().trimmed();
    m_state.macros[slot].description = m_macroDescEdit->toPlainText().trimmed();
    m_state.macros[slot].isUsed      = true;
    m_server.broadcastMPrp(slot);
    syncMacroList();
    for (int i = 0; i < m_macroList->count(); ++i)
        if (m_macroList->item(i)->data(Qt::UserRole).toInt() == slot)
            { m_macroList->setCurrentRow(i); break; }
    uiLog(QString("Macro %1 updated: \"%2\"").arg(slot+1).arg(m_state.macros[slot].name));
}

void MainWindow::onMacroSaveOutput()
{
    auto* sel = m_macroList->currentItem();
    if (!sel) { uiLog("Save Output: no macro selected"); return; }
    int slot = sel->data(Qt::UserRole).toInt();

    QVector<Atem::MacroAction> acts;
    acts.append({ Atem::MacroActionType::SwitchProgram, (int)m_state.programSource });
    acts.append({ Atem::MacroActionType::KeyerEnable,   m_state.keyerOn ? 1 : 0 });
    m_state.macros[slot].actions = acts;

    Atem::MacroSnapshot snap;
    snap.captured = true; snap.programSource = m_state.programSource;
    snap.dve = m_state.dve;
    snap.lockSize = m_lockSize ? m_lockSize->isChecked() : true;
    for (int i = 0; i < 4; ++i) {
        if (m_useVideo[i])       snap.inputs[i] = { Atem::InputMode::Video, 0, m_videoSources[i]->filePath() };
        else if (m_usePhoto[i])  snap.inputs[i] = { Atem::InputMode::Photo, 0, m_photoSources[i]->path() };
        else                     snap.inputs[i] = { Atem::InputMode::SolidColor, m_sources[i]->color().rgba(), {} };
    }
    m_state.macros[slot].snapshot = snap;
    m_state.macros[slot].isUsed   = true;
    m_server.broadcastMPrp(slot);

    QImage frame = m_compositor.compose(
        sourceForId(m_state.programSource)->currentFrame(),
        sourceForId(m_state.dve.fillSrc)->currentFrame(), m_state.dve);
    frame.scaled(320,180, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
         .save(macroThumbPath(slot), "JPEG", 80);

    syncMacroList();
    for (int i = 0; i < m_macroList->count(); ++i)
        if (m_macroList->item(i)->data(Qt::UserRole).toInt() == slot)
            { m_macroList->setCurrentRow(i); break; }
    updateSnapshotDisplay(slot);

    const char* pgmName = "?";
    for (int i = 0; i < kN; ++i)
        if (kSrc[i].id == m_state.programSource) { pgmName = kSrc[i].full; break; }
    uiLog(QString("Macro \"%1\" saved: PGM=%2, PiP=%3")
          .arg(m_state.macros[slot].name).arg(pgmName)
          .arg(m_state.keyerOn ? "On" : "Off"));

    m_macroStatus->setText("\u2713 Saved");
    m_macroStatus->setStyleSheet("color:#44bb44;font-weight:700;font-size:10px;"
                                 "letter-spacing:1px;background:transparent;border:0;");
    QTimer::singleShot(2000, this, [this]{
        m_macroStatus->setText("IDLE");
        m_macroStatus->setStyleSheet("color:#333;font-weight:700;font-size:10px;"
                                     "letter-spacing:1px;background:transparent;border:0;");
    });
}

void MainWindow::onMacroSelectionChanged()
{
    auto* sel = m_macroList->currentItem(); if (!sel) return;
    int slot = sel->data(Qt::UserRole).toInt();
    m_macroNameEdit->setText(m_state.macros[slot].name);
    m_macroDescEdit->setPlainText(m_state.macros[slot].description);
    updateSnapshotDisplay(slot);
}

void MainWindow::applyMacroSnapshot(int slot)
{
    const auto& snap = m_state.macros[slot].snapshot;
    if (!snap.captured) return;
    setProgramSource(snap.programSource);
    m_state.dve = snap.dve; m_state.keyerOn = snap.dve.enabled;
    if (m_lockSize) m_lockSize->setChecked(snap.lockSize);
    syncKeyerUi(); m_server.broadcastKeOn(); m_server.broadcastKeDV();
    for (int i = 0; i < 4; ++i) {
        const auto& inp = snap.inputs[i];
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
    uiLog(QString("Macro %1 loaded: \"%2\"").arg(slot+1).arg(m_state.macros[slot].name));
}

void MainWindow::updateSnapshotDisplay(int slot)
{
    if (!m_snapshotView) return;
    const auto& mac = m_state.macros[slot];
    if (!mac.snapshot.captured) { m_snapshotView->setPlainText("(no saved state)"); return; }
    const auto& s = mac.snapshot;
    QString txt;
    auto srcName = [&](quint16 id) -> QString {
        for (int i = 0; i < kN; ++i) if (kSrc[i].id == id) return kSrc[i].full; return "?";
    };
    txt += "PGM: " + srcName(s.programSource) + "\n";
    txt += QString("PiP: %1").arg(s.dve.enabled ? "On" : "Off");
    if (s.dve.enabled) txt += " (" + srcName(s.dve.fillSrc) + ")";
    txt += "\n";
    txt += QString("Size %1%  Pos %2,%3\n").arg(s.dve.sizeX/10).arg(s.dve.posX).arg(s.dve.posY);
    txt += QString("Rot %1\xc2\xb0  Opa %2%  Bdr %3px\n").arg(s.dve.rotation/100).arg(s.dve.opacity).arg(s.dve.border);
    txt += QString("Crop L%1 R%2 T%3 B%4\n").arg(s.dve.cropLeft).arg(s.dve.cropRight).arg(s.dve.cropTop).arg(s.dve.cropBottom);
    txt += "─────────────\n";
    for (int i = 0; i < 4; ++i) {
        const auto& inp = s.inputs[i];
        QString val = (inp.mode == Atem::InputMode::SolidColor)
            ? QColor::fromRgba(inp.argb).name()
            : ((inp.mode == Atem::InputMode::Photo ? "img:" : "vid:") + QFileInfo(inp.path).fileName().left(16));
        txt += QString("CAM%1: %2\n").arg(i+1).arg(val);
    }
    m_snapshotView->setPlainText(txt.trimmed());
}

void MainWindow::syncMacroList()
{
    int selSlot = m_macroList->currentItem()
        ? m_macroList->currentItem()->data(Qt::UserRole).toInt() : -1;
    m_macroList->clear();
    for (int i = 0; i < 20; ++i) {
        const auto& mac = m_state.macros[i];
        auto* item = new QListWidgetItem;
        item->setData(Qt::DisplayRole, mac.name.isEmpty() ? QString("Slot %1").arg(i+1) : mac.name);
        item->setData(Qt::UserRole, i);
        item->setData(Qt::UserRole+1, mac.description);
        if (QFile::exists(macroThumbPath(i))) item->setIcon(QIcon(macroThumbPath(i)));
        m_macroList->addItem(item);
    }
    for (int i = 0; i < m_macroList->count(); ++i)
        if (m_macroList->item(i)->data(Qt::UserRole).toInt() == selSlot)
            { m_macroList->setCurrentRow(i); break; }
}

void MainWindow::onMacroStarted(int index)
{
    m_state.macroRun = { true, false, (quint16)index };
    m_server.broadcastMRPr();
    m_macroStatus->setText(QString("\u25b6  %1").arg(m_state.macros[index].name));
    m_macroStatus->setStyleSheet("color:#44bb44;font-weight:700;font-size:10px;"
                                 "letter-spacing:1px;background:transparent;border:0;");
}

void MainWindow::onMacroFinished(int)
{
    m_state.macroRun = {};
    m_server.broadcastMRPr();
    m_macroStatus->setText("IDLE");
    m_macroStatus->setStyleSheet("color:#333;font-weight:700;font-size:10px;"
                                 "letter-spacing:1px;background:transparent;border:0;");
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

void MainWindow::onRefreshPreview()
{
    QImage frame = m_compositor.compose(
        sourceForId(m_state.programSource)->currentFrame(),
        sourceForId(m_state.dve.fillSrc)->currentFrame(),
        m_state.dve);
    m_preview->setFrame(frame);
    if (m_webcamActive) pushWebcamFrame(frame);
    updateSourceThumbs();
}

// ── Remote commands ───────────────────────────────────────────────────────────

void MainWindow::onMacroActionApply(const Atem::MacroAction& a)
{
    using T = Atem::MacroActionType;
    switch (a.type) {
    case T::SwitchProgram: setProgramSource((quint16)a.param); break;
    case T::KeyerEnable:
        m_state.keyerOn = (a.param != 0); m_state.dve.enabled = m_state.keyerOn;
        syncKeyerUi(); m_server.broadcastKeOn(); m_server.broadcastKeDV(); break;
    default: break;
    }
}

void MainWindow::onCmdProgramInput(quint16 s) { uiLog(QString("Remote PGM \u2192 src %1").arg(s)); setProgramSource(s); }
void MainWindow::onCmdCut() {}

void MainWindow::onCmdKeyerOn(bool on)
{
    uiLog(QString("Remote PiP: %1").arg(on ? "On" : "Off"));
    m_state.keyerOn = on; m_state.dve.enabled = on;
    syncKeyerUi(); m_server.broadcastKeOn(); m_server.broadcastKeDV();
}

void MainWindow::onCmdKeyerDVE(quint16 fs, quint32 sx, quint32 sy, qint32 px, qint32 py)
{
    uiLog("Remote DVE update");
    m_state.dve.fillSrc = fs; m_state.dve.sizeX = sx; m_state.dve.sizeY = sy;
    m_state.dve.posX = px; m_state.dve.posY = py;
    syncKeyerUi(); m_server.broadcastKeDV();
}

void MainWindow::onCmdMacroRun(quint16 i) { uiLog(QString("Remote macro run: slot %1").arg(i)); m_macroEngine.runMacro(i); }
void MainWindow::onCmdMacroStop() { m_macroEngine.stopMacro(); }

// ── Network binding ───────────────────────────────────────────────────────────

void MainWindow::onNetworkToggle(bool on)
{
    if (on == m_netActive) return;
    if (on) {
        m_netActive = m_server.start();
        if (m_netActive) {
            m_statusLabel->setText(
                QString("  Listening  \u00b7  UDP 0.0.0.0:%1").arg(Atem::ATEM_PORT));
            m_statusLabel->setStyleSheet("color:#333;font-size:10px;");
            uiLog("Network binding started");
        } else {
            uiLog("Network: failed to bind UDP port 9910");
        }
    } else {
        m_server.stop();
        m_netActive = false;
        m_statusLabel->setText("  Network  \u00b7  OFF");
        m_statusLabel->setStyleSheet("color:#555;font-size:10px;");
        uiLog("Network binding stopped");
    }
    updateNetButtons();
}

// ── Status ────────────────────────────────────────────────────────────────────

void MainWindow::onClientConnected(int n)
{
    m_statusLabel->setText(QString("  \u25cf  %1 client%2  \u00b7  UDP 0.0.0.0:%3")
        .arg(n).arg(n!=1?"s":"").arg(Atem::ATEM_PORT));
    m_statusLabel->setStyleSheet("color:#338833;font-size:10px;");
    uiLog(QString("Client connected (total: %1)").arg(n));
}

void MainWindow::onClientDisconnected(int n)
{
    if (n == 0) {
        m_statusLabel->setText(QString("  No clients  \u00b7  UDP 0.0.0.0:%1").arg(Atem::ATEM_PORT));
        m_statusLabel->setStyleSheet("color:#333;font-size:10px;");
    } else { onClientConnected(n); }
}

void MainWindow::onLogMessage(const QString& msg) { appendLog(m_netLogView, msg); }

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
    return (id == Atem::SRC_BARS) ? (InputSource*)&bars : (InputSource*)&blk;
}

int MainWindow::sourceIdToComboIndex(quint16 id) const
{
    for (int i = 0; i < kN; ++i) if (kSrc[i].id == id) return i;
    return 1;
}
