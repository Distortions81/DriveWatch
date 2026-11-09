#pragma once
#include "monitor.h"
#include <QMainWindow>
#include <QSettings>
#include <QSystemTrayIcon>

class QTableWidget;
class QLabel;
class QTextEdit;
class QSpinBox;
class QCheckBox;
class QMenu;
class QTabWidget;
class QScrollArea;
class QPushButton;

class Window : public QMainWindow {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.m45software.DriveWatcher")
public:
    explicit Window(bool demo = false, bool demoWarning = false, int initialTab = 0);
    void poll();
    QWidget *trayPopup() const;
    void showTrayPopup();
public slots:
    void Show();
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    bool m_demo;
    bool m_demoWarning;
    bool m_saveFailed = false;
    QString m_statePath;
    QString m_stateError;
    Monitor m_monitor;
    QSettings m_settings;
    QSystemTrayIcon m_tray;
    QLabel *m_status;
    QLabel *m_subtitle;
    QLabel *m_detailName;
    QLabel *m_detailModel;
    QLabel *m_detailMessage;
    QMap<QString, QLabel *> m_summaryFields;
    QMap<QString, QLabel *> m_technicalFields;
    QTableWidget *m_volumes;
    QTableWidget *m_counters;
    QPushButton *m_diagnosticsToggle;
    QTableWidget *m_drives;
    QTableWidget *m_history;
    QTextEdit *m_details;
    QSpinBox *m_warm;
    QSpinBox *m_hot;
    QLabel *m_warmF;
    QLabel *m_hotF;
    QCheckBox *m_startup;
    QCheckBox *m_notifications;
    QMenu *m_popup;
    QWidget *m_mini;
    QScrollArea *m_miniScroll;
    struct MiniDriveRow {
        QWidget *widget;
        QLabel *icon;
        QLabel *model;
        QLabel *detail;
        QLabel *temperature;
    };
    QMap<QString, MiniDriveRow> m_miniRows;
    QLabel *m_miniSummary;
    QLabel *m_miniCoverage;
    QTabWidget *m_tabs;
    Policy policy() const;
    void render();
    void renderMini();
    void details();
    void save();
    void notify(const QString &title, const QString &text, bool critical = false);
    bool setAutostart(bool enabled);
};
