#pragma once
#include "AtemState.h"
#include "InputSource.h"
#include "Compositor.h"
#include "PreviewWidget.h"
#include "SourceButton.h"
#include "commands.h"
#include "device.h"
#include "server.h"
#include <QMainWindow>
#include <QLabel>
#include <QSpinBox>
#include <QCheckBox>
#include <QListWidget>
#include <QLineEdit>
#include <QTextEdit>
#include <QTimer>
#include <QColorDialog>
#include <QBuffer>
#include <QCloseEvent>

struct EmulatorOptions {
    QString profileDir;                  // recorded switcher (dump.txt, macros.txt)
    QString listenAddress = "0.0.0.0";
    // Reference mode: the profile's state and macros only, nothing saved or
    // loaded — for checking the emulator with atem-sweep.
    bool reference = false;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(const EmulatorOptions& options, QWidget* parent = nullptr);
    ~MainWindow() override;

    bool isReady() const { return m_ready; }

protected:
    void closeEvent(QCloseEvent* e) override;

private slots:
    void onProgramButton(int sourceId);
    void onFillBtn(int srcIdx);
    void onKeyerBorderColorPick();
    void onInputColorPick(int i);
    void onInputMediaBrowse(int i);
    void onMacroRun();
    void onMacroUpdate();
    void onMacroSaveOutput();
    void onMacroSelectionChanged();
    void onMacroStarted(int index);
    void onMacroFinished(int index);
    void onRefreshPreview();
    void onClientCount(int total);
    void onLogMessage(const QString& msg);
    void onWebcamToggle();
    void onNetworkToggle(bool on);
    void syncFromDevice();

private:
    void buildUi();
    QWidget* buildLeftPanel();
    QWidget* buildRightPanel();
    QWidget* buildPgmBus();
    QWidget* buildDveSection();
    QWidget* buildMacroSection();
    static QWidget* sectionHeader(const char* title);
    static QSpinBox* makeSpin(int lo, int hi, int val, const QString& suffix = {});

    void uiLog(const QString& msg);
    void saveMacros();
    bool loadMacros();   // returns true if a saved macro pool was loaded
    static QString macroDataPath();
    void setProgramSource(quint16 src);
    void syncProgramButtons();
    void syncKeyerUi();
    void syncMacroList();
    void updateMacroStatus();
    void updateBorderColorBtn();
    void updateColorBtnStyle(int i);
    void updateSourceThumbs();
    void updateSnapshotDisplay(int slot);
    void applyMacroExtras(int slot);
    void pushWebcamFrame(const QImage& img);
    void updateVCamButtons();
    void updateNetButtons();
    static QString macroThumbPath(int slot);

    // Commands to the switcher (the same handlers the SDK's commands use)
    void apply(const char* command, const QByteArray& data);
    void sendDve(quint32 mask, const emu::cmd::DveParams& p);
    void sendSize(int percentX, int percentY);
    void sendCrop();
    Atem::KeDVState pipState(const emu::SwitcherView& v) const;
    QString sourceName(quint16 id) const;
    QString describeMacro(int slot) const;

    InputSource* sourceForId(quint16 id) const;

    // ── Switcher ─────────────────────────────────────────────────────
    EmulatorOptions  m_options;
    emu::Device      m_device;
    emu::Server      m_server;
    Compositor       m_compositor;
    bool             m_ready = false;
    QTimer           m_syncTimer;          // coalesces state changes into one UI refresh
    bool             m_fadeFromBlack = false;
    QTimer           m_saveTimer;          // saves the macro pool shortly after a change

    // Emulator-only picture settings and the per-macro extras the ATEM has
    // no place for (camera pictures, size lock, rotation, opacity).
    int m_rotation = 0;
    int m_opacity  = 100;
    struct MacroExtras {
        bool captured = false;
        bool lockSize = true;
        int  rotation = 0;
        int  opacity  = 100;
        Atem::InputSnap inputs[4];
    };
    QVector<MacroExtras> m_macroExtras;

    SolidColorSource*  m_sources[4]      = {};
    VideoFileSource*   m_videoSources[4] = {};
    StaticImageSource* m_photoSources[4] = {};
    bool               m_useVideo[4]     = {};
    bool               m_usePhoto[4]     = {};

    // ── Program bus ──────────────────────────────────────────────────
    SourceButton* m_pgmBtns[6]     = {};
    QPushButton*  m_pgmColorBtn[4] = {};
    QPushButton*  m_pgmMediaBtn[4] = {};

    // ── DVE / PiP controls ───────────────────────────────────────────
    SourceButton* m_fillBtns[6]    = {};
    QCheckBox*    m_lockSize       = nullptr;
    QPushButton*  m_borderColorBtn = nullptr;
    QSpinBox*     m_sizeXSpin      = nullptr;
    QSpinBox*     m_sizeYSpin      = nullptr;
    QSpinBox*     m_posXSpin       = nullptr;
    QSpinBox*     m_posYSpin       = nullptr;
    QSpinBox*     m_rotationSpin   = nullptr;
    QSpinBox*     m_borderSpin     = nullptr;
    QSpinBox*     m_opacitySpin    = nullptr;
    QSpinBox*     m_cropLSpin      = nullptr;
    QSpinBox*     m_cropRSpin      = nullptr;
    QSpinBox*     m_cropTSpin      = nullptr;
    QSpinBox*     m_cropBSpin      = nullptr;

    // ── Macros ───────────────────────────────────────────────────────
    QListWidget* m_macroList     = nullptr;
    QLineEdit*   m_macroNameEdit = nullptr;
    QTextEdit*   m_macroDescEdit = nullptr;
    QTextEdit*   m_snapshotView  = nullptr;
    QLabel*      m_macroStatus   = nullptr;
    QPushButton* m_macroRunBtn   = nullptr;

    // ── Virtual camera (DirectShow, shared memory) ────────────────────
    void*        m_vcamSharedMem  = nullptr;
    void*        m_vcamFrame      = nullptr;
    void*        m_vcamEvent      = nullptr;
    bool         m_vcamRegistered = false;
    bool         m_webcamActive   = false;
    QPushButton* m_vcamOnBtn      = nullptr;
    QPushButton* m_vcamOffBtn     = nullptr;

    // ── Network binding ───────────────────────────────────────────────
    bool         m_netActive  = false;
    QPushButton* m_netOnBtn   = nullptr;
    QPushButton* m_netOffBtn  = nullptr;

    // ── Misc ─────────────────────────────────────────────────────────
    PreviewWidget* m_preview      = nullptr;
    QLabel*        m_statusLabel  = nullptr;
    QTextEdit*     m_netLogView   = nullptr;
    QTextEdit*     m_uiLogView    = nullptr;
    QTimer*        m_refreshTimer = nullptr;
};
