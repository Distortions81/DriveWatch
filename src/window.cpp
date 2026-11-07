#include "window.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCursor>
#include <QCloseEvent>
#include <QDateTime>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusVariant>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <algorithm>
#include <cmath>

namespace {
QString field(const QJsonObject &o, const QString &key, const QString &suffix = {}) {
    auto v = o.value(key);
    if (v.isUndefined() || v.isNull()) return QString::fromUtf8("—");
    return (v.isString() ? v.toString() : QString::number(v.toDouble())) + suffix;
}
QString temperatureText(double c) {
    return displayTemperature(c);
}
QColor statColor(double fraction) {
    // Palette-aware contrast, with an ordered green -> amber -> red gradient.
    const bool dark = qApp->palette().color(QPalette::Base).lightnessF() < 0.5;
    const QColor green(dark ? "#68d6b5" : "#137a59");
    const QColor amber(dark ? "#f4c366" : "#916100");
    const QColor red(dark ? "#ff8585" : "#b32632");
    fraction = std::clamp(fraction, 0.0, 1.0);
    const QColor a = fraction <= 0.5 ? green : amber;
    const QColor b = fraction <= 0.5 ? amber : red;
    const double weight = fraction <= 0.5 ? fraction * 2 : (fraction - 0.5) * 2;
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * weight,
                            a.greenF() + (b.greenF() - a.greenF()) * weight,
                            a.blueF() + (b.blueF() - a.blueF()) * weight);
}
double temperatureRisk(double c, const Policy &p) {
    const double low = p.warm - 15.0;
    return c <= p.warm ? 0.5 * std::clamp((c - low) / 15.0, 0.0, 1.0)
                       : 0.5 + 0.5 * std::clamp((c - p.warm) / (p.hot - p.warm), 0.0, 1.0);
}
void colorLabel(QLabel *label, double risk) {
    auto palette = label->palette();
    palette.setColor(QPalette::WindowText, statColor(risk));
    label->setPalette(palette);
}
QIcon statusIcon(bool warn) {
    QPixmap p(64, 64);
    p.fill(Qt::transparent);
    QPainter painter(&p);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(warn ? QColor("#e8a23b") : QColor("#54cbb8"), 4));
    painter.setBrush(QColor("#263240"));
    painter.drawRoundedRect(QRectF(10, 6, 44, 51), 8, 8);
    if (warn) {
        painter.drawLine(32, 18, 32, 35);
        painter.setBrush(QColor("#e8a23b"));
        painter.drawEllipse(QPointF(32, 44), 2, 2);
    } else {
        QPolygonF pulse;
        pulse << QPointF(17, 33) << QPointF(25, 33) << QPointF(30, 22)
              << QPointF(37, 43) << QPointF(43, 33) << QPointF(48, 33);
        painter.drawPolyline(pulse);
    }
    return QIcon(p);
}
QJsonObject demoSnapshot() {
    QJsonArray drives;
    const QStringList models{"Crucial BX100 1 TB", "Samsung 970 EVO 2 TB", "Samsung 512 GB", "Samsung 512 GB", "Crucial P3 2 TB"};
    const QList<int> temps{30, 45, 26, 27, 34};
    for (int i = 0; i < 5; ++i) {
        drives.append(QJsonObject{{"id", QString("demo-%1").arg(i)}, {"model", models[i]},
            {"device", i == 0 ? "/dev/sda" : QString("/dev/nvme%1").arg(i - 1)},
            {"serial", QString("DEMO-%1").arg(i)}, {"protocol", i == 0 ? "ATA" : "NVMe"},
            {"temperature", temps[i]}, {"passed", true}, {"available_spare", i == 1 ? 92 : 100},
            {"percentage_used", i == 4 ? 21 : 3},
            {"counters", QJsonObject{{"media_errors", i == 1 ? "1135" : "0"}}}});
    }
    return {{"version", 1}, {"collected_at", QDateTime::currentSecsSinceEpoch()}, {"drives", drives}};
}
QString autostartPath() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart/io.github.drivewatch.desktop";
}
}

