#pragma once
// atem-sweep-gui's window: the coverage map on the left (every test a
// square), the drill-down / backup list on the right. Runs atem-sweep (and,
// for the emulator, atem-emu) as child processes.

#include <QHash>
#include <QMainWindow>
#include <QMap>
#include <QProcess>
#include <QSet>
#include <functional>

#include "model.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QScrollArea;
class QToolButton;
class QVBoxLayout;
class CellGrid;
class PhaseBar;
class ProgressStrip;
class TestRow;
class NavRow;

class Window : public QMainWindow {
    Q_OBJECT
public:
    Window();
    ~Window() override;
    // For --screenshot: open a view as if clicked.
    void showInterface(const QString& iface, const QString& prop = {});
    void showBackups();
    // --selftest: run these groups against the emulator (never the real ATEM),
    // then call done.
    void selfTest(const QStringList& groups, std::function<void()> done);

protected:
    void closeEvent(QCloseEvent*) override;

private:
    // Setup
    QWidget* buildHeader();
    QWidget* buildLegend();
    QWidget* buildSide();
    bool loadPlan(QString* error);
    void loadGolden();
    void setTarget(bool emulator);

    // Views
    void rebuildMap();
    void rebuildList();
    void rebuildBackups();
    void refresh();                 // counts, bars, labels, repaint
    void updateCard();
    void updateBackupButton();
    void select(int test);
    void setHover(Hover h);
    QVector<int> scope() const;     // tests of the current drill-down
    int level() const;              // 0 interfaces, 1 properties, 2 tests

    // Running
    void run();
    void stop();
    bool ensureEmulator(QString* error);
    void startSweep(const QStringList& args, const QString& outDir);
    void onOutput();
    void onLine(const QString& line);
    void onFinished(int code);
    void backupNow();
    void restoreFrom(const QString& dir);
    bool busy() const;
    void setStatus(const QString& text);

    // Report
    void saveReport();

    Model m_model;
    QHash<QString, int> m_index;
    QString m_iface, m_prop;
    bool m_emulator = true;
    enum class Side { Tests, Backups } m_side = Side::Tests;
    QSet<QString> m_openGroups;

    // Header
    QLabel* m_subtitle = nullptr;
    QLabel* m_pct = nullptr;
    QLabel* m_pct1 = nullptr;
    QLabel* m_pct2 = nullptr;
    QPushButton* m_emuBtn = nullptr;
    QPushButton* m_atemBtn = nullptr;
    QComboBox* m_address = nullptr;
    QComboBox* m_golden = nullptr;
    QPushButton* m_runBtn = nullptr;
    QPushButton* m_backupBtn = nullptr;
    ProgressStrip* m_strip = nullptr;
    PhaseBar* m_phases = nullptr;

    // Legend
    QPushButton* m_showPass = nullptr;
    QPushButton* m_showFail = nullptr;
    QPushButton* m_showSkip = nullptr;
    QLabel* m_notRun = nullptr;

    // Map
    QVBoxLayout* m_mapLayout = nullptr;
    struct Section { QString group; QCheckBox* box; QLabel* summary; CellGrid* grid; QWidget* widget; };
    QList<Section> m_sections;
    QList<CellGrid*> m_grids;
    QLabel* m_zoomSummary = nullptr;
    QMap<QString, bool> m_groupOn;

    // Side
    QWidget* m_card = nullptr;
    QLabel* m_badge = nullptr;
    QLabel* m_testNo = nullptr;
    QLabel* m_testId = nullptr;
    QLabel* m_testTitle = nullptr;
    QLabel* m_values = nullptr;
    QToolButton* m_back = nullptr;
    QLabel* m_listTitle = nullptr;
    QLabel* m_listHint = nullptr;
    QPushButton* m_backupNow = nullptr;
    QVBoxLayout* m_listLayout = nullptr;
    QList<NavRow*> m_navRows;
    QList<TestRow*> m_testRows;

    // Processes
    QProcess* m_sweep = nullptr;
    QProcess* m_emu = nullptr;
    enum class Job { None, Sweep, Backup, Restore } m_job = Job::None;
    QByteArray m_buffer;
    QStringList m_log;
    QString m_outDir, m_stopFile;
    QJsonObject m_summary;
    bool m_stopping = false;
    int m_done = 0, m_total = 0;
    QString m_runTarget, m_runMode, m_runGolden;
    QDateTime m_runStarted;
    std::function<void()> m_whenDone;
};
