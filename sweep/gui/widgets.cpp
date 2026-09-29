#include "widgets.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QTextStream>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QFont uiFont(int px, int weight = QFont::Normal) {
    QFont f("Segoe UI");
    f.setPixelSize(px);
    f.setWeight(static_cast<QFont::Weight>(weight));
    return f;
}

QFont monoFont(int px) {
    QFont f("Cascadia Mono");
    f.setStyleHint(QFont::Monospace);
    f.setPixelSize(px);
    return f;
}

const QColor kInk("#1d1f22"), kMuted("#5f6368"), kTrack("#e8e5dd"), kHot("#ece8df");

// The status mark used in lists: a tick, a cross, a ring or a dot.
void drawMark(QPainter& p, const QRectF& r, St s) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QPointF c = r.center();
    if (s == St::Pass) {
        p.setPen(QPen(ui::color(St::Pass), 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath path;
        path.moveTo(c + QPointF(-5, 0.5));
        path.lineTo(c + QPointF(-1.5, 4));
        path.lineTo(c + QPointF(5.5, -4));
        p.drawPath(path);
    } else if (s == St::Fail) {
        p.setPen(QPen(QColor("#c9431f"), 2.4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c + QPointF(-4.5, -4.5), c + QPointF(4.5, 4.5));
        p.drawLine(c + QPointF(4.5, -4.5), c + QPointF(-4.5, 4.5));
    } else if (s == St::Skip) {
        p.setPen(QPen(QColor("#8a8578"), 2));
        p.drawEllipse(c, 5, 5);
    } else {
        p.setPen(Qt::NoPen);
        p.setBrush(s == St::Running ? ui::color(St::Running) : QColor("#cfcac0"));
        p.drawEllipse(c, 4, 4);
    }
    p.restore();
}

} // namespace

// ── CellGrid ─────────────────────────────────────────────────

CellGrid::CellGrid(Model* model, QWidget* parent) : QWidget(parent), m_model(model) {
    QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
    setMouseTracking(true);
}

void CellGrid::setCells(const QVector<int>& tests, int size) {
    m_tests = tests;
    m_size = size;
    updateGeometry();
    update();
}

int CellGrid::columns(int width) const { return std::max(1, (width + gap()) / (m_size + gap())); }

int CellGrid::heightForWidth(int width) const {
    int rows = (static_cast<int>(m_tests.size()) + columns(width) - 1) / columns(width);
    return std::max(0, rows * (m_size + gap()) - gap()) + 4;
}

QSize CellGrid::sizeHint() const { return { 800, heightForWidth(800) }; }

int CellGrid::testAt(const QPoint& p) const {
    int step = m_size + gap(), cols = columns(width());
    int c = (p.x() - 2) / step, r = (p.y() - 2) / step;
    if (c < 0 || c >= cols || r < 0) return -1;
    int i = r * cols + c;
    return i < m_tests.size() ? m_tests[i] : -1;
}

void CellGrid::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    int step = m_size + gap(), cols = columns(width());
    qreal radius = m_size > 20 ? 6 : 2;
    p.setFont(monoFont(10));
    for (int i = 0; i < m_tests.size(); ++i) {
        const TestItem& t = m_model->tests[m_tests[i]];
        QRectF r(2 + (i % cols) * step, 2 + (i / cols) * step, m_size, m_size);
        QColor c = m_model->shown(t.status) ? ui::color(t.status) : ui::color(St::Queued);
        bool lit = m_model->lit(t);
        if (!lit) c.setAlphaF(0.16);
        if (t.status == St::Off) c.setAlphaF(lit ? 0.5 : 0.1);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(r, radius, radius);
        if (m_model->hover.kind == Hover::Test && lit) {
            p.setPen(QPen(ui::color(St::Running), 3));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r.adjusted(-2, -2, 2, 2), radius + 2, radius + 2);
        }
        if (m_model->selected == m_tests[i]) {
            p.setPen(QPen(kInk, 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r.adjusted(-2.5, -2.5, 2.5, 2.5), radius + 2, radius + 2);
        }
        if (m_size >= 48) {
            p.setPen(t.status == St::Pass || t.status == St::Fail ? Qt::white : QColor("#45484c"));
            QString text = t.value.isEmpty() ? t.id.section('.', -1) : t.value;
            p.drawText(r.adjusted(3, 3, -3, -3), Qt::AlignCenter | Qt::TextWrapAnywhere,
                       p.fontMetrics().elidedText(text, Qt::ElideRight, static_cast<int>(r.width() * 2 - 12)));
        }
    }
}

