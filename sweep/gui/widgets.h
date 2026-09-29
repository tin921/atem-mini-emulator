#pragma once
// Painted pieces of atem-sweep-gui: the squares, the list rows, the bars.

#include <QMap>
#include <QPushButton>
#include <QWidget>

#include "model.h"

// Squares, one per test, in rows that fill the width.
class CellGrid : public QWidget {
    Q_OBJECT
public:
    CellGrid(Model* model, QWidget* parent = nullptr);
    void setCells(const QVector<int>& tests, int size);
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;

signals:
    void clicked(int test);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    bool event(QEvent*) override;

private:
    int columns(int width) const;
    int testAt(const QPoint& p) const;
    int gap() const { return m_size > 20 ? 6 : 2; }

    Model* m_model;
    QVector<int> m_tests;
    int m_size = 9;
};

// A row that leads further in: label, pass/differ/skip bar, count, chevron.
class NavRow : public QWidget {
    Q_OBJECT
public:
    NavRow(Model* model, const QString& label, const QVector<int>& tests, QWidget* parent = nullptr);
    QSize sizeHint() const override { return { 360, 40 }; }
    QString label() const { return m_label; }

signals:
    void clicked();
    void hovered(bool on);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override { emit clicked(); }
    void enterEvent(QEnterEvent*) override { m_hot = true; update(); emit hovered(true); }
    void leaveEvent(QEvent*) override { m_hot = false; update(); emit hovered(false); }

private:
    Model* m_model;
    QString m_label;
    QVector<int> m_tests;
    bool m_hot = false;
};

// One test: status, value (or id), and both values where they differ.
class TestRow : public QWidget {
    Q_OBJECT
public:
    TestRow(Model* model, int test, bool showValue, QWidget* parent = nullptr);
    QSize sizeHint() const override;
    int test() const { return m_test; }
    void refresh() { updateGeometry(); update(); }

signals:
    void clicked();
    void hovered(bool on);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override { emit clicked(); }
    void enterEvent(QEnterEvent*) override { m_hot = true; update(); emit hovered(true); }
    void leaveEvent(QEvent*) override { m_hot = false; update(); emit hovered(false); }

private:
    Model* m_model;
    int m_test;
    bool m_showValue;
    bool m_hot = false;
};

// The backups of one sweep: a header that opens and closes the list.
class BackupGroupRow : public QWidget {
    Q_OBJECT
public:
    BackupGroupRow(const BackupGroup& group, bool open, QWidget* parent = nullptr);
    QSize sizeHint() const override { return { 360, 56 }; }

signals:
    void toggled();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override { emit toggled(); }
    void enterEvent(QEnterEvent*) override { m_hot = true; update(); }
    void leaveEvent(QEvent*) override { m_hot = false; update(); }

private:
    BackupGroup m_group;
    bool m_open, m_hot = false;
};

// One backup: when, what it holds, open its folder, restore it.
class BackupRow : public QWidget {
    Q_OBJECT
public:
    BackupRow(const BackupInfo& backup, bool canRestore, QWidget* parent = nullptr);

signals:
    void restore(const QString& dir);
};

// The run's progress as a thin strip: same, differ, skipped.
class ProgressStrip : public QWidget {
    Q_OBJECT
public:
    explicit ProgressStrip(QWidget* parent = nullptr) : QWidget(parent) { setFixedHeight(4); }
    void setCounts(int pass, int fail, int skip, int total);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    int m_pass = 0, m_fail = 0, m_skip = 0, m_total = 1;
};

// The steps of a run on the real switcher: backup, sweep, settings, ...
class PhaseBar : public QWidget {
    Q_OBJECT
public:
    explicit PhaseBar(QWidget* parent = nullptr);
    void reset(const QStringList& phases);
    void setState(const QString& phase, const QString& state);
    void setSweepPercent(int percent);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QStringList m_phases;
    QMap<QString, QString> m_state;
    int m_percent = 0;
};

// The SDK's methods by what covers them (coverage/api-categories.txt).
class SdkBar : public QWidget {
    Q_OBJECT
public:
    explicit SdkBar(const QString& categoriesFile, QWidget* parent = nullptr);
    QSize sizeHint() const override { return { 600, 120 }; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    struct Part { QString label; int count; QColor color; };
    QList<Part> m_parts;
    int m_total = 0;
};
