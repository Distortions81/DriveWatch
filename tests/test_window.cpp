#include "window.h"
#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QLabel>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>

class WindowTests : public QObject {
    Q_OBJECT
private slots:
    void previewShowsDualUnitsAndNoWrites() {
        Window window(true);
        window.poll();
        auto *drives = window.findChild<QTableWidget *>("driveTable");
        QVERIFY(drives);
        QCOMPARE(drives->rowCount(), 5);
        bool dual = false;
        for (int i = 0; i < drives->rowCount(); ++i) {
            dual |= drives->item(i, 2)->text() == "45°C / 113°F";
            QVERIFY(drives->item(i, 2)->foreground().color().isValid());
        }
        QVERIFY(dual);
        const auto path = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/state.json";
        QVERIFY(!QFile::exists(path));
    }
    void popupShowsReadingsAndActions() {
        Window window(true, true);
        window.poll();
        auto *popup = window.trayPopup();
        bool hot = false;
        for (auto *label : popup->findChildren<QLabel *>()) hot |= label->text() == "78°C / 172°F";
        QVERIFY(hot);
        window.showTrayPopup();
        QVERIFY(popup->isVisible());
        QVERIFY(popup->sizeHint().width() < 600);
        window.showTrayPopup();
        QVERIFY(!popup->isVisible());
    }
    void startupPreferenceCreatesAndUpdatesDesktopOverride() {
        QSettings settings("DriveWatch", "DriveWatch");
        settings.setValue("notifications", false); settings.sync();
        Window window(false);
        QCheckBox *startup = nullptr;
        for (auto *checkbox : window.findChildren<QCheckBox *>())
            if (checkbox->text().contains("log in")) startup = checkbox;
        QVERIFY(startup);
        QVERIFY(startup->isChecked());
        startup->setChecked(false);
        const auto path = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
                        + "/autostart/io.github.drivewatch.desktop";
        QFile f(path); QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().contains("Hidden=true")); f.close();
        startup->setChecked(true);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().contains("Hidden=false"));
    }
};

int main(int argc, char **argv) {
    QTemporaryDir sandbox;
    if (!sandbox.isValid()) return 1;
    qputenv("XDG_CONFIG_HOME", (sandbox.path() + "/config").toUtf8());
    qputenv("XDG_DATA_HOME", (sandbox.path() + "/data").toUtf8());
    qputenv("XDG_CACHE_HOME", (sandbox.path() + "/cache").toUtf8());
    QApplication app(argc, argv);
    app.setApplicationName("DriveWatchTest");
    app.setOrganizationName("DriveWatchTest");
    app.setQuitOnLastWindowClosed(false);
    WindowTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "test_window.moc"