void CellGrid::mousePressEvent(QMouseEvent* e) {
    int t = testAt(e->pos());
    if (t >= 0) emit clicked(t);
}

bool CellGrid::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip) {
        auto* h = static_cast<QHelpEvent*>(e);
        int t = testAt(h->pos());
        if (t >= 0) {
            const TestItem& item = m_model->tests[t];
            QToolTip::showText(h->globalPos(), item.id + "\n" + item.title + "\n" + ui::statusWord(item.status), this);
        } else {
            QToolTip::hideText();
        }
        return true;
    }
    return QWidget::event(e);
}

// ── NavRow ───────────────────────────────────────────────────

NavRow::NavRow(Model* model, const QString& label, const QVector<int>& tests, QWidget* parent)
    : QWidget(parent), m_model(model), m_label(label), m_tests(tests) {
    setCursor(Qt::PointingHandCursor);
    setFixedHeight(40);
    setToolTip(label);
}

void NavRow::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    int n = 0, pass = 0, fail = 0, skip = 0, off = 0;
    for (int i : m_tests) {
        ++n;
        switch (m_model->tests[i].status) {
        case St::Pass: ++pass; break;
        case St::Fail: ++fail; break;
        case St::Skip: ++skip; break;
        case St::Off: ++off; break;
        default: break;
        }
    }
    if (off == n) p.setOpacity(0.45);
    if (m_hot) {
        p.setPen(Qt::NoPen);
        p.setBrush(kHot);
        p.drawRoundedRect(rect(), 6, 6);
    }
    p.setPen(kInk);
    p.setFont(monoFont(12));
    p.drawText(QRect(8, 0, 160, height()), Qt::AlignVCenter, p.fontMetrics().elidedText(m_label, Qt::ElideRight, 160));
    QRectF bar(176, height() / 2.0 - 4, width() - 176 - 110, 8);
    p.setPen(Qt::NoPen);
    p.setBrush(kTrack);
    p.drawRoundedRect(bar, 4, 4);
    qreal x = bar.left();
    for (auto [count, st] : { std::pair{ pass, St::Pass }, std::pair{ fail, St::Fail }, std::pair{ skip, St::Skip } }) {
        qreal w = bar.width() * count / std::max(1, n);
        p.setBrush(ui::color(st));
        p.drawRect(QRectF(x, bar.top(), w, bar.height()));
        x += w;
    }
    p.setPen(kMuted);
    p.setFont(monoFont(12));
    QString count = QString::number(n) + (fail ? QString::fromUtf8(" · %1 ✕").arg(fail) : QString());
    p.drawText(QRect(width() - 102, 0, 76, height()), Qt::AlignVCenter | Qt::AlignRight, count);
    p.setPen(QPen(QColor("#9a9ea3"), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    int cx = width() - 14, cy = height() / 2;
    p.drawLine(cx - 3, cy - 5, cx + 2, cy);
    p.drawLine(cx + 2, cy, cx - 3, cy + 5);
}

// ── TestRow ──────────────────────────────────────────────────

TestRow::TestRow(Model* model, int test, bool showValue, QWidget* parent)
    : QWidget(parent), m_model(model), m_test(test), m_showValue(showValue) {
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize TestRow::sizeHint() const {
    const TestItem& t = m_model->tests[m_test];
    int n = t.status == St::Fail ? std::min(3, static_cast<int>(ui::differences(t).size())) : 0;
    return { 360, 36 + n * 46 + (n ? 6 : 0) };
}

void TestRow::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const TestItem& t = m_model->tests[m_test];
    if (m_model->selected == m_test || m_hot) {
        p.setPen(Qt::NoPen);
        p.setBrush(m_model->selected == m_test ? kHot : QColor("#f3f0e9"));
        p.drawRoundedRect(rect(), 6, 6);
    }
    drawMark(p, QRectF(8, 10, 16, 16), t.status);
    p.setPen(kInk);
    p.setFont(monoFont(13));
    QString label = m_showValue && !t.value.isEmpty() ? "= " + t.value : t.id;
    p.setFont(uiFont(12));
    QString state = ui::statusWord(t.status);
    int stateW = p.fontMetrics().horizontalAdvance(state) + 8;
    p.setPen(kMuted);
    p.drawText(QRect(width() - stateW - 8, 0, stateW, 36), Qt::AlignVCenter | Qt::AlignRight, state);
    p.setFont(monoFont(13));
    p.setPen(kInk);
    p.drawText(QRect(32, 0, width() - 48 - stateW, 36), Qt::AlignVCenter,
               p.fontMetrics().elidedText(label, Qt::ElideRight, width() - 48 - stateW));
    if (t.status != St::Fail) return;
    QList<Diff> diffs = ui::differences(t);
    int y = 36;
    int w = (width() - 32 - 8 - 6) / 2;
    for (int i = 0; i < std::min(3, static_cast<int>(diffs.size())); ++i, y += 46) {
        const Diff& d = diffs[i];
        QRect left(32, y, w, 40), right(32 + w + 6, y, w, 40);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#eef5f3"));
        p.drawRoundedRect(left, 6, 6);
        p.setBrush(QColor("#fde7df"));
        p.drawRoundedRect(right, 6, 6);
        p.setFont(uiFont(11));
        p.setPen(QColor("#3f6f64"));
        p.drawText(left.adjusted(8, 3, -6, -20), Qt::AlignLeft | Qt::AlignVCenter,
                   p.fontMetrics().elidedText("Real ATEM · " + d.key, Qt::ElideRight, left.width() - 14));
        p.setPen(QColor("#8a3217"));
        p.drawText(right.adjusted(8, 3, -6, -20), Qt::AlignLeft | Qt::AlignVCenter, m_model->gotLabel);
        p.setFont(monoFont(12));
        p.setPen(kInk);
        p.drawText(left.adjusted(8, 19, -6, -3), Qt::AlignLeft | Qt::AlignVCenter,
                   p.fontMetrics().elidedText(d.golden, Qt::ElideRight, left.width() - 14));
        p.setPen(QColor("#7a2710"));
        p.drawText(right.adjusted(8, 19, -6, -3), Qt::AlignLeft | Qt::AlignVCenter,
                   p.fontMetrics().elidedText(d.got, Qt::ElideRight, right.width() - 14));
    }
}

// ── Backups ──────────────────────────────────────────────────

BackupGroupRow::BackupGroupRow(const BackupGroup& group, bool open, QWidget* parent)
    : QWidget(parent), m_group(group), m_open(open) {
    setCursor(Qt::PointingHandCursor);
    setFixedHeight(56);
}

void BackupGroupRow::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (m_hot || m_open) {
        p.setPen(Qt::NoPen);
        p.setBrush(m_open ? kHot : QColor("#f3f0e9"));
        p.drawRoundedRect(rect(), 8, 8);
    }
    // chevron
    p.setPen(QPen(QColor("#5f6368"), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    int cx = 16, cy = height() / 2;
    if (m_open) {
        p.drawLine(cx - 5, cy - 2, cx, cy + 3);
        p.drawLine(cx, cy + 3, cx + 5, cy - 2);
    } else {
        p.drawLine(cx - 2, cy - 5, cx + 3, cy);
        p.drawLine(cx + 3, cy, cx - 2, cy + 5);
    }
    p.setPen(kInk);
    p.setFont(uiFont(14, QFont::DemiBold));
    p.drawText(QRect(34, 8, width() - 150, 20), Qt::AlignLeft | Qt::AlignVCenter,
               m_group.title + "  " + m_group.when.toString("yyyy-MM-dd HH:mm"));
    p.setFont(uiFont(12));
    p.setPen(kMuted);
    QString sub = ui::ago(m_group.when) + " · " + QString::number(m_group.members.size()) +
                  (m_group.members.size() == 1 ? " backup" : " backups");
    p.drawText(QRect(34, 30, width() - 150, 18), Qt::AlignLeft | Qt::AlignVCenter, sub);
    if (m_group.verdict >= 0) {
        QString v = m_group.verdict == 1 ? "restored exactly" : "NOT as before";
        p.setFont(uiFont(12, QFont::DemiBold));
        int w = p.fontMetrics().horizontalAdvance(v) + 20;
        QRect pill(width() - w - 10, height() / 2 - 12, w, 24);
        p.setPen(Qt::NoPen);
        p.setBrush(m_group.verdict == 1 ? ui::color(St::Pass) : ui::color(St::Fail));
        p.drawRoundedRect(pill, 12, 12);
        p.setPen(Qt::white);
        p.drawText(pill, Qt::AlignCenter, v);
    }
}

BackupRow::BackupRow(const BackupInfo& b, bool canRestore, QWidget* parent) : QWidget(parent) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(34, 6, 8, 6);
    row->setSpacing(10);
    auto* text = new QVBoxLayout;
    text->setSpacing(1);
    auto* title = new QLabel(QString("<b>%1</b> &nbsp;<span style='color:#5f6368'>%2 · %3</span>")
                                 .arg(ui::roleLabel(b.role), b.created.toString("yyyy-MM-dd HH:mm:ss"), ui::ago(b.created)));
    auto* what = new QLabel(QString("%1%2 fields · %3 macros · %4 stills · %5")
                                .arg(b.complete ? "" : "<span style='color:#c9431f'>incomplete</span> · ")
                                .arg(b.fields).arg(b.macros).arg(b.stills)
                                .arg(b.target.isEmpty() ? QString("USB") : b.target));
    what->setStyleSheet("color:#45484c; font-size:12px");
    title->setStyleSheet("font-size:13px");
    text->addWidget(title);
    text->addWidget(what);
    row->addLayout(text, 1);
    auto* open = new QPushButton("Folder");
    open->setObjectName("small");
    open->setToolTip(QDir::toNativeSeparators(b.dir));
    connect(open, &QPushButton::clicked, this, [dir = b.dir]() { QDesktopServices::openUrl(QUrl::fromLocalFile(dir)); });
    row->addWidget(open);
    auto* restore = new QPushButton("Restore");
    restore->setObjectName("small");
    restore->setEnabled(canRestore && b.complete);
    restore->setToolTip(!b.complete ? "An incomplete backup can't be restored"
                        : canRestore ? "Put the switcher's stored macros and stills back to this backup, then check"
                                     : "Select Real ATEM to restore");
    connect(restore, &QPushButton::clicked, this, [this, dir = b.dir]() { emit this->restore(dir); });
    row->addWidget(restore);
}

// ── Bars ─────────────────────────────────────────────────────

void ProgressStrip::setCounts(int pass, int fail, int skip, int total) {
    m_pass = pass;
    m_fail = fail;
    m_skip = skip;
    m_total = std::max(1, total);
    update();
}

void ProgressStrip::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), ui::color(St::Queued));
    qreal x = 0;
    for (auto [n, s] : { std::pair{ m_pass, St::Pass }, std::pair{ m_fail, St::Fail }, std::pair{ m_skip, St::Skip } }) {
        qreal w = width() * static_cast<qreal>(n) / m_total;
        p.fillRect(QRectF(x, 0, w, height()), ui::color(s));
        x += w;
    }
}

