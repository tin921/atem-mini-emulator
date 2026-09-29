#pragma once
// Waiting for SDK transfers (macro and still uploads/downloads) and for the
// media pool lock. Shared by the backup/restore and the storage tests.

#include <QByteArray>
#include <QString>
#include <QThread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>

#include "sdk.h"

constexpr int kTransferTimeoutMs = 20000;
constexpr int kLockTimeoutMs = 5000;

// One transfer at a time: the callback reports completion for an index.
struct TransferWait {
    std::mutex m;
    std::condition_variable cv;
    int index = -1;
    QString result;             // "done", "failed", "cancelled"
    QByteArray bytes;           // stills: copied in the callback (the frame is only lent)
    int width = 0, height = 0, rowBytes = 0;
    uint32_t pixelFormat = 0;

    void start(int i) {
        std::lock_guard<std::mutex> lock(m);
        index = i;
        result.clear();
        bytes.clear();
    }
    void finish(int i, const QString& r) {
        std::lock_guard<std::mutex> lock(m);
        if (i != index || !result.isEmpty()) return;
        result = r;
        cv.notify_all();
    }
    QString wait(int timeoutMs = kTransferTimeoutMs) {
        std::unique_lock<std::mutex> lock(m);
        cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return !result.isEmpty(); });
        return result.isEmpty() ? QString("timed out") : result;
    }
};

// A macro pool sink feeding a TransferWait (caller adds / removes / releases).
inline IBMDSwitcherMacroPoolCallback* macroTransferSink(TransferWait& wait) {
    return new Sink<IBMDSwitcherMacroPoolCallback, BMDSwitcherMacroPoolEventType, unsigned int, IBMDSwitcherTransferMacro*>(
        [&wait](BMDSwitcherMacroPoolEventType t, unsigned int i, IBMDSwitcherTransferMacro*) {
            if (t == bmdSwitcherMacroPoolEventTypeTransferCompleted) wait.finish(static_cast<int>(i), "done");
            else if (t == bmdSwitcherMacroPoolEventTypeTransferFailed) wait.finish(static_cast<int>(i), "failed");
            else if (t == bmdSwitcherMacroPoolEventTypeTransferCancelled) wait.finish(static_cast<int>(i), "cancelled");
        });
}

// A stills sink feeding a TransferWait; downloaded frames are copied into it.
inline IBMDSwitcherStillsCallback* stillTransferSink(TransferWait& wait) {
    return new Sink<IBMDSwitcherStillsCallback, BMDSwitcherMediaPoolEventType, IBMDSwitcherFrame*, int>(
        [&wait](BMDSwitcherMediaPoolEventType t, IBMDSwitcherFrame* frame, int i) {
            if (t == bmdSwitcherMediaPoolEventTypeTransferCompleted) {
                void* bytes = nullptr;
                if (frame && SUCCEEDED(frame->GetBytes(&bytes)) && bytes) {
                    std::lock_guard<std::mutex> lock(wait.m);
                    if (i == wait.index) {
                        wait.width = frame->GetWidth();
                        wait.height = frame->GetHeight();
                        wait.rowBytes = frame->GetRowBytes();
                        wait.pixelFormat = static_cast<uint32_t>(frame->GetPixelFormat());
                        wait.bytes = QByteArray(static_cast<const char*>(bytes), wait.rowBytes * wait.height);
                    }
                }
                wait.finish(i, "done");
            } else if (t == bmdSwitcherMediaPoolEventTypeTransferFailed) {
                wait.finish(i, "failed");
            } else if (t == bmdSwitcherMediaPoolEventTypeTransferCancelled) {
                wait.finish(i, "cancelled");
            }
        });
}

class LockSink : public IBMDSwitcherLockCallback {
public:
    std::atomic<bool> obtained{ false };
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (iid == IID_IUnknown || iid == __uuidof(IBMDSwitcherLockCallback)) {
            *ppv = static_cast<IBMDSwitcherLockCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG c = --m_ref; if (!c) delete this; return c; }
    HRESULT STDMETHODCALLTYPE Obtained() override { obtained = true; return S_OK; }
private:
    std::atomic<ULONG> m_ref{ 1 };
};

inline bool pollUntil(const std::function<bool()>& cond, int ms) {
    for (int waited = 0; waited < ms; waited += 20) {
        if (cond()) return true;
        QThread::msleep(20);
    }
    return cond();
}
