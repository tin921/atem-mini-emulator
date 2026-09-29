#include "window.h"
#include "widgets.h"
#include "../src/groups.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#ifndef SWEEP_BACKUP_DIR
#define SWEEP_BACKUP_DIR "backups"
#endif
#ifndef SWEEP_GOLDEN_DIR
#define SWEEP_GOLDEN_DIR "golden"
#endif
#ifndef SWEEP_COVERAGE_DIR
#define SWEEP_COVERAGE_DIR "coverage"
#endif
#ifndef EMU_EXE
#define EMU_EXE "atem-emu.exe"
#endif
#ifndef EMU_PROFILES
#define EMU_PROFILES "profiles"
#endif

namespace {

constexpr const char* kEmulatorAddress = "127.0.0.2";

// The process that owns a UDP port on an IPv4 address (0 if none).
DWORD udpOwner(const char* address, int port) {
    ULONG size = 0;
    GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    QByteArray buf(static_cast<int>(size), '\0');
    auto* table = reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(buf.data());
    if (GetExtendedUdpTable(table, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) != NO_ERROR) return 0;
    in_addr want{};
    inet_pton(AF_INET, address, &want);
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (row.dwLocalAddr == want.s_addr && ntohs(static_cast<u_short>(row.dwLocalPort)) == port) return row.dwOwningPid;
    }
    return 0;
}

QString processName(DWORD pid) {
    QString name;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &n)) name = QFileInfo(QString::fromWCharArray(path, static_cast<int>(n))).fileName();
        CloseHandle(h);
    }
    return name;
}

QIcon swatch(const QColor& c) {
    QPixmap pm(12, 12);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawRoundedRect(QRectF(0, 0, 12, 12), 3, 3);
    return QIcon(pm);
}

QString html(const QString& s) { return s.toHtmlEscaped(); }

// atem-sweep and atem-emu find their Qt DLLs through PATH.
QProcessEnvironment childEnvironment() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
#ifdef QT_BIN_DIR
    env.insert("PATH", QDir::toNativeSeparators(QT_BIN_DIR) + ";" + env.value("PATH"));
#endif
    return env;
}

QString mdCell(QString s) {
    s.replace('|', "\\|");
    s.replace('\n', ' ');
    return s;
}

const char* kStyle = R"(
QMainWindow, #central { background: #f4f2ec; }
QLabel { color: #1d1f22; }
#header { border-bottom: 1px solid #dcd8cf; }
#title { font-size: 20px; font-weight: 600; }
#subtitle, #muted { color: #5f6368; font-size: 13px; }
#pct { font-size: 32px; font-weight: 600; }
#pctLine { color: #5f6368; font-size: 12px; }
QFrame#seg { background: #e8e5dd; border-radius: 10px; }
QPushButton#segBtn { border: 0; border-radius: 7px; padding: 0 16px; min-height: 36px; background: transparent; color: #5f6368; font-size: 13px; font-weight: 500; }
QPushButton#segBtn:checked { background: #fbfaf7; color: #1d1f22; }
QPushButton#run { background: #1d1f22; color: #f4f2ec; border: 0; border-radius: 10px; padding: 0 22px; min-height: 46px; min-width: 130px; font-size: 14px; font-weight: 600; }
QPushButton#run:disabled { background: #8a8f94; }
QPushButton#backup { background: #fbfaf7; color: #1d1f22; border: 1px solid #cfcac0; border-radius: 10px; padding: 4px 16px; min-height: 46px; font-size: 13px; text-align: left; }
QPushButton#backup:checked { background: #ece8df; border-color: #1d1f22; }
QPushButton#backup:disabled { color: #a6a298; background: #ece9e2; border-color: #e0dcd2; }
QPushButton#legend { border: 1px solid transparent; border-radius: 8px; padding: 0 10px; min-height: 34px; background: transparent; color: #8a8f94; font-size: 13px; }
QPushButton#legend:checked { border-color: #cfcac0; background: #fbfaf7; color: #1d1f22; }
QPushButton#report { border: 1px solid #1d1f22; border-radius: 8px; padding: 0 14px; min-height: 34px; background: #fbfaf7; font-size: 13px; font-weight: 600; }
QPushButton#small { border: 1px solid #cfcac0; border-radius: 6px; padding: 4px 10px; background: #fff; font-size: 12px; }
QPushButton#small:disabled { color: #a6a298; background: #f4f2ec; }
QPushButton#backupNow { background: #1d1f22; color: #f4f2ec; border: 0; border-radius: 8px; padding: 0 14px; min-height: 34px; font-size: 13px; font-weight: 600; }
QPushButton#backupNow:disabled { background: #a6a298; }
QPushButton#crumb { border: 0; border-radius: 6px; padding: 0 10px; min-height: 30px; background: #e8e5dd; font-size: 13px; }
QPushButton#crumbLast { border: 0; border-radius: 6px; padding: 0 10px; min-height: 30px; background: #1d1f22; color: #f4f2ec; font-size: 13px; }
QToolButton#back { border: 1px solid #d6d1c6; border-radius: 8px; background: #fff; min-width: 34px; min-height: 34px; font-size: 16px; }
#side { background: #fbfaf7; border-left: 1px solid #dcd8cf; }
#card { border-bottom: 1px solid #dcd8cf; }
#navbar { border-bottom: 1px solid #ece8df; }
#testId { font-family: 'Cascadia Mono', Consolas, monospace; font-size: 14px; font-weight: 500; }
#listTitle { font-size: 14px; font-weight: 600; }
QScrollArea { border: 0; background: transparent; }
#mapHost, #listHost { background: transparent; }
QComboBox { min-height: 32px; padding: 0 8px; border: 1px solid #cfcac0; border-radius: 8px; background: #fff; font-size: 13px; }
QCheckBox { font-size: 14px; font-weight: 600; spacing: 10px; }
)";

} // namespace

