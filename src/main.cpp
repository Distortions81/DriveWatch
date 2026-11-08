#include "window.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDir>
#include <QFileInfo>
#include <QTimer>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("m45-drive-watcher");
    app.setApplicationDisplayName("M45 Drive Watcher");
    app.setOrganizationName("M45Software");
    app.setOrganizationDomain("m45software.com");
    app.setApplicationVersion(M45_DRIVE_WATCHER_VERSION);
    app.setDesktopFileName("com.m45software.DriveWatcher");
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser args;
    args.setApplicationDescription("Drive health monitoring for your system tray");
    args.addHelpOption();
    args.addVersionOption();
    args.addOption({"show", "Open the drive overview on launch."});
    args.addOption({"demo", "Preview with synthetic data; never change real monitoring state or settings."});
    args.addOption({"demo-warning", "Show synthetic warning conditions (requires --demo)."});
    args.addOption({"page", "Initial page: drives, history, settings, tray.", "page", "drives"});
    args.addOption({"screenshot", "Save a demo screenshot and exit (requires --demo).", "file"});
    args.process(app);
    const bool demo = args.isSet("demo");
    if ((args.isSet("screenshot") || args.isSet("demo-warning")) && !demo) args.showHelp(2);
    const int page = QStringList{"drives", "history", "settings", "tray"}.indexOf(args.value("page"));
    if (page < 0) args.showHelp(2);
    auto bus = QDBusConnection::sessionBus();
    if (!demo) {
        if (!bus.isConnected()) qFatal("A desktop D-Bus session is required.");
        if (!bus.registerService("com.m45software.DriveWatcher")) {
            QDBusInterface existing("com.m45software.DriveWatcher", "/M45DriveWatcher", "com.m45software.DriveWatcher", bus);
            existing.call("Show");
            return 0;
        }
    }
    Window window(demo, args.isSet("demo-warning"), page);
    if (!demo) bus.registerObject("/M45DriveWatcher", &window, QDBusConnection::ExportAllSlots);
    if (page == 3) QTimer::singleShot(100, &window, &Window::showTrayPopup);
    else if (demo || args.isSet("show")) window.Show();
    if (args.isSet("screenshot")) {
        QTimer::singleShot(600, &app, [&] {
            const QString path = QFileInfo(args.value("screenshot")).absoluteFilePath();
            QWidget *target = page == 3 ? window.trayPopup() : &window;
            app.exit(target->grab().save(path) ? 0 : 1);
        });
    }
    return app.exec();
}
