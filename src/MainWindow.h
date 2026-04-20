#pragma once
#include "AtemState.h"
#include "AtemServer.h"
#include "InputSource.h"
#include "Compositor.h"
#include "MacroEngine.h"
#include "PreviewWidget.h"
#include "SourceButton.h"
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

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

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
    void onCmdProgramInput(quint16 src);
    void onCmdCut();
    void onCmdKeyerOn(bool on);
    void onCmdKeyerDVE(quint16 fillSrc, quint32 sX, quint32 sY, qint32 pX, qint32 pY);
    void onCmdMacroRun(quint16 index);
    void onCmdMacroStop();
    void onMacroActionApply(const Atem::MacroAction& action);
    void onMacroStarted(int index);
    void onMacroFinished(int index);
    void onRefreshPreview();
    void onClientConnected(int total);
    void onClientDisconnected(int total);
    void onLogMessage(const QString& msg);
    void onWebcamToggle();
    void onNetworkToggle(bool on);

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
    void setProgramSource(quint16 src);
    void syncProgramButtons();
    void syncKeyerUi();
    void syncMacroList();
    void updateBorderColorBtn();
    void updateColorBtnStyle(int i);
    void updateSourceThumbs();
    void updateSnapshotDisplay(int slot);
    void applyMacroSnapshot(int slot);
    void pushWebcamFrame(const QImage& img);
    void updateVCamButtons();
    void updateNetButtons();
    static QString macroThumbPath(int slot);

    InputSource* sourceForId(quint16 id) const;
    int sourceIdToComboIndex(quint16 id) const;

    // ── State ────────────────────────────────────────────────────────
    Atem::ATEMState  m_state;
    Atem::AtemServer m_server;
    MacroEngine      m_macroEngine;
    Compositor       m_compositor;

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