Window::Window() {
    setWindowTitle("atem-sweep");
    resize(1440, 900);
    qApp->setStyleSheet(kStyle);
    for (const QString& g : testGroups()) m_groupOn[g] = true;

    auto* central = new QWidget;
    central->setObjectName("central");
    auto* v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(buildHeader());
    m_strip = new ProgressStrip;
    v->addWidget(m_strip);

    auto* body = new QHBoxLayout;
    body->setSpacing(0);
    auto* left = new QVBoxLayout;
    left->setContentsMargins(32, 16, 24, 0);
    left->setSpacing(10);
    m_phases = new PhaseBar;
    m_phases->hide();
    left->addWidget(m_phases);
    left->addWidget(buildLegend());
    auto* mapScroll = new QScrollArea;
    mapScroll->setWidgetResizable(true);
    mapScroll->setFrameShape(QFrame::NoFrame);
    auto* mapHost = new QWidget;
    mapHost->setObjectName("mapHost");
    m_mapLayout = new QVBoxLayout(mapHost);
    m_mapLayout->setContentsMargins(0, 4, 8, 24);
    m_mapLayout->setSpacing(16);
    mapScroll->setWidget(mapHost);
    left->addWidget(mapScroll, 1);
    body->addLayout(left, 1);
    body->addWidget(buildSide());
    v->addLayout(body, 1);
    setCentralWidget(central);

    QString error;
    if (!loadPlan(&error)) {
        QMessageBox::critical(this, "atem-sweep", "Cannot get the list of tests from atem-sweep:\n" + error);
    }
    loadGolden();
    setTarget(true);
    rebuildMap();
    rebuildList();
    refresh();

    auto* clock = new QTimer(this);   // "last backup 5 min ago" keeps counting
    connect(clock, &QTimer::timeout, this, &Window::updateBackupButton);
    clock->start(30000);
}

Window::~Window() = default;

void Window::showInterface(const QString& iface, const QString& prop) {
    m_iface = iface;
    m_prop = prop;
    rebuildMap();
    rebuildList();
}

void Window::selfTest(const QStringList& groups, std::function<void()> done) {
    setTarget(true);
    for (const QString& g : testGroups()) m_groupOn[g] = groups.contains(g);
    rebuildMap();
    m_whenDone = std::move(done);
    run();
    if (!busy() && m_whenDone) m_whenDone();
}

void Window::showBackups() {
    setTarget(false);
    m_backupBtn->setChecked(true);
    m_side = Side::Backups;
    QList<BackupGroup> groups = ui::backupGroups(SWEEP_BACKUP_DIR);
    if (!groups.isEmpty()) m_openGroups.insert(groups.first().key);
    rebuildList();
}

void Window::closeEvent(QCloseEvent* e) {
    if (busy()) {
        auto answer = QMessageBox::question(this, "atem-sweep",
            "A run is still going. Stop it (it puts the settings back first) and close when it has finished?");
        if (answer == QMessageBox::Yes) stop();
        e->ignore();
        return;
    }
    if (m_emu && m_emu->state() != QProcess::NotRunning) {
        m_emu->kill();
        m_emu->waitForFinished(2000);
    }
    e->accept();
}

// ── Setup ────────────────────────────────────────────────────

QWidget* Window::buildHeader() {
    auto* header = new QWidget;
    header->setObjectName("header");
    auto* h = new QHBoxLayout(header);
    h->setContentsMargins(32, 12, 32, 12);
    h->setSpacing(18);

    auto* titles = new QVBoxLayout;
    titles->setSpacing(2);
    auto* title = new QLabel("Sweep coverage");
    title->setObjectName("title");
    m_subtitle = new QLabel;
    m_subtitle->setObjectName("subtitle");
    m_subtitle->setMinimumWidth(200);
    m_subtitle->setWordWrap(true);
    titles->addWidget(title);
    titles->addWidget(m_subtitle);
    h->addLayout(titles, 1);

    m_pct = new QLabel("0%");
    m_pct->setObjectName("pct");
    h->addWidget(m_pct);
    auto* lines = new QVBoxLayout;
    lines->setSpacing(0);
    m_pct1 = new QLabel;
    m_pct2 = new QLabel;
    m_pct1->setObjectName("pctLine");
    m_pct2->setObjectName("pctLine");
    lines->addWidget(m_pct1);
    lines->addWidget(m_pct2);
    h->addLayout(lines);

    auto* seg = new QFrame;
    seg->setObjectName("seg");
    auto* sl = new QHBoxLayout(seg);
    sl->setContentsMargins(4, 4, 4, 4);
    sl->setSpacing(4);
    m_emuBtn = new QPushButton("Emulator");
    m_atemBtn = new QPushButton("Real ATEM");
    auto* group = new QButtonGroup(this);
    for (QPushButton* b : { m_emuBtn, m_atemBtn }) {
        b->setObjectName("segBtn");
        b->setCheckable(true);
        group->addButton(b);
        sl->addWidget(b);
    }
    connect(m_emuBtn, &QPushButton::clicked, this, [this]() { setTarget(true); });
    connect(m_atemBtn, &QPushButton::clicked, this, [this]() { setTarget(false); });
    h->addWidget(seg);

    m_golden = new QComboBox;
    m_golden->setToolTip("The golden record the emulator is checked against");
    for (const QFileInfo& fi : QDir(SWEEP_GOLDEN_DIR).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        if (QFile::exists(fi.filePath() + "/results.json")) m_golden->addItem(fi.fileName(), fi.filePath());
    m_golden->setCurrentIndex(m_golden->count() - 1);
    connect(m_golden, &QComboBox::currentIndexChanged, this, [this]() { loadGolden(); refresh(); });
    h->addWidget(m_golden);

    m_address = new QComboBox;
    m_address->setEditable(true);
    m_address->addItems({ "192.168.0.240", "usb" });
    m_address->setToolTip("The ATEM's address. Over USB there is no wire capture and no verified backup, "
                          "so the storage tests are skipped.");
    m_address->setMinimumWidth(150);
    h->addWidget(m_address);

    m_runBtn = new QPushButton("Run sweep");
    m_runBtn->setObjectName("run");
    connect(m_runBtn, &QPushButton::clicked, this, [this]() { m_job == Job::Sweep ? stop() : run(); });
    h->addWidget(m_runBtn);

    m_backupBtn = new QPushButton;
    m_backupBtn->setObjectName("backup");
    m_backupBtn->setCheckable(true);
    connect(m_backupBtn, &QPushButton::clicked, this, [this](bool on) {
        m_side = on ? Side::Backups : Side::Tests;
        rebuildList();
    });
    h->addWidget(m_backupBtn);
    return header;
}

QWidget* Window::buildLegend() {
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 8, 0);
    h->setSpacing(8);
    auto legend = [&](QPushButton*& b, St s, bool Model::*flag) {
        b = new QPushButton;
        b->setObjectName("legend");
        b->setCheckable(true);
        b->setChecked(true);
        b->setIcon(swatch(ui::color(s)));
        connect(b, &QPushButton::toggled, this, [this, flag](bool on) {
            m_model.*flag = on;
            refresh();
        });
        h->addWidget(b);
    };
    legend(m_showPass, St::Pass, &Model::showPass);
    legend(m_showFail, St::Fail, &Model::showFail);
    legend(m_showSkip, St::Skip, &Model::showSkip);
    m_notRun = new QLabel;
    m_notRun->setObjectName("muted");
    auto* dot = new QLabel;
    dot->setPixmap(swatch(ui::color(St::Queued)).pixmap(12, 12));
    h->addSpacing(6);
    h->addWidget(dot);
    h->addWidget(m_notRun);
    h->addStretch(1);
    auto* report = new QPushButton("Download report");
    report->setObjectName("report");
    report->setToolTip("Every test with its source, values and SDK functions, as a Markdown file");
    connect(report, &QPushButton::clicked, this, &Window::saveReport);
    h->addWidget(report);
    return w;
}