Window::Window(bool demo, bool demoWarning, int initialTab)
    : m_demo(demo), m_demoWarning(demoWarning), m_settings(demo ? "DriveWatchPreview" : "DriveWatch", "DriveWatch") {
    setWindowTitle(demo ? "DriveWatch — Preview" : "DriveWatch");
    setWindowIcon(statusIcon(false));
    resize(1040, 710);
    m_statePath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/state.json";
    if (!demo) {
        QFile f(m_statePath);
        if (f.exists()) {
            if (!f.open(QIODevice::ReadOnly)) m_stateError = "Cannot read saved baseline. Fix access and restart DriveWatch.";
            else {
                QJsonParseError error;
                auto doc = QJsonDocument::fromJson(f.readAll(), &error);
                if (error.error != QJsonParseError::NoError || !doc.isObject() || doc.object().value("version").toInt() != 1)
                    m_stateError = "Saved baseline is invalid. Preserve state.json and restore a backup before restarting.";
                else m_monitor = Monitor(doc.object());
            }
        }
    }
    auto *central = new QWidget;
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(12);
    auto *title = new QLabel("DriveWatch");
    auto titleFont = title->font(); titleFont.setPointSize(23); titleFont.setBold(true); title->setFont(titleFont);
    layout->addWidget(title);
    m_status = new QLabel;
    auto statusFont = m_status->font(); statusFont.setPointSize(14); statusFont.setBold(true); m_status->setFont(statusFont);
    m_status->setTextFormat(Qt::PlainText);
    layout->addWidget(m_status);
    m_subtitle = new QLabel;
    m_subtitle->setTextFormat(Qt::PlainText);
    m_subtitle->setWordWrap(true);
    layout->addWidget(m_subtitle);
    auto *tabs = new QTabWidget;
    m_tabs = tabs;
    layout->addWidget(tabs, 1);
    auto *overview = new QWidget;
    auto *ov = new QVBoxLayout(overview);
    auto *splitter = new QSplitter(Qt::Vertical);
    m_drives = new QTableWidget(0, 6);
    m_drives->setObjectName("driveTable");
    m_drives->setHorizontalHeaderLabels({"Drive", "Device", "Temperature", "Endurance used", "Spare", "Status"});
    m_drives->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_drives->setSelectionMode(QAbstractItemView::SingleSelection);
    m_drives->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_drives->verticalHeader()->hide();
    m_drives->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_drives->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_drives->setAlternatingRowColors(true);
    splitter->addWidget(m_drives);
    m_details = new QTextEdit;
    m_details->setReadOnly(true);
    splitter->addWidget(m_details);
    splitter->setSizes({300, 170});
    ov->addWidget(splitter);
    m_ranges = new QLabel;
    m_ranges->setWordWrap(true); ov->addWidget(m_ranges);
    tabs->addTab(overview, "Drives");
    m_history = new QTableWidget(0, 3);
    m_history->setHorizontalHeaderLabels({"Time", "State", "Event"});
    m_history->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_history->verticalHeader()->hide();
    m_history->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_history->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_history->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_history->setWordWrap(true);
    tabs->addTab(m_history, "Alert history");
    auto *settings = new QWidget;
    auto *form = new QFormLayout(settings);
    m_startup = new QCheckBox("Start DriveWatch when I log in");
    QSettings entry(autostartPath(), QSettings::IniFormat);
    entry.beginGroup("Desktop Entry");
    m_startup->setChecked(!entry.value("Hidden", false).toBool());
    m_startup->setEnabled(!demo);
    form->addRow(m_startup);
    m_notifications = new QCheckBox("Show desktop notifications");
    m_notifications->setChecked(demo || m_settings.value("notifications", true).toBool());
    form->addRow(m_notifications);
    m_warm = new QSpinBox;
    m_hot = new QSpinBox;
    m_warm->setRange(30, 100); m_hot->setRange(31, 110);
    m_warm->setSuffix(" °C"); m_hot->setSuffix(" °C");
    m_warm->setValue(demo ? 65 : m_settings.value("warm", 65).toInt());
    m_hot->setValue(demo ? 75 : m_settings.value("hot", 75).toInt());
    if (m_hot->value() <= m_warm->value()) m_hot->setValue(m_warm->value() + 1);
    m_warm->setFixedWidth(115); m_hot->setFixedWidth(115);
    m_warmF = new QLabel; m_hotF = new QLabel;
    auto *warmRow = new QHBoxLayout; warmRow->addWidget(m_warm); warmRow->addWidget(m_warmF); warmRow->addStretch();
    auto *hotRow = new QHBoxLayout; hotRow->addWidget(m_hot); hotRow->addWidget(m_hotF); hotRow->addStretch();
    form->addRow("Temperature warning", warmRow);
    form->addRow("Critical temperature", hotRow);
    auto *explanation = new QLabel("Warnings clear after cooling by 5°C / 9°F. Device-reported failure flags always apply.\n"
        "Temperature limits vary by model; these configurable defaults are monitoring thresholds, not universal drive limits.\n"
        "Endurance: lower used is better; warning at 90%, critical at 100%. Spare: higher is better; device threshold applies.\n"
        "The system collector polls every minute; the tray checks for readings every 10 seconds.\n"
        "Repeated notifications for the same drive and issue are limited to one per 30 minutes.\n"
        "Acknowledging alerts clears unread history; active faults keep the warning icon visible.");
    explanation->setWordWrap(true); form->addRow(explanation);
    auto *test = new QPushButton("Send test notification");
    form->addRow(test);
    auto *forget = new QPushButton("Stop tracking disconnected drives");
    form->addRow(forget);
    tabs->addTab(settings, "Settings");
    tabs->setCurrentIndex(initialTab < 3 ? initialTab : 0);
    auto *buttons = new QHBoxLayout;
    auto *ack = new QPushButton("Acknowledge alerts");
    auto *refresh = new QPushButton("Reload readings");
    auto *hide = new QPushButton("Hide to tray");
    buttons->addWidget(ack); buttons->addWidget(refresh); buttons->addStretch(); buttons->addWidget(hide);
    layout->addLayout(buttons);
    setCentralWidget(central);
    connect(m_drives, &QTableWidget::itemSelectionChanged, this, &Window::details);
    connect(ack, &QPushButton::clicked, this, [this] { m_monitor.acknowledge(); save(); render(); });
    connect(refresh, &QPushButton::clicked, this, &Window::poll);
    connect(hide, &QPushButton::clicked, this, &QWidget::hide);
    connect(test, &QPushButton::clicked, this, [this] { notify("DriveWatch test", "DriveWatch notifications are working."); });
    connect(forget, &QPushButton::clicked, this, [this] { m_monitor.forgetDisconnected(); save(); render(); });
    connect(m_startup, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!setAutostart(enabled)) {
            m_startup->blockSignals(true); m_startup->setChecked(!enabled); m_startup->blockSignals(false);
            QMessageBox::warning(this, "Startup setting", "Could not save your login startup preference.");
        }
    });
    auto settingsChanged = [this] {
        if (!m_demo) {
            m_settings.setValue("warm", m_warm->value());
            m_settings.setValue("hot", m_hot->value());
            m_settings.setValue("notifications", m_notifications->isChecked());
            m_settings.sync();
        }
        poll();
    };
    connect(m_warm, &QSpinBox::valueChanged, this, [this, settingsChanged](int value) {
        if (m_hot->value() <= value) m_hot->setValue(value + 1);
        settingsChanged();
    });
    connect(m_hot, &QSpinBox::valueChanged, this, [this, settingsChanged](int value) {
        if (m_warm->value() >= value) m_warm->setValue(value - 1);
        settingsChanged();
    });
    connect(m_notifications, &QCheckBox::toggled, this, settingsChanged);
    auto *menu = new QMenu(this);
    m_popup = menu;
    m_mini = new QWidget;
    m_mini->setMinimumWidth(370);
    auto *miniLayout = new QVBoxLayout(m_mini);
    miniLayout->setContentsMargins(14, 12, 14, 10);
    auto *miniTitle = new QLabel("DriveWatch");
    auto miniFont = miniTitle->font(); miniFont.setBold(true); miniFont.setPointSize(14); miniTitle->setFont(miniFont);
    miniLayout->addWidget(miniTitle);
    m_miniSummary = new QLabel;
    m_miniSummary->setTextFormat(Qt::PlainText);
    m_miniSummary->setWordWrap(true);
    miniLayout->addWidget(m_miniSummary);
    m_miniCoverage = new QLabel;
    m_miniCoverage->setTextFormat(Qt::PlainText);
    m_miniCoverage->setWordWrap(true); m_miniCoverage->setMaximumWidth(360);
    miniLayout->addWidget(m_miniCoverage);
    if (m_demo) miniLayout->addWidget(new QLabel("PREVIEW · Synthetic readings"));
    m_miniGuide = new QLabel;
    m_miniGuide->setWordWrap(true);
    miniLayout->addWidget(m_miniGuide);
    auto *miniAction = new QWidgetAction(menu);
    miniAction->setDefaultWidget(m_mini);
    menu->addAction(miniAction);
    menu->addSeparator();
    menu->addAction("Open DriveWatch", this, &Window::Show);
    menu->addAction("Settings…", this, [this] { m_tabs->setCurrentIndex(2); Show(); });
    menu->addAction("Reload readings", this, &Window::poll);
    menu->addAction("Acknowledge alerts", this, [this] { m_monitor.acknowledge(); save(); render(); });
    menu->addSeparator();
    menu->addAction("Quit DriveWatch", qApp, &QApplication::quit);
    m_tray.setContextMenu(menu);
    m_tray.setIcon(statusIcon(false));
    m_tray.setToolTip("DriveWatch — waiting for readings");
    connect(&m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) showTrayPopup();
        if (reason == QSystemTrayIcon::DoubleClick) { m_popup->hide(); Show(); }
    });
    connect(&m_tray, &QSystemTrayIcon::messageClicked, this, &Window::Show);
    if (!demo) m_tray.show();
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &Window::poll);
    timer->start(10000);
    QTimer::singleShot(0, this, &Window::poll);
}