PhaseBar::PhaseBar(QWidget* parent) : QWidget(parent) { setFixedHeight(40); }

void PhaseBar::reset(const QStringList& phases) {
    m_phases = phases;
    m_state.clear();
    m_percent = 0;
    update();
}

void PhaseBar::setState(const QString& phase, const QString& state) {
    m_state[phase] = state;
    update();
}

void PhaseBar::setSweepPercent(int percent) {
    m_percent = percent;
    update();
}

void PhaseBar::paintEvent(QPaintEvent*) {
    static const QMap<QString, QString> names = { { "emulator", "Start the emulator" }, { "backup", "Back up" },
        { "tests", "Sweep" }, { "settings", "Put settings back" }, { "swept", "Back up what the sweep left" },
        { "restore", "Restore stored content" }, { "compare", "Check it is as before" } };
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setFont(uiFont(13));
    int x = 0;
    for (int i = 0; i < m_phases.size(); ++i) {
        const QString& ph = m_phases[i];
        QString st = m_state.value(ph);
        QString text = names.value(ph, ph);
        if (ph == "tests" && st == "start") text += QString(" %1%").arg(m_percent);
        QColor dot = st == "ok" ? ui::color(St::Pass) : st == "fail" || st == "unavailable" ? ui::color(St::Fail)
                   : st == "start" ? ui::color(St::Running) : QColor("#cfcac0");
        p.setPen(Qt::NoPen);
        p.setBrush(dot);
        p.drawEllipse(QPointF(x + 6, height() / 2.0), 5, 5);
        p.setPen(st.isEmpty() ? kMuted : kInk);
        QFont f = uiFont(13, st == "start" ? QFont::DemiBold : QFont::Normal);
        p.setFont(f);
        int w = p.fontMetrics().horizontalAdvance(text);
        p.drawText(QRect(x + 16, 0, w + 4, height()), Qt::AlignVCenter, text);
        x += 16 + w + 12;
        if (i + 1 < m_phases.size()) {
            p.setPen(QPen(QColor("#b9b4a8"), 1.5));
            p.drawLine(x, height() / 2, x + 16, height() / 2);
            x += 28;
        }
    }
}