QWidget* Window::buildSide() {
    auto* side = new QWidget;
    side->setObjectName("side");
    side->setFixedWidth(430);
    auto* v = new QVBoxLayout(side);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    m_card = new QWidget;
    m_card->setObjectName("card");
    auto* c = new QVBoxLayout(m_card);
    c->setContentsMargins(24, 18, 24, 16);
    c->setSpacing(6);
    auto* top = new QHBoxLayout;
    m_badge = new QLabel;
    m_testNo = new QLabel;
    m_testNo->setObjectName("muted");
    top->addWidget(m_badge);
    top->addWidget(m_testNo);
    top->addStretch(1);
    c->addLayout(top);
    m_testId = new QLabel;
    m_testId->setObjectName("testId");
    m_testId->setWordWrap(true);
    m_testId->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_testTitle = new QLabel;
    m_testTitle->setWordWrap(true);
    m_testTitle->setStyleSheet("color:#45484c; font-size:13px");
    m_values = new QLabel;
    m_values->setWordWrap(true);
    m_values->setTextFormat(Qt::RichText);
    m_values->setTextInteractionFlags(Qt::TextSelectableByMouse);
    c->addWidget(m_testId);
    c->addWidget(m_testTitle);
    c->addWidget(m_values);
    v->addWidget(m_card);

    auto* nav = new QWidget;
    nav->setObjectName("navbar");
    auto* n = new QHBoxLayout(nav);
    n->setContentsMargins(16, 10, 16, 10);
    n->setSpacing(10);
    m_back = new QToolButton;
    m_back->setObjectName("back");
    m_back->setText(QString::fromUtf8("‹"));
    m_back->setToolTip("Back");
    connect(m_back, &QToolButton::clicked, this, [this]() {
        if (m_side == Side::Backups) {
            m_side = Side::Tests;
            m_backupBtn->setChecked(false);
        } else if (!m_prop.isEmpty()) {
            m_prop.clear();
        } else {
            m_iface.clear();
        }
        setHover({});
        rebuildMap();
        rebuildList();
    });
    n->addWidget(m_back);
    auto* titles = new QVBoxLayout;
    titles->setSpacing(0);
    m_listTitle = new QLabel;
    m_listTitle->setObjectName("listTitle");
    m_listHint = new QLabel;
    m_listHint->setObjectName("muted");
    m_listHint->setStyleSheet("font-size:12px");
    titles->addWidget(m_listTitle);
    titles->addWidget(m_listHint);
    n->addLayout(titles, 1);
    m_backupNow = new QPushButton("Backup now");
    m_backupNow->setObjectName("backupNow");
    connect(m_backupNow, &QPushButton::clicked, this, &Window::backupNow);
    n->addWidget(m_backupNow);
    v->addWidget(nav);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* host = new QWidget;
    host->setObjectName("listHost");
    m_listLayout = new QVBoxLayout(host);
    m_listLayout->setContentsMargins(12, 8, 12, 16);
    m_listLayout->setSpacing(2);
    scroll->setWidget(host);
    v->addWidget(scroll, 1);
    return side;
}

// The tests of the current atem-sweep, in run order.
bool Window::loadPlan(QString* error) {
    QProcess p;
    p.setProcessEnvironment(childEnvironment());
    p.start(QCoreApplication::applicationDirPath() + "/atem-sweep.exe", { "--list", "--json" });
    if (!p.waitForFinished(20000)) {
        *error = p.errorString();
        return false;
    }
    for (const QByteArray& line : p.readAllStandardOutput().split('\n')) {
        if (!line.startsWith("@@")) continue;
        QJsonObject o = QJsonDocument::fromJson(line.mid(2)).object();
        if (!o.contains("plan")) continue;
        for (const QJsonValue& v : o["plan"].toArray()) {
            TestItem t;
            t.id = v["id"].toString();
            t.title = v["title"].toString();
            t.group = testGroup(t.id);
            ui::classify(t);
            m_index[t.id] = static_cast<int>(m_model.tests.size());
            m_model.tests << t;
        }
    }
    if (m_model.tests.isEmpty()) *error = "no tests in its output";
    return !m_model.tests.isEmpty();
}

// What the real switcher answered, per test: shown for tests not run yet and
// next to this run's values.
void Window::loadGolden() {
    for (TestItem& t : m_model.tests) {
        t.goldenObs = {};
        t.goldenSdk.clear();
    }
    QFile f(m_golden->currentData().toString() + "/results.json");
    if (!f.open(QIODevice::ReadOnly)) return;
    for (const QJsonValue& v : QJsonDocument::fromJson(f.readAll()).object()["tests"].toArray()) {
        auto it = m_index.find(v["id"].toString());
        if (it == m_index.end()) continue;
        TestItem& t = m_model.tests[*it];
        t.goldenObs = v["obs"].toObject();
        for (const QJsonValue& s : v["sdk"].toArray()) t.goldenSdk << s.toString();
    }
}

void Window::setTarget(bool emulator) {
    if (busy()) {
        (m_emulator ? m_emuBtn : m_atemBtn)->setChecked(true);
        return;
    }
    m_emulator = emulator;
    m_emuBtn->setChecked(emulator);
    m_atemBtn->setChecked(!emulator);
    m_golden->setVisible(emulator);
    m_address->setVisible(!emulator);
    m_model.gotLabel = emulator ? "Emulator" : "This run";
    m_subtitle->setText(emulator ? "Every square is one test: the emulator against the real ATEM’s answers."
                                 : "Every square is one test: recording the real ATEM; everything is put back.");
    if (emulator && m_side == Side::Backups) {
        m_side = Side::Tests;
        m_backupBtn->setChecked(false);
    }
    updateBackupButton();
    rebuildList();
    refresh();
}