Policy Window::policy() const {
    Policy p; p.warm = m_warm->value(); p.hot = m_hot->value(); return p;
}

void Window::poll() {
    QStringList notifications;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (!m_stateError.isEmpty()) notifications = m_monitor.unavailable(m_stateError, now, policy());
    else if (m_demo) {
        auto sample = demoSnapshot();
        if (m_demoWarning) {
            // Seed a stable baseline before displaying a later synthetic change.
            if (m_monitor.drives().isEmpty()) {
                sample["collected_at"] = now - 120;
                m_monitor.update(sample, now - 120, policy());
                sample["collected_at"] = now;
            }
            auto drives = sample.value("drives").toArray();
            auto drive = drives[1].toObject();
            drive["counters"] = QJsonObject{{"media_errors", "1137"}};
            drive["temperature"] = 78;
            drives[1] = drive;
            sample["drives"] = drives;
        }
        m_monitor.update(sample, now, policy());
    }
    else {
        QFile f("/run/drivewatch/snapshot.json");
        if (!f.open(QIODevice::ReadOnly))
            notifications = m_monitor.unavailable("Collector unavailable. Check drivewatch-collector.timer and service.", now, policy());
        else {
            QJsonParseError error;
            auto doc = QJsonDocument::fromJson(f.readAll(), &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject())
                notifications = m_monitor.unavailable("Collector data could not be decoded.", now, policy());
            else notifications = m_monitor.update(doc.object(), now, policy());
        }
    }
    save(); render();
    if (!notifications.isEmpty() && m_notifications->isChecked()) {
        QString text = notifications.mid(0, 4).join('\n');
        if (notifications.size() > 4) text += QString("\nAnd %1 more. Open DriveWatch for details.").arg(notifications.size() - 4);
        bool critical = false;
        for (const auto v : m_monitor.drives())
            for (const auto c : v.toObject().value("active").toObject())
                critical |= c.toObject().value("level").toString() == "critical";
        notify("DriveWatch — drive health alert", text, critical);
    }
}