SdkBar::SdkBar(const QString& file, QWidget* parent) : QWidget(parent) {
    static const QColor colors[] = { QColor("#2f8f7c"), QColor("#4f9fb8"), QColor("#7cb7a8"), QColor("#c9b68a"),
                                     QColor("#d8d3c7"), QColor("#e4572e") };
    QFile f(file);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        static const QRegularExpression line("^\\s+([1-6])\\s+\\S.*?\\s{2,}(\\d+)\\s+(.*)$");
        QTextStream in(&f);
        while (!in.atEnd()) {
            auto m = line.match(in.readLine());
            if (!m.hasMatch()) continue;
            int n = m.captured(1).toInt();
            m_parts << Part{ m.captured(3).trimmed(), m.captured(2).toInt(), colors[n - 1] };
            m_total += m.captured(2).toInt();
        }
    }
    setFixedHeight(m_parts.isEmpty() ? 0 : 54 + ((m_parts.size() + 2) / 3) * 22);
}

void SdkBar::paintEvent(QPaintEvent*) {
    if (m_parts.isEmpty()) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setFont(uiFont(14, QFont::DemiBold));
    p.setPen(kInk);
    p.drawText(QRect(0, 0, 220, 20), Qt::AlignVCenter, "The SDK behind it");
    p.setFont(uiFont(12));
    p.setPen(kMuted);
    p.drawText(QRect(160, 0, width() - 160, 20), Qt::AlignVCenter,
               QString("%1 methods of the switcher SDK, by what covers them").arg(m_total));
    qreal x = 0;
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 26, width(), 16), 5, 5);
    p.save();
    p.setClipPath(clip);
    for (const Part& part : m_parts) {
        qreal w = width() * static_cast<qreal>(part.count) / std::max(1, m_total);
        p.fillRect(QRectF(x, 26, w, 16), part.color);
        x += w;
    }
    p.restore();
    int colW = width() / 3;
    for (int i = 0; i < m_parts.size(); ++i) {
        int cx = (i % 3) * colW, cy = 50 + (i / 3) * 22;
        p.setPen(Qt::NoPen);
        p.setBrush(m_parts[i].color);
        p.drawRoundedRect(QRectF(cx, cy + 4, 12, 12), 3, 3);
        p.setPen(kInk);
        p.setFont(uiFont(12));
        QString text = m_parts[i].label;
        p.drawText(QRect(cx + 18, cy, colW - 70, 20), Qt::AlignVCenter, p.fontMetrics().elidedText(text, Qt::ElideRight, colW - 70));
        p.setPen(kMuted);
        p.setFont(monoFont(12));
        p.drawText(QRect(cx + colW - 60, cy, 44, 20), Qt::AlignVCenter | Qt::AlignRight, QString::number(m_parts[i].count));
    }
}