// ── Views ────────────────────────────────────────────────────

int Window::level() const {
    if (m_iface.isEmpty()) return 0;
    QSet<QString> props;
    for (const TestItem& t : m_model.tests)
        if (t.iface == m_iface && !t.prop.isEmpty()) props.insert(t.prop);
    return props.size() > 1 && m_prop.isEmpty() ? 1 : 2;
}

QVector<int> Window::scope() const {
    QVector<int> out;
    for (int i = 0; i < m_model.tests.size(); ++i) {
        const TestItem& t = m_model.tests[i];
        if ((m_iface.isEmpty() || t.iface == m_iface) && (m_prop.isEmpty() || t.prop == m_prop)) out << i;
    }
    return out;
}

void Window::rebuildMap() {
    while (QLayoutItem* item = m_mapLayout->takeAt(0)) {
        if (item->widget()) {
            item->widget()->hide();
            item->widget()->deleteLater();
        }
        delete item;
    }
    m_sections.clear();
    m_grids.clear();
    m_zoomSummary = nullptr;
    m_model.iface = m_iface;

    if (level() == 0) {
        for (const QString& g : testGroups()) {
            QVector<int> cells;
            for (int i = 0; i < m_model.tests.size(); ++i)
                if (m_model.tests[i].group == g) cells << i;
            if (cells.isEmpty()) continue;
            auto* w = new QWidget;
            auto* v = new QVBoxLayout(w);
            v->setContentsMargins(0, 0, 0, 0);
            v->setSpacing(8);
            auto* row = new QHBoxLayout;
            auto* box = new QCheckBox(QString(ui::groupLabel(g)).replace("&", "&&"));
            box->setChecked(m_groupOn[g]);
            connect(box, &QCheckBox::toggled, this, [this, g](bool on) {
                m_groupOn[g] = on;
                for (TestItem& t : m_model.tests)
                    if (t.group == g && !t.ran) t.status = on ? St::Queued : St::Off;
                refresh();
            });
            auto* summary = new QLabel;
            summary->setObjectName("muted");
            summary->setStyleSheet("font-size:12px");
            row->addWidget(box);
            row->addWidget(summary);
            row->addStretch(1);
            v->addLayout(row);
            auto* grid = new CellGrid(&m_model);
            grid->setCells(cells, 9);
            grid->setContentsMargins(28, 0, 0, 0);
            connect(grid, &CellGrid::clicked, this, &Window::select);
            auto* indent = new QHBoxLayout;
            indent->setContentsMargins(28, 0, 0, 0);
            indent->addWidget(grid);
            v->addLayout(indent);
            m_mapLayout->addWidget(w);
            m_sections << Section{ g, box, summary, grid, w };
            m_grids << grid;
        }
        auto* line = new QFrame;
        line->setFrameShape(QFrame::HLine);
        line->setStyleSheet("color:#dcd8cf");
        m_mapLayout->addWidget(line);
        m_mapLayout->addWidget(new SdkBar(QString(SWEEP_COVERAGE_DIR) + "/api-categories.txt"));
    } else {
        auto* crumbs = new QHBoxLayout;
        crumbs->setSpacing(6);
        QStringList parts = { "All tests", m_iface };
        if (!m_prop.isEmpty()) parts << m_prop;
        for (int i = 0; i < parts.size(); ++i) {
            auto* b = new QPushButton(parts[i]);
            b->setObjectName(i == parts.size() - 1 ? "crumbLast" : "crumb");
            connect(b, &QPushButton::clicked, this, [this, i]() {
                if (i == 0) m_iface.clear();
                if (i <= 1) m_prop.clear();
                setHover({});
                rebuildMap();
                rebuildList();
            });
            crumbs->addWidget(b);
            if (i + 1 < parts.size()) crumbs->addWidget(new QLabel("/"));
        }
        m_zoomSummary = new QLabel;
        m_zoomSummary->setObjectName("muted");
        m_zoomSummary->setStyleSheet("font-size:12px");
        crumbs->addSpacing(8);
        crumbs->addWidget(m_zoomSummary);
        crumbs->addStretch(1);
        auto* cw = new QWidget;
        cw->setLayout(crumbs);
        m_mapLayout->addWidget(cw);
        QVector<int> cells = scope();
        int n = static_cast<int>(cells.size());
        auto* grid = new CellGrid(&m_model);
        grid->setCells(cells, n > 600 ? 9 : n > 150 ? 14 : n > 30 ? 26 : 56);
        connect(grid, &CellGrid::clicked, this, &Window::select);
        m_mapLayout->addWidget(grid);
        m_grids << grid;
    }
    m_mapLayout->addStretch(1);
    refresh();
}