void Window::save() {
    if (m_demo || !m_stateError.isEmpty()) return;
    QDir().mkpath(QFileInfo(m_statePath).absolutePath());
    QSaveFile file(m_statePath);
    const QByteArray data = QJsonDocument(m_monitor.state()).toJson();
    m_saveFailed = !file.open(QIODevice::WriteOnly);
    if (!m_saveFailed) {
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        m_saveFailed = file.write(data) != data.size() || !file.commit();
    }
}

void Window::render() {
    const bool warn = m_monitor.warning() || m_saveFailed;
    m_status->setText(warn ? QString("Attention needed · %1 unread alerts").arg(m_monitor.unreadCount())
                          : "No new health concerns");
    QString subtitle = m_demo ? "PREVIEW · Synthetic readings. No real monitoring state or settings are changed."
                              : m_monitor.coverage();
    const auto stamp = m_monitor.state().value("collected_at").toInteger();
    if (stamp) subtitle += "  Last collection: " + QDateTime::fromSecsSinceEpoch(stamp).toString("MMM d, HH:mm:ss");
    if (m_saveFailed) subtitle += "  WARNING: Baseline and alert history could not be saved.";
    m_subtitle->setText(subtitle);
    m_warmF->setText(QString("/ %1°F").arg(qRound(policy().warm * 9.0 / 5.0 + 32)));
    m_hotF->setText(QString("/ %1°F").arg(qRound(policy().hot * 9.0 / 5.0 + 32)));
    m_ranges->setText(QString("Temperature guide · Below %1: below app warning · %1: warning · %2: critical.\n"
        "Drive limits vary by model. Endurance used: warning at 90%, critical at 100%. Old error counts are baselined.")
        .arg(temperatureText(policy().warm), temperatureText(policy().hot)));
    m_tray.setIcon(statusIcon(warn));
    m_tray.setToolTip("DriveWatch — " + m_status->text() + '\n' + m_monitor.coverage());
    QString selected;
    if (m_drives->currentRow() >= 0 && m_drives->item(m_drives->currentRow(), 0))
        selected = m_drives->item(m_drives->currentRow(), 0)->data(Qt::UserRole).toString();
    m_drives->blockSignals(true);
    auto drives = m_monitor.drives();
    m_drives->setRowCount(drives.size());
    int selectedRow = 0;
    for (int i = 0; i < drives.size(); ++i) {
        auto r = drives[i].toObject(); auto d = r.value("sample").toObject();
        const auto active = r.value("active").toObject();
        bool critical = false;
        for (const auto c : active) critical |= c.toObject().value("level").toString() == "critical";
        QString status = !r.value("connected").toBool() ? "Disconnected" : (critical ? "Critical" : (!active.isEmpty() ? "Warning" : "Stable"));
        bool unread = false;
        for (const auto e : m_monitor.events())
            if (e.toObject().value("drive_id") == r.value("id") && !e.toObject().value("acknowledged").toBool()) unread = true;
        if (unread && active.isEmpty() && r.value("connected").toBool()) status = "New change";
        if (selected.isEmpty() && !active.isEmpty()) selectedRow = i;
        QStringList texts{d.value("model").toString(), d.value("device").toString(), d.contains("temperature") ? temperatureText(d.value("temperature").toDouble()) : "—",
                          field(d, "percentage_used", "%"), field(d, "available_spare", "%"), status};
        for (int col = 0; col < texts.size(); ++col) {
            auto *item = new QTableWidgetItem(texts[col]);
            if (col == 0) item->setData(Qt::UserRole, r.value("id").toString());
            if (col == 5) item->setIcon(statusIcon(!active.isEmpty() || unread));
            if (col == 2 && d.contains("temperature")) {
                item->setForeground(statColor(temperatureRisk(d.value("temperature").toDouble(), policy())));
                item->setToolTip(QString("Configured warning: %1\nConfigured critical: %2\nGreen → amber → red as temperature approaches the limits.\nDevice limits vary by model.")
                                 .arg(temperatureText(policy().warm), temperatureText(policy().hot)));
            }
            if (col == 3 && d.contains("percentage_used")) {
                item->setForeground(statColor(std::max(0.0, (d.value("percentage_used").toDouble() - 70) / 30.0)));
                item->setToolTip("Manufacturer's estimate of endurance used, not remaining years.\nLower is better. Warning at 90%; critical at 100%.");
            }
            if (col == 4 && d.contains("available_spare")) {
                const double threshold = d.value("spare_threshold").toDouble(10);
                item->setForeground(statColor((100 - d.value("available_spare").toDouble()) / std::max(1.0, 100 - threshold)));
                item->setToolTip(QString("Available reserve capacity; higher is better.\nDevice-reported warning threshold: %1%.\nA new decrease also triggers a change alert.").arg(threshold));
            }
            if (col == 5) item->setForeground(statColor(critical ? 1.0 : ((!active.isEmpty() || unread) ? 0.5 : 0.0)));
            if (d.contains("error") && col == 2) { item->setText("Unavailable"); item->setToolTip("Last successful temperature is shown in drive details."); }
            m_drives->setItem(i, col, item);
        }
        if (r.value("id").toString() == selected) selectedRow = i;
    }
    if (!drives.isEmpty()) m_drives->selectRow(selectedRow);
    m_drives->blockSignals(false);
    details();
    auto log = m_monitor.events();
    m_history->setRowCount(log.size());
    for (int row = 0; row < log.size(); ++row) {
        auto event = log[log.size() - row - 1].toObject();
        QStringList texts{QDateTime::fromSecsSinceEpoch(event.value("at").toInteger()).toString("MMM d HH:mm:ss"),
            event.value("acknowledged").toBool() ? "Acknowledged" : "Unread", event.value("message").toString()};
        for (int col = 0; col < 3; ++col) m_history->setItem(row, col, new QTableWidgetItem(texts[col]));
    }
    m_history->resizeRowsToContents();
    renderMini();
}

