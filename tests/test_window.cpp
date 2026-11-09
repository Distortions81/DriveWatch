#include "window.h"
#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QLabel>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QPushButton>
#include <QTextEdit>
#include <QTest>

class WindowTests : public QObject {
    Q_OBJECT
private slots:
    void previewShowsDualUnitsAndNoWrites() {
        Window window(true);
        window.poll();
        auto *drives = window.findChild<QTableWidget *>("driveTable");
        QVERIFY(drives);
        QCOMPARE(drives->rowCount(), 7);
        bool dual = false;
        for (int i = 0; i < drives->rowCount(); ++i) {
            dual |= drives->item(i, 3)->text() == "45°C / 113°F";
            QVERIFY(drives->item(i, 3)->foreground().color().isValid());
        }
        QVERIFY(dual);
        QCOMPARE(window.findChild<QLabel *>("summaryConnection")->text(), QString("Internal: NVMe"));
        QCOMPARE(window.findChild<QLabel *>("summaryConnectionspeed")->text(), QString("PCIe 8.0 GT/s ×4"));
        const auto path = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/state.json";
        QVERIFY(!QFile::exists(path));
    }
    void driveCheckboxSuppressesWarningsAndPopupAndKeepsLabels() {
        Window window(true, true);
        window.poll();
        auto *table = window.findChild<QTableWidget *>("driveTable");
        int hotRow = -1;
        for (int i = 0; i < table->rowCount(); ++i) {
            QCOMPARE(table->item(i, 0)->checkState(), i < 5 ? Qt::Checked : Qt::Unchecked);
            if (table->item(i, 3)->text() == "78°C / 172°F") hotRow = i;
        }
        QVERIFY(hotRow >= 0);
        QCOMPARE(table->columnCount(), 5);
        QVERIFY(table->alternatingRowColors());
        QVERIFY(table->item(5, 4)->text().contains("External USB"));
        QCOMPARE(table->item(5, 0)->text(), QString("Backups"));
        QCOMPARE(table->item(5, 1)->text(), QString("Seagate Expansion SW"));
        QCOMPARE(table->item(6, 2)->text(), QString("Monitoring off"));
        for (int i = 0; i < table->rowCount(); ++i)
            for (int col = 0; col < table->columnCount(); ++col) {
                QVERIFY(!table->item(i, col)->text().contains("/dev/"));
                QVERIFY(!table->item(i, col)->text().contains("/media/"));
            }
        auto *details = window.findChild<QTabWidget *>("driveDetails");
        QVERIFY(details); QCOMPARE(details->currentIndex(), 0);
        table->selectRow(5);
        QCOMPARE(window.findChild<QLabel *>("summaryConnection")->text(), QString("External: USB"));
        QCOMPARE(window.findChild<QLabel *>("summaryConnectionspeed")->text(), QString("5 Gb/s"));
        auto *volumes = window.findChild<QTableWidget *>("volumeTable");
        QCOMPARE(volumes->item(0, 0)->text(), QString("Backups"));
        QCOMPARE(volumes->item(0, 1)->text(), QString("/media/Backups"));
        auto *diagnostics = window.findChild<QTextEdit *>("diagnosticOutput");
        QVERIFY(diagnostics->isHidden());
        details->setCurrentIndex(1);
        window.findChild<QPushButton *>("diagnosticsToggle")->setChecked(true);
        QVERIFY(!diagnostics->isHidden());
        window.poll();
        QCOMPARE(details->currentIndex(), 1);
        table->item(hotRow, 0)->setCheckState(Qt::Unchecked);
        QTest::qWait(30);
        QCOMPARE(table->item(hotRow, 2)->text(), QString("Monitoring off"));
        window.poll();
        QCOMPARE(table->item(hotRow, 0)->checkState(), Qt::Unchecked);
        bool hotVisible = false;
        for (auto *label : window.trayPopup()->findChildren<QLabel *>())
            hotVisible |= !label->isHidden() && label->text() == "78°C / 172°F";
        QVERIFY(!hotVisible);
        table->item(hotRow, 0)->setCheckState(Qt::Checked);
        QTest::qWait(30);
        QCOMPARE(table->item(hotRow, 2)->text(), QString("Critical"));
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
    void legacyUserDataMigratesWithoutOverwritingNewData() {
        const auto dataRoot = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        const auto legacyPath = dataRoot + "/DriveWatch/DriveWatch/state.json";
        QVERIFY(QDir().mkpath(QFileInfo(legacyPath).absolutePath()));
        QFile legacy(legacyPath);
        QVERIFY(legacy.open(QIODevice::WriteOnly));
        legacy.write(R"({"version":1,"migration_marker":"retained"})"); legacy.close();
        QSettings oldSettings("DriveWatch", "DriveWatch");
        oldSettings.setValue("notifications", false); oldSettings.sync();
        const auto oldStartup = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
                              + "/autostart/io.github.drivewatch.desktop";
        QVERIFY(QDir().mkpath(QFileInfo(oldStartup).absolutePath()));
        QFile startup(oldStartup); QVERIFY(startup.open(QIODevice::WriteOnly));
        startup.write("[Desktop Entry]\nHidden=true\n"); startup.close();
        const auto statePath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/state.json";
        { Window preview(true); QVERIFY(!QFile::exists(statePath)); }
        {
            Window window(false);
            QFile state(statePath); QVERIFY(state.open(QIODevice::ReadOnly));
            QVERIFY(state.readAll().contains("retained"));
            QSettings settings("M45Software", "m45-drive-watcher");
            QCOMPARE(settings.value("notifications").toBool(), false);
            for (auto *checkbox : window.findChildren<QCheckBox *>())
                if (checkbox->text().contains("log in")) QVERIFY(!checkbox->isChecked());
            settings.setValue("notifications", true); settings.sync();
        }
        QVERIFY(legacy.open(QIODevice::WriteOnly | QIODevice::Truncate));
        legacy.write(R"({"version":1,"migration_marker":"changed"})"); legacy.close();
        {
            Window window(false);
            QFile state(statePath); QVERIFY(state.open(QIODevice::ReadOnly));
            QVERIFY(state.readAll().contains("retained"));
            QSettings settings("M45Software", "m45-drive-watcher");
            QCOMPARE(settings.value("notifications").toBool(), true);
        }
        QVERIFY(QFile::exists(legacyPath));
        QFile::remove(oldStartup);
        QFile::remove(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
                      + "/autostart/com.m45software.DriveWatcher.desktop");
    }
    void startupPreferenceCreatesAndUpdatesDesktopOverride() {
        QSettings settings("M45Software", "m45-drive-watcher");
        settings.setValue("notifications", false); settings.sync();
        Window window(false);
        QCheckBox *startup = nullptr;
        for (auto *checkbox : window.findChildren<QCheckBox *>())
            if (checkbox->text().contains("log in")) startup = checkbox;
        QVERIFY(startup);
        QVERIFY(startup->isChecked());
        startup->setChecked(false);
        const auto path = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
                        + "/autostart/com.m45software.DriveWatcher.desktop";
        QFile f(path); QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().contains("Hidden=true")); f.close();
        startup->setChecked(true);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().contains("Hidden=false"));
    }
    void popupReadingsStayVisibleAcrossRefreshes() {
        Window window(true, true);
        window.poll();
        window.showTrayPopup();
        auto *popup = window.trayPopup();
        const QRegularExpression temperature("^\\d+°C / \\d+°F$");
        for (int refresh = 0; refresh < 4; ++refresh) {
            // Leave the popup mapped through a real 10-second background timer tick too.
            QTest::qWait(refresh == 3 ? 10500 : 30);
            QVERIFY(popup->isVisible());
            int visibleTemperatures = 0;
            for (auto *label : popup->findChildren<QLabel *>())
                if (temperature.match(label->text()).hasMatch() && label->isVisibleTo(popup))
                    ++visibleTemperatures;
            QCOMPARE(visibleTemperatures, 5);
            window.poll();
        }
    }
};

int main(int argc, char **argv) {
    QTemporaryDir sandbox;
    if (!sandbox.isValid()) return 1;
    qputenv("XDG_CONFIG_HOME", (sandbox.path() + "/config").toUtf8());
    qputenv("XDG_DATA_HOME", (sandbox.path() + "/data").toUtf8());
    qputenv("XDG_CACHE_HOME", (sandbox.path() + "/cache").toUtf8());
    QApplication app(argc, argv);
    app.setApplicationName("m45-drive-watcher");
    app.setOrganizationName("M45Software");
    app.setQuitOnLastWindowClosed(false);
    WindowTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "test_window.moc"