void Window::rebuildList() {
    while (QLayoutItem* item = m_listLayout->takeAt(0)) {
        if (item->widget()) {
            item->widget()->hide();
            item->widget()->deleteLater();
        }
        delete item;
    }
    m_navRows.clear();
    m_testRows.clear();

    if (m_side == Side::Backups) {
        m_card->hide();
        m_back->show();
        m_backupNow->show();
        m_backupNow->setEnabled(!m_emulator && !busy());
        m_listTitle->setText("Backups of the real ATEM");
        m_listHint->setText("Grouped by sweep · click to open");
        rebuildBackups();
        return;
    }
    m_card->show();
    m_backupNow->hide();
    int lv = level();
    m_back->setVisible(lv > 0);
    if (lv == 0) {
        m_listTitle->setText("By interface");
        m_listHint->setText("Differences first · point to find, click to drill in");
        QMap<QString, QVector<int>> by;
        for (int i = 0; i < m_model.tests.size(); ++i) by[m_model.tests[i].iface] << i;
        QList<QString> keys = by.keys();
        auto fails = [&](const QString& k) {
            int n = 0;
            for (int i : by[k]) n += m_model.tests[i].status == St::Fail;
            return n;
        };
        std::sort(keys.begin(), keys.end(), [&](const QString& a, const QString& b) {
            int fa = fails(a), fb = fails(b);
            return fa != fb ? fa > fb : by[a].size() > by[b].size();
        });
        for (const QString& k : keys) {
            auto* row = new NavRow(&m_model, k, by[k]);
            connect(row, &NavRow::hovered, this, [this, k](bool on) { setHover(on ? Hover{ Hover::Iface, k, -1 } : Hover{}); });
            connect(row, &NavRow::clicked, this, [this, k]() {
                m_iface = k;
                m_prop.clear();
                setHover({});
                rebuildMap();
                rebuildList();
            });
            m_listLayout->addWidget(row);
            m_navRows << row;
        }
    } else if (lv == 1) {
        m_listTitle->setText(m_iface);
        m_listHint->setText("Properties · point to find, click to drill in");
        QMap<QString, QVector<int>> by;
        for (int i : scope())
            if (!m_model.tests[i].prop.isEmpty()) by[m_model.tests[i].prop] << i;
        for (auto it = by.begin(); it != by.end(); ++it) {
            QString k = it.key();
            auto* row = new NavRow(&m_model, k, it.value());
            connect(row, &NavRow::hovered, this, [this, k](bool on) { setHover(on ? Hover{ Hover::Prop, k, -1 } : Hover{}); });
            connect(row, &NavRow::clicked, this, [this, k]() {
                m_prop = k;
                setHover({});
                rebuildMap();
                rebuildList();
            });
            m_listLayout->addWidget(row);
            m_navRows << row;
        }
    } else {
        m_listTitle->setText(m_prop.isEmpty() ? m_iface : m_iface + " · " + m_prop);
        m_listHint->setText("Each test · both values where they differ");
        for (int i : scope()) {
            auto* row = new TestRow(&m_model, i, !m_prop.isEmpty());
            connect(row, &TestRow::hovered, this, [this, i](bool on) { setHover(on ? Hover{ Hover::Test, {}, i } : Hover{}); });
            connect(row, &TestRow::clicked, this, [this, i]() { select(i); });
            m_listLayout->addWidget(row);
            m_testRows << row;
        }
    }
    m_listLayout->addStretch(1);
}

void Window::rebuildBackups() {
    QList<BackupGroup> groups = ui::backupGroups(SWEEP_BACKUP_DIR);
    if (groups.isEmpty()) {
        auto* none = new QLabel("No backups yet.");
        none->setObjectName("muted");
        none->setContentsMargins(12, 24, 12, 0);
        m_listLayout->addWidget(none);
    }
    for (const BackupGroup& g : groups) {
        bool open = m_openGroups.contains(g.key);
        auto* header = new BackupGroupRow(g, open);
        connect(header, &BackupGroupRow::toggled, this, [this, key = g.key]() {
            if (m_openGroups.contains(key)) m_openGroups.remove(key);
            else m_openGroups.insert(key);
            rebuildList();
        });
        m_listLayout->addWidget(header);
        if (!open) continue;
        for (const BackupInfo& b : g.members) {
            auto* row = new BackupRow(b, !m_emulator && !busy());
            connect(row, &BackupRow::restore, this, &Window::restoreFrom);
            m_listLayout->addWidget(row);
        }
    }
    m_listLayout->addStretch(1);
}

void Window::refresh() {
    int pass = 0, fail = 0, skip = 0, total = 0, done = 0;
    for (const TestItem& t : m_model.tests) {
        if (t.status == St::Off) continue;
        ++total;
        if (t.status == St::Pass) ++pass, ++done;
        else if (t.status == St::Fail) ++fail, ++done;
        else if (t.status == St::Skip) ++skip, ++done;
    }
    m_done = done;
    m_total = total;
    int pct = total ? done * 100 / total : 0;
    m_pct->setText(QString("%1%").arg(pct));
    m_pct1->setText(QString("%1 of %2 tests").arg(done).arg(total));
    m_pct2->setText(QString("%1 differ · %2 skipped").arg(fail).arg(skip));
    m_strip->setCounts(pass, fail, skip, total);
    m_phases->setSweepPercent(pct);
    m_showPass->setText(QString("Same as the real ATEM  %1").arg(pass));
    m_showFail->setText(QString("Differs  %1").arg(fail));
    m_showSkip->setText(QString("Skipped  %1").arg(skip));
    m_notRun->setText(QString("Not run yet  %1").arg(total - done));

    for (const Section& s : m_sections) {
        int n = 0, d = 0, f = 0;
        for (const TestItem& t : m_model.tests) {
            if (t.group != s.group) continue;
            ++n;
            if (t.status == St::Pass || t.status == St::Fail || t.status == St::Skip) ++d;
            f += t.status == St::Fail;
        }
        bool on = m_groupOn[s.group];
        s.box->setEnabled(!busy());
        s.summary->setText(!on ? QString("%1 tests · not in this run").arg(n)
                               : d ? QString("%1 tests · %2% · %3 differ").arg(n).arg(d * 100 / n).arg(f)
                                   : QString("%1 tests").arg(n));
        auto* fx = s.widget->graphicsEffect();
        Q_UNUSED(fx);
        s.widget->setStyleSheet(on ? "" : "QLabel, QCheckBox { color: #a6a298; }");
    }
    if (m_zoomSummary) {
        int n = 0, p = 0, f = 0, k = 0;
        for (int i : scope()) {
            ++n;
            St s = m_model.tests[i].status;
            p += s == St::Pass;
            f += s == St::Fail;
            k += s == St::Skip;
        }
        m_zoomSummary->setText(QString("%1 tests · %2 same · %3 differ · %4 skipped").arg(n).arg(p).arg(f).arg(k));
    }
    for (CellGrid* g : m_grids) g->update();
    for (NavRow* r : m_navRows) r->update();
    for (TestRow* r : m_testRows) r->refresh();
    updateCard();
}