void Window::renderMini() {
    auto *layout = qobject_cast<QVBoxLayout *>(m_mini->layout());
    // A live QWidgetAction must keep its child widgets across polling updates.
    // Replacing the entire tree while QMenu is open can leave the hosted panel blank.
    m_miniSummary->setText(m_status->text());
    QSet<QString> seen;
    int position = 2;
    for (const auto v : m_monitor.drives()) {
        auto r = v.toObject(); auto d = r.value("sample").toObject();
        const auto id = r.value("id").toString();
        seen.insert(id);
        if (!m_miniRows.contains(id)) {
            MiniDriveRow row;
            row.widget = new QWidget(m_mini);
            auto *line = new QHBoxLayout(row.widget);
            line->setContentsMargins(0, 6, 0, 6); line->setSpacing(10);
            row.icon = new QLabel;
            line->addWidget(row.icon);
            auto *labels = new QWidget;
            auto *texts = new QVBoxLayout(labels); texts->setContentsMargins(0, 0, 0, 0); texts->setSpacing(2);
            row.model = new QLabel;
            row.model->setTextFormat(Qt::PlainText);
            auto modelFont = row.model->font(); modelFont.setBold(true); row.model->setFont(modelFont);
            texts->addWidget(row.model);
            row.detail = new QLabel;
            row.detail->setTextFormat(Qt::PlainText); texts->addWidget(row.detail);
            line->addWidget(labels, 1);
            row.temperature = new QLabel;
            auto tempFont = row.temperature->font(); tempFont.setBold(true); tempFont.setPointSize(12); row.temperature->setFont(tempFont);
            line->addWidget(row.temperature);
            layout->insertWidget(position, row.widget);
            row.widget->show();
            m_miniRows.insert(id, row);
        }
        ++position;
        auto &row = m_miniRows[id];
        bool unread = false;
        for (const auto e : m_monitor.events())
            if (e.toObject().value("drive_id") == r.value("id") && !e.toObject().value("acknowledged").toBool()) unread = true;
        const auto conditions = r.value("active").toObject();
        bool active = !conditions.isEmpty();
        bool critical = false;
        for (const auto c : conditions) critical |= c.toObject().value("level").toString() == "critical";
        const bool disconnected = !r.value("connected").toBool();
        row.icon->setPixmap(statusIcon(active || unread).pixmap(24, 24));
        row.model->setText(d.value("model").toString());
        QString state = disconnected ? "Disconnected" : (critical ? "Critical warning" : (active ? "Active warning" : (unread ? "New health change" : "Stable")));
        row.detail->setText(d.value("device").toString() + " · " + state);
        colorLabel(row.detail, critical ? 1.0 : ((active || unread) ? 0.5 : 0.0));
        row.temperature->setText(d.contains("error") || disconnected || !d.contains("temperature") ? "—" : temperatureText(d.value("temperature").toDouble()));
        row.temperature->setPalette(qApp->palette());
        if (!d.contains("error") && !disconnected && d.contains("temperature"))
            colorLabel(row.temperature, temperatureRisk(d.value("temperature").toDouble(), policy()));
    }
    for (auto it = m_miniRows.begin(); it != m_miniRows.end();) {
        if (seen.contains(it.key())) { ++it; continue; }
        it->widget->hide();
        layout->removeWidget(it->widget);
        it->widget->deleteLater();
        it = m_miniRows.erase(it);
    }
    m_miniCoverage->setText(m_subtitle->text());
    m_miniCoverage->setVisible(m_monitor.drives().isEmpty() || !m_monitor.state().value("global").toObject().isEmpty() || m_saveFailed);
    m_miniGuide->setText(QString("Warning %1 · Critical %2").arg(temperatureText(policy().warm), temperatureText(policy().hot)));
    m_mini->updateGeometry();
    m_mini->update();
    if (!m_popup->isVisible()) m_mini->adjustSize();
}