void Window::updateCard() {
    if (m_model.selected < 0) {
        m_badge->setText("");
        m_badge->setStyleSheet("");
        m_testNo->setText("Click a square or a test to see it here.");
        m_testId->clear();
        m_testTitle->clear();
        m_values->clear();
        return;
    }
    const TestItem& t = m_model.tests[m_model.selected];
    static const QMap<St, QString> badge = { { St::Pass, "Same as the real ATEM" }, { St::Fail, "Differs" }, { St::Skip, "Skipped" },
                                             { St::Running, "Running" }, { St::Queued, "Not run yet" }, { St::Off, "Not in this run" } };
    QColor c = t.status == St::Queued ? QColor("#8a8f94") : t.status == St::Off ? QColor("#a6a298")
             : t.status == St::Skip ? QColor("#7d786d") : ui::color(t.status);
    m_badge->setText(badge.value(t.status));
    m_badge->setStyleSheet(QString("background:%1; color:white; border-radius:10px; padding:2px 10px; font-size:12px; font-weight:600").arg(c.name()));
    m_testNo->setText(QString("test %1 of %2").arg(m_model.selected + 1).arg(m_model.tests.size()));
    m_testId->setText(t.id);
    m_testTitle->setText(t.title);

    QString rows;
    auto row = [&](const QString& k, const QString& g, const QString& got, bool differs) {
        rows += QString("<tr><td style='padding:4px 8px; font-family:Cascadia Mono,Consolas'>%1</td>"
                        "<td style='padding:4px 8px; font-family:Cascadia Mono,Consolas'>%2</td>"
                        "<td style='padding:4px 8px; font-family:Cascadia Mono,Consolas; %4'>%3</td></tr>")
                    .arg(html(k), html(g), html(got), differs ? "background:#fde7df; color:#7a2710" : "");
    };
    if (t.status == St::Fail) {
        for (const Diff& d : ui::differences(t)) row(d.key, d.golden, d.got, true);
    } else {
        QJsonObject shown = t.ran ? t.obs : t.goldenObs;
        int n = 0;
        for (auto it = shown.begin(); it != shown.end() && n < 10; ++it, ++n) {
            QString g = ui::brief(t.goldenObs.value(it.key()));
            row(it.key(), t.goldenObs.contains(it.key()) ? g : "—", t.ran ? ui::brief(it.value()) : "…", false);
        }
    }
    QString notes = t.notes.isEmpty() ? QString() : "<p style='color:#5f6368'>" + html(t.notes.join("; ")) + "</p>";
    QStringList sdk = t.sdk.isEmpty() ? t.goldenSdk : t.sdk;
    QString calls = sdk.isEmpty() ? QString()
                                  : "<p style='color:#5f6368; font-size:11px'>SDK: " + html(sdk.join(", ")) + "</p>";
    m_values->setText(rows.isEmpty() ? notes + calls
                                     : QString("<table style='border-collapse:collapse; font-size:12px' cellspacing='0'>"
                                               "<tr style='color:#5f6368'><td style='padding:4px 8px'>Value</td>"
                                               "<td style='padding:4px 8px'>Real ATEM</td><td style='padding:4px 8px'>%1</td></tr>%2</table>%3%4")
                                           .arg(m_model.gotLabel, rows, notes, calls));
}

void Window::updateBackupButton() {
    QList<BackupInfo> all = ui::backups(SWEEP_BACKUP_DIR);
    QString last = all.isEmpty() ? "no backup yet" : "last backup " + ui::ago(all.first().created);
    m_backupBtn->setText("Backup ATEM\n" + last);
    m_backupBtn->setEnabled(!m_emulator);
    m_backupBtn->setToolTip(m_emulator ? "Backups are of the real ATEM: select Real ATEM"
                                       : "Show the backups, take one, or restore one");
}

void Window::select(int test) {
    m_model.selected = test;
    refresh();
}

void Window::setHover(Hover h) {
    m_model.hover = h;
    for (CellGrid* g : m_grids) g->update();
}

// ── Running ──────────────────────────────────────────────────

bool Window::busy() const { return m_job != Job::None; }

void Window::setStatus(const QString& text) { m_subtitle->setText(text); }

void Window::run() {
    QStringList groups;
    for (const QString& g : testGroups())
        if (m_groupOn[g]) groups << g;
    if (groups.isEmpty()) {
        setStatus("Select at least one group of tests.");
        return;
    }
    QString address = m_address->currentText().trimmed();
    if (!m_emulator) {
        auto answer = QMessageBox::warning(this, "Run the sweep on the real ATEM",
            QString("The sweep switches inputs, moves the PiP, runs the stored macros and fades to black on the "
                    "live output for about 30 minutes, then puts every setting back.\n\n"
                    "%1\n\nIs nothing live right now?")
                .arg(address.compare("usb", Qt::CaseInsensitive) == 0
                         ? "Over USB there is no verified backup: the storage tests are skipped."
                         : "Stored macros and stills are backed up first, restored at the end and checked."),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) return;
    }

    for (TestItem& t : m_model.tests) {
        t.status = m_groupOn[t.group] ? St::Queued : St::Off;
        t.obs = {};
        t.diffs.clear();
        t.notes.clear();
        t.problems.clear();
        t.sdk.clear();
        t.ran = false;
    }
    m_log.clear();
    m_summary = {};
    m_stopping = false;
    m_phases->reset(m_emulator ? QStringList{ "emulator", "tests", "settings" }
                               : QStringList{ "backup", "tests", "settings", "swept", "restore", "compare" });
    m_phases->show();
    refresh();

    QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    QString outDir = QCoreApplication::applicationDirPath() + "/runs/" + stamp + (m_emulator ? "-gui-verify" : "-gui-record");
    QDir().mkpath(outDir);
    m_stopFile = outDir + "/stop";
    QFile::remove(m_stopFile);

    QStringList args;
    if (m_emulator) {
        m_phases->setState("emulator", "start");
        QString error;
        if (!ensureEmulator(&error)) {
            m_phases->setState("emulator", "fail");
            setStatus(error);
            QMessageBox::warning(this, "atem-sweep", error);
            return;
        }
        m_phases->setState("emulator", "ok");
        m_runGolden = m_golden->currentText();
        args << kEmulatorAddress << "--verify" << m_golden->currentData().toString() + "/results.json";
        m_runMode = "verify";
        m_runTarget = QString("emulator (%1)").arg(kEmulatorAddress);
    } else {
        args << address << "--protect";
        m_runMode = "record";
        m_runGolden.clear();
        m_runTarget = "real ATEM (" + address + ")";
    }
    args << "--yes" << "--json" << "--stop-file" << m_stopFile << "--out" << outDir;
    if (groups.size() < testGroups().size()) args << "--groups" << groups.join(',');
    m_runStarted = QDateTime::currentDateTime();
    m_job = Job::Sweep;
    startSweep(args, outDir);
}

void Window::stop() {
    if (m_job != Job::Sweep || m_stopping) return;
    QFile f(m_stopFile);
    if (f.open(QIODevice::WriteOnly)) f.write("stop\n");
    m_stopping = true;
    m_runBtn->setText("Stopping…");
    m_runBtn->setEnabled(false);
    setStatus("Stopping after the current test; everything is put back first.");
}

// The emulator must own 127.0.0.2:9910 before the sweep starts: if nothing
// answers there the SDK falls back to an ATEM on USB (the real one).
bool Window::ensureEmulator(QString* error) {
    DWORD owner = udpOwner(kEmulatorAddress, 9910);
    if (!owner) owner = udpOwner("0.0.0.0", 9910);
    if (owner) {
        QString name = processName(owner);
        if (name.startsWith("atem-emu", Qt::CaseInsensitive)) {
            setStatus("Using the emulator that is already running (" + name + ").");
            return true;
        }
        *error = QString("UDP 9910 is in use by %1 (pid %2), not by the emulator: not starting.").arg(name).arg(owner);
        return false;
    }
    QString exe = EMU_EXE;
    if (!QFile::exists(exe)) {
        *error = "The emulator is not built: " + QDir::toNativeSeparators(exe);
        return false;
    }
    QStringList args = { "--listen", kEmulatorAddress };
    QString profile = m_golden->currentText();
    profile.remove(QRegularExpression("_sdk[0-9.]+"));
    if (QFileInfo::exists(QString(EMU_PROFILES) + "/" + profile + "/dump.txt"))
        args << "--profile" << QString(EMU_PROFILES) + "/" + profile;
    if (!m_emu) m_emu = new QProcess(this);
    m_emu->setProcessEnvironment(childEnvironment());
    m_emu->setStandardOutputFile(QProcess::nullDevice());
    m_emu->setStandardErrorFile(QProcess::nullDevice());
    m_emu->start(exe, args);
    if (!m_emu->waitForStarted(5000)) {
        *error = "Cannot start the emulator: " + m_emu->errorString();
        return false;
    }
    setStatus("Starting the emulator on " + QString(kEmulatorAddress) + "…");
    for (int i = 0; i < 50; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(200);
        if (udpOwner(kEmulatorAddress, 9910) == static_cast<DWORD>(m_emu->processId())) return true;
        if (m_emu->state() == QProcess::NotRunning) break;
    }
    m_emu->kill();
    *error = "The emulator did not take 127.0.0.2:9910: the sweep was not started.";
    return false;
}

void Window::startSweep(const QStringList& args, const QString& outDir) {
    m_outDir = outDir;
    m_buffer.clear();
    if (!m_sweep) {
        m_sweep = new QProcess(this);
        m_sweep->setProcessChannelMode(QProcess::MergedChannels);
        m_sweep->setProcessEnvironment(childEnvironment());
        connect(m_sweep, &QProcess::readyReadStandardOutput, this, &Window::onOutput);
        connect(m_sweep, &QProcess::finished, this, [this](int code) { onFinished(code); });
    }
    m_sweep->start(QCoreApplication::applicationDirPath() + "/atem-sweep.exe", args);
    m_runBtn->setText(m_job == Job::Sweep ? "Stop" : "Run sweep");
    m_runBtn->setEnabled(m_job == Job::Sweep);
    m_emuBtn->setEnabled(false);
    m_atemBtn->setEnabled(false);
    m_backupNow->setEnabled(false);
    refresh();
    if (m_side == Side::Backups) rebuildList();
}

void Window::onOutput() {
    m_buffer += m_sweep->readAllStandardOutput();
    int nl;
    while ((nl = m_buffer.indexOf('\n')) >= 0) {
        QString line = QString::fromUtf8(m_buffer.left(nl)).trimmed();
        m_buffer.remove(0, nl + 1);
        onLine(line);
    }
}

void Window::onLine(const QString& raw) {
    static const QRegularExpression ansi("\x1b\\[[0-9;]*m");
    // A colour reset can end up at the start of the next line.
    const QString line = QString(raw).remove(ansi).trimmed();
    if (!line.startsWith("@@")) {
        const QString& text = line;
        m_log << text;
        if (!text.isEmpty() && !text.startsWith('[') && !text.startsWith("diff:") && !text.startsWith("note:")
            && !text.startsWith("problem:"))
            setStatus(text);
        return;
    }
    QJsonObject o = QJsonDocument::fromJson(line.mid(2).toUtf8()).object();
    if (o.contains("start")) {
        auto it = m_index.find(o["start"].toString());
        if (it != m_index.end()) {
            m_model.tests[*it].status = St::Running;
            for (CellGrid* g : m_grids) g->update();
        }
    } else if (o.contains("result")) {
        QJsonObject r = o["result"].toObject();
        auto it = m_index.find(r["id"].toString());
        if (it == m_index.end()) return;
        TestItem& t = m_model.tests[*it];
        QString s = r["status"].toString();
        t.status = s == "pass" ? St::Pass : s == "skip" ? St::Skip : St::Fail;
        t.obs = r["obs"].toObject();
        if (r.contains("goldenObs")) t.goldenObs = r["goldenObs"].toObject();
        auto list = [](const QJsonValue& v) {
            QStringList out;
            for (const QJsonValue& x : v.toArray()) out << x.toString();
            return out;
        };
        t.diffs = list(r["diffs"]);
        t.notes = list(r["notes"]);
        t.problems = list(r["problems"]);
        t.sdk = list(r["sdk"]);
        if (t.diffs.isEmpty() && !t.problems.isEmpty() && t.status == St::Fail) t.diffs = t.problems;
        t.ms = r["ms"].toInt();
        t.ran = true;
        refresh();
    } else if (o.contains("phase")) {
        QString ph = o["phase"].toString(), st = o["state"].toString();
        m_phases->setState(ph, st);
    } else if (o.contains("summary")) {
        m_summary = o["summary"].toObject();
    }
}

void Window::onFinished(int code) {
    onOutput();
    Job job = m_job;
    m_job = Job::None;
    m_stopping = false;
    if (m_emu && m_emu->state() != QProcess::NotRunning) {
        m_emu->kill();
        m_emu->waitForFinished(2000);
    }
    m_runBtn->setText("Run sweep");
    m_runBtn->setEnabled(true);
    m_emuBtn->setEnabled(true);
    m_atemBtn->setEnabled(true);
    QString text;
    if (job == Job::Sweep) {
        for (TestItem& t : m_model.tests)
            if (t.status == St::Running) t.status = St::Queued;
        switch (code) {
        case 0: text = "Finished: every test answered like the real ATEM."; break;
        case 1: text = QString("Finished: %1 tests differ.").arg(m_summary["fail"].toInt()); break;
        case 2: text = "atem-sweep could not start: " + m_log.value(m_log.size() - 2); break;
        case 3: text = "Finished, but the results could not be written."; break;
        case 4: text = "Not started: the switcher is running or recording a macro."; break;
        case 5:
            text = "Finished, but the switcher is NOT back as it was: see the backups.";
            QMessageBox::critical(this, "atem-sweep", "The switcher is not back exactly as it was.\n\n"
                                                      "Open the backups (Backup ATEM) and restore the sweep's \"Before\" backup.");
            break;
        default: text = QString("atem-sweep ended with code %1.").arg(code);
        }
        if (m_summary["stopped"].toBool()) text = "Stopped. " + text;
    } else if (job == Job::Backup) {
        text = code == 0 ? "Backup complete." : QString("The backup is not complete (code %1).").arg(code);
    } else if (job == Job::Restore) {
        text = code == 0 ? "Restored and checked: the switcher holds exactly the backup's content."
             : code == 5 ? "Restored, but the check found differences: see the log."
                         : QString("The restore did not finish (code %1).").arg(code);
        if (code != 0) QMessageBox::warning(this, "atem-sweep", text);
    }
    setStatus(text);
    updateBackupButton();
    refresh();
    rebuildList();
    if (m_whenDone) std::exchange(m_whenDone, {})();
}