QWidget *Window::trayPopup() const { return m_popup; }

void Window::showTrayPopup() {
    if (m_popup->isVisible()) { m_popup->hide(); return; }
    renderMini();
    const auto geometry = m_tray.geometry();
    m_popup->popup(geometry.isValid() ? geometry.center() : QCursor::pos());
}

void Window::details() {
    const int row = m_drives->currentRow();
    if (row < 0 || !m_drives->item(row, 0)) { m_details->setPlainText("Waiting for drive readings."); return; }
    const auto id = m_drives->item(row, 0)->data(Qt::UserRole).toString();
    for (const auto v : m_monitor.drives()) {
        auto r = v.toObject(); if (r.value("id").toString() != id) continue;
        auto d = r.value("sample").toObject();
        QStringList lines{d.value("model").toString() + " · " + d.value("device").toString(),
                          "Serial: " + field(d, "serial") + "   Protocol: " + field(d, "protocol"),
                          "Power-on hours: " + field(d, "power_on_hours"),
                          "Temperature: " + (d.contains("temperature") ? temperatureText(d.value("temperature").toDouble()) : "Unavailable")};
        const auto active = r.value("active").toObject();
        for (auto it = active.begin(); it != active.end(); ++it) lines << "WARNING: " + it.value().toObject().value("message").toString();
        if (active.isEmpty()) lines << "No active warning conditions. Historical counts below are retained as the comparison baseline.";
        auto counters = d.value("counters").toObject();
        for (auto it = counters.begin(); it != counters.end(); ++it)
            lines << QString(it.key()).replace('_', ' ') + ": " + it.value().toString();
        for (const auto note : d.value("notes").toArray()) lines << note.toString();
        m_details->setPlainText(lines.join('\n'));
        return;
    }
}