void Window::backupNow() {
    if (busy() || m_emulator) return;
    m_phases->hide();
    m_job = Job::Backup;
    setStatus("Backing up the ATEM (read-only)…");
    startSweep({ m_address->currentText().trimmed(), "--backup" }, {});
}

void Window::restoreFrom(const QString& dir) {
    if (busy() || m_emulator) return;
    auto answer = QMessageBox::warning(this, "Restore the ATEM",
        QString("Put the switcher's stored macros and stills back to\n%1?\n\n"
                "What the switcher holds now is overwritten (slots empty in the backup are cleared). "
                "A new backup is taken afterwards and compared with this one.")
            .arg(QDir::toNativeSeparators(dir)),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;
    m_phases->hide();
    m_job = Job::Restore;
    setStatus("Restoring from " + QFileInfo(dir).fileName() + "…");
    QString after = QString(SWEEP_BACKUP_DIR) + "/" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + "-restored";
    startSweep({ m_address->currentText().trimmed(), "--restore", dir, "--yes", "--out", after }, {});
}

// ── Report ───────────────────────────────────────────────────

void Window::saveReport() {
    QString suggested = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/atem-sweep-report-" +
                        QDateTime::currentDateTime().toString("yyyyMMdd-HHmm") + ".md";
    QString path = QFileDialog::getSaveFileName(this, "Download report", suggested, "Markdown (*.md)");
    if (path.isEmpty()) return;

    int pass = 0, fail = 0, skip = 0, notRun = 0;
    for (const TestItem& t : m_model.tests) {
        if (t.status == St::Pass) ++pass;
        else if (t.status == St::Fail) ++fail;
        else if (t.status == St::Skip) ++skip;
        else if (t.status != St::Off) ++notRun;
    }
    QString md;
    QTextStream out(&md);
    out << "# atem-sweep report\n\n";
    out << "| | |\n|---|---|\n";
    out << "| Date | " << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm") << " |\n";
    out << "| Switcher | " << (m_runTarget.isEmpty() ? (m_emulator ? "emulator" : "real ATEM") : m_runTarget) << " |\n";
    out << "| Mode | " << (m_runMode.isEmpty() ? "(not run yet)" : m_runMode == "verify" ? "verify against the golden record" : "record") << " |\n";
    if (m_emulator || !m_runGolden.isEmpty())
        out << "| Golden record | " << (m_runGolden.isEmpty() ? m_golden->currentText() : m_runGolden) << " |\n";
    QStringList groups;
    for (const QString& g : testGroups())
        if (m_groupOn[g]) groups << ui::groupLabel(g);
    out << "| Groups | " << mdCell(groups.join(", ")) << " |\n";
    out << "| Result | " << pass << " same, " << fail << " differ, " << skip << " skipped, " << notRun << " not run |\n";
    if (!m_outDir.isEmpty()) out << "| Run folder | `" << QDir::toNativeSeparators(m_outDir) << "` |\n";
    out << "\n";

    if (fail) {
        out << "## Differences from the real ATEM\n\n";
        for (const TestItem& t : m_model.tests) {
            if (t.status != St::Fail) continue;
            out << "- `" << t.id << "` — " << t.title << "\n";
            for (const Diff& d : ui::differences(t))
                out << "  - " << d.key << ": real ATEM `" << d.golden << "`, " << m_model.gotLabel.toLower() << " `" << d.got << "`\n";
        }
        out << "\n";
    }
    if (skip) {
        out << "## Skipped\n\n";
        for (const TestItem& t : m_model.tests)
            if (t.status == St::Skip) out << "- `" << t.id << "` — " << (t.notes.isEmpty() ? t.title : t.notes.join("; ")) << "\n";
        out << "\n";
    }

    out << "## All tests\n\n";
    out << "| # | Test | Source | Result | Values | SDK functions |\n";
    out << "|---:|---|---|---|---|---|\n";
    for (int i = 0; i < m_model.tests.size(); ++i) {
        const TestItem& t = m_model.tests[i];
        QString values;
        if (t.status == St::Fail) {
            QStringList parts;
            for (const Diff& d : ui::differences(t))
                parts << QString("**%1**: real `%2` → %3 `%4`").arg(d.key, d.golden, m_model.gotLabel.toLower(), d.got);
            values = parts.join("; ");
        } else {
            QJsonObject shown = t.ran ? t.obs : t.goldenObs;
            QStringList parts;
            for (auto it = shown.begin(); it != shown.end(); ++it) parts << it.key() + " = `" + ui::brief(it.value()) + "`";
            values = parts.join("; ");
            if (!t.ran && !values.isEmpty()) values = "(real ATEM) " + values;
        }
        QStringList sdk = t.sdk.isEmpty() ? t.goldenSdk : t.sdk;
        out << "| " << i + 1 << " | `" << t.id << "` " << mdCell(t.title) << " | " << ui::groupLabel(t.group) << " | "
            << ui::statusWord(t.status) << " | " << mdCell(values) << " | " << mdCell(sdk.join(", ")) << " |\n";
    }
    if (!m_log.isEmpty()) {
        out << "\n## Log of the last run\n\n```text\n";
        for (const QString& l : m_log)
            if (!l.startsWith('[')) out << l << "\n";
        out << "```\n";
    }
    out.flush();

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(md.toUtf8()) < 0 || !f.commit()) {
        QMessageBox::warning(this, "Download report", "Could not write " + QDir::toNativeSeparators(path));
        return;
    }
    setStatus("Report saved: " + QDir::toNativeSeparators(path));
}