bool Window::setAutostart(bool enabled) {
    if (m_demo) return false;
    const auto path = autostartPath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    QByteArray contents = "[Desktop Entry]\nType=Application\nName=DriveWatch\nExec=drivewatch\nIcon=drivewatch\nTerminal=false\nHidden=";
    contents += enabled ? "false\n" : "true\n";
    contents += "X-GNOME-Autostart-enabled=";
    contents += enabled ? "true\n" : "false\n";
    return f.write(contents) == contents.size() && f.commit();
}

void Window::notify(const QString &title, const QString &text, bool critical) {
    if (m_demo) return;
    QDBusInterface service("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                           "org.freedesktop.Notifications", QDBusConnection::sessionBus());
    QVariantMap hints;
    hints["desktop-entry"] = "io.github.drivewatch";
    hints["urgency"] = QVariant::fromValue(static_cast<uchar>(critical ? 2 : 1));
    hints["category"] = "device.error";
    if (service.isValid()) {
        service.asyncCall("Notify", "DriveWatch", uint(0), "drivewatch", title, text, QStringList{}, hints, int(-1));
    } else {
        m_tray.showMessage(title, text, critical ? QSystemTrayIcon::Critical : QSystemTrayIcon::Warning);
    }
}

void Window::Show() { showNormal(); raise(); activateWindow(); }
void Window::closeEvent(QCloseEvent *e) { hide(); e->ignore(); }
