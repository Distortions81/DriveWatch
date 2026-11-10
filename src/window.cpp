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
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStyle>
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
QString driveName(const QJsonObject &d) {
    QStringList labels;
    auto add = [&labels](const QJsonObject &v) {
        const QString label = v.value("label").toString().trimmed();
        if (!label.isEmpty() && label.compare("EFI", Qt::CaseInsensitive) != 0 &&
            label.compare("EFI System Partition", Qt::CaseInsensitive) != 0 &&
            label.compare("System Reserved", Qt::CaseInsensitive) != 0) labels << label;
    };
    add(d);
    for (const auto p : d.value("partitions").toArray()) add(p.toObject());
    labels.removeDuplicates();
    return labels.isEmpty() ? d.value("model").toString() : labels.join(" · ");
}
QString capacity(const QJsonObject &d) {
    if (!d.value("size_bytes").isDouble()) return "Unavailable";
    const double bytes = d.value("size_bytes").toDouble();
    return bytes >= 1e12 ? QString("%1 TB").arg(bytes / 1e12, 0, 'f', 1)
                        : QString("%1 GB").arg(bytes / 1e9, 0, 'f', 0);
}
QString connectionType(const QJsonObject &d) {
    const auto transport = d.value("transport").toString().toLower();
    const QMap<QString, QString> names{{"nvme", "NVMe"}, {"sata", "SATA"}, {"ata", "ATA"},
        {"sas", "SAS"}, {"scsi", "SCSI"}, {"usb", "USB"}, {"mmc", "MMC"}, {"sd", "SD"},
        {"spi", "SPI"}, {"sbp", "FireWire"}, {"firewire", "FireWire"}, {"fc", "Fibre Channel"}, {"iscsi", "iSCSI"}};
    QString type = names.value(transport, transport.toUpper());
    if (type.isEmpty()) {
        type = d.value("protocol").toString();
        if (type.isEmpty() || type == "Unavailable") type = "Unknown";
    }
    if (transport == "iscsi" || transport == "fc") return "Network: " + type;
    return (d.value("external").toBool() || transport == "usb" || type == "FireWire" ? "External: " : "Internal: ") + type;
}
QString driveStatus(const QJsonObject &r, bool critical, bool unread) {
    const auto d = r.value("sample").toObject();
    if (!r.value("monitored").toBool(true)) return "Monitoring off";
    if (!r.value("connected").toBool()) return "Disconnected";
    if (critical) return "Critical";
    if (!r.value("active").toObject().isEmpty()) return "Warning";
    if (unread) return "New change";
    if (d.value("smart_support") == "unsupported") return "SMART unavailable";
    if (d.contains("error")) return "SMART unavailable";
    return "Stable";
}
QString relativeUpdate(qint64 stamp, qint64 now) {
    const qint64 age = std::max(qint64(0), now - stamp);
    if (age < 60) return "Updated just now";
    if (age < 3600) {
        const auto minutes = age / 60;
        return minutes == 1 ? "Updated 1 minute ago" : QString("Updated %1 minutes ago").arg(minutes);
    }
    if (age < 86400) {
        const auto hours = age / 3600;
        return hours == 1 ? "Updated 1 hour ago" : QString("Updated %1 hours ago").arg(hours);
    }
    const auto days = age / 86400;
    return days == 1 ? "Updated 1 day ago" : QString("Updated %1 days ago").arg(days);
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
            {"label", QStringList{"Library", "System", "Backup SSD", "Backup SSD", "Projects"}[i]},
            {"size_bytes", i == 0 ? 1e12 : 2e12}, {"serial", QString("DEMO-%1").arg(i)}, {"protocol", i == 0 ? "ATA" : "NVMe"}, {"transport", i == 0 ? "sata" : "nvme"},
            {"connection_speed", i == 0 ? "6.0 Gb/s" : "PCIe 8.0 GT/s ×4"},
            {"temperature", temps[i]}, {"passed", true}, {"available_spare", i == 1 ? 92 : 100},
            {"percentage_used", i == 4 ? 21 : 3},
            {"counters", QJsonObject{{"media_errors", i == 1 ? "1135" : "0"}}}});
    }
    for (int i = 0; i < 2; ++i) {
        drives.append(QJsonObject{{"id", QString("demo-usb-%1").arg(i)},
            {"size_bytes", i == 0 ? 8e12 : 6e12}, {"model", i == 0 ? "Seagate Expansion SW" : "Seagate Expansion HDD"},
            {"device", i == 0 ? "/dev/sdb" : "/dev/sdc"}, {"external", true}, {"transport", "usb"}, {"connection_speed", "5 Gb/s"},
            {"smart_support", i == 0 ? "unavailable" : "unsupported"},
            {"error", i == 0 ? "USB adapter did not provide SMART access." : "Device reports no SMART support."},
            {"partitions", QJsonArray{QJsonObject{{"label", i == 0 ? "Backups" : "Archive"},
                {"mountpoints", QJsonArray{i == 0 ? "/media/Backups" : "/media/Archive"}}}}}});
    }
    return {{"version", 1}, {"collected_at", QDateTime::currentSecsSinceEpoch()}, {"drives", drives}};
}
QString autostartPath() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart/com.m45software.DriveWatcher.desktop";
}
}

Window::Window(bool demo, bool demoWarning, int initialTab)
    : m_demo(demo), m_demoWarning(demoWarning), m_settings(demo ? "M45SoftwarePreview" : "M45Software", "m45-drive-watcher") {
    setWindowTitle(demo ? "M45 Drive Watcher — Preview" : "M45 Drive Watcher");
    setWindowIcon(statusIcon(false));
    resize(960, 860);
    m_statePath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/state.json";
    if (!demo) {
        // Copy legacy user data once, without overwriting a new installation's data.
        const auto legacyState = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                               + "/DriveWatch/DriveWatch/state.json";
        if (!QFile::exists(m_statePath) && QFile::exists(legacyState)) {
            if (!QDir().mkpath(QFileInfo(m_statePath).absolutePath()) || !QFile::copy(legacyState, m_statePath))
                m_stateError = "Cannot migrate the DriveWatch baseline. Fix access and restart M45 Drive Watcher.";
        }
        QSettings legacySettings("DriveWatch", "DriveWatch");
        if (!QFile::exists(m_settings.fileName()) && QFile::exists(legacySettings.fileName())) {
            for (const auto &key : legacySettings.allKeys()) m_settings.setValue(key, legacySettings.value(key));
            m_settings.sync();
        }
        const auto legacyAutostart = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
                                   + "/autostart/io.github.drivewatch.desktop";
        if (!QFile::exists(autostartPath()) && QFile::exists(legacyAutostart)) {
            QSettings entry(legacyAutostart, QSettings::IniFormat);
            entry.beginGroup("Desktop Entry");
            setAutostart(!entry.value("Hidden", false).toBool());
        }
        QFile f(m_statePath);
        if (f.exists()) {
            if (!f.open(QIODevice::ReadOnly)) m_stateError = "Cannot read saved baseline. Fix access and restart M45 Drive Watcher.";
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
    auto *title = new QLabel("M45 Drive Watcher");
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
    splitter->setChildrenCollapsible(false);
    m_drives = new QTableWidget(0, 5);
    m_drives->setObjectName("driveTable");
    m_drives->setHorizontalHeaderLabels({"Label", "Drive", "Status", "Temperature", "Connection"});
    m_drives->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_drives->setSelectionMode(QAbstractItemView::SingleSelection);
    m_drives->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_drives->verticalHeader()->hide();
    m_drives->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_drives->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_drives->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_drives->setAlternatingRowColors(true);
    m_drives->setStyleSheet("QTableWidget::item { padding-left: 8px; padding-right: 8px; }");
    m_drives->setShowGrid(false);
    m_drives->setWordWrap(false);
    m_drives->verticalHeader()->setDefaultSectionSize(42);
    m_drives->setMinimumHeight(210);
    splitter->addWidget(m_drives);
    auto *driveDetails = new QTabWidget;
    driveDetails->setObjectName("driveDetails");
    auto *summary = new QWidget;
    auto *summaryLayout = new QVBoxLayout(summary);
    summaryLayout->setContentsMargins(16, 14, 16, 14);
    m_detailName = new QLabel;
    m_detailName->setTextFormat(Qt::PlainText); m_detailName->setWordWrap(true);
    auto detailFont = m_detailName->font(); detailFont.setPointSize(17); detailFont.setBold(true);
    m_detailName->setFont(detailFont);
    summaryLayout->addWidget(m_detailName);
    m_detailModel = new QLabel; m_detailModel->setTextFormat(Qt::PlainText); m_detailModel->setWordWrap(true);
    summaryLayout->addWidget(m_detailModel);
    auto *metrics = new QGridLayout;
    metrics->setHorizontalSpacing(30); metrics->setVerticalSpacing(18);
    const QStringList headings{"Status", "Temperature", "Monitoring", "Capacity", "Connection", "Connection speed"};
    for (int i = 0; i < headings.size(); ++i) {
        auto *cell = new QWidget;
        auto *cellLayout = new QVBoxLayout(cell); cellLayout->setContentsMargins(0, 0, 0, 0); cellLayout->setSpacing(4);
        auto *caption = new QLabel(headings[i]);
        auto captionFont = caption->font(); captionFont.setPointSizeF(captionFont.pointSizeF() * 0.9); caption->setFont(captionFont);
        cellLayout->addWidget(caption);
        auto *value = new QLabel;
        value->setObjectName("summary" + QString(headings[i]).remove(' '));
        value->setTextFormat(Qt::PlainText); value->setWordWrap(true);
        auto valueFont = value->font(); valueFont.setBold(true); value->setFont(valueFont);
        cellLayout->addWidget(value);
        m_summaryFields[headings[i]] = value;
        metrics->addWidget(cell, i / 3, i % 3);
        metrics->setColumnStretch(i % 3, 1);
    }
    summaryLayout->addSpacing(10);
    summaryLayout->addLayout(metrics);
    m_detailMessage = new QLabel;
    m_detailMessage->setObjectName("driveSummaryMessage");
    m_detailMessage->setTextFormat(Qt::PlainText); m_detailMessage->setWordWrap(true);
    summaryLayout->addSpacing(8); summaryLayout->addWidget(m_detailMessage); summaryLayout->addStretch();
    driveDetails->addTab(summary, "Summary");
    auto *technicalScroll = new QScrollArea;
    technicalScroll->setWidgetResizable(true); technicalScroll->setFrameShape(QFrame::NoFrame);
    auto *technical = new QWidget;
    auto *technicalLayout = new QVBoxLayout(technical);
    technicalLayout->setContentsMargins(16, 14, 16, 14);
    auto *identity = new QFormLayout;
    for (const auto &heading : {"Device", "Serial number", "Protocol", "Power-on hours", "Endurance used", "Available spare"}) {
        auto *value = new QLabel;
        value->setTextFormat(Qt::PlainText); value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        identity->addRow(heading, value); m_technicalFields[heading] = value;
    }
    technicalLayout->addLayout(identity);
    auto addTable = [technicalLayout](const QString &title, const QStringList &columns) {
        auto *group = new QGroupBox(title);
        auto *layout = new QVBoxLayout(group);
        auto *table = new QTableWidget(0, columns.size());
        table->setHorizontalHeaderLabels(columns); table->verticalHeader()->hide(); table->setShowGrid(false);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table->setMinimumHeight(110); table->setMaximumHeight(180);
        layout->addWidget(table); technicalLayout->addWidget(group);
        return table;
    };
    m_volumes = addTable("Volumes", {"Label", "Mount location", "Device"}); m_volumes->setObjectName("volumeTable");
    m_counters = addTable("Health counters", {"Reading", "Value"}); m_counters->setObjectName("counterTable");
    m_diagnosticsToggle = new QPushButton("Show diagnostics"); m_diagnosticsToggle->setCheckable(true);
    m_diagnosticsToggle->setObjectName("diagnosticsToggle"); technicalLayout->addWidget(m_diagnosticsToggle, 0, Qt::AlignLeft);
    m_details = new QTextEdit;
    m_details->setReadOnly(true); m_details->setMinimumHeight(180); m_details->hide();
    m_details->setObjectName("diagnosticOutput"); technicalLayout->addWidget(m_details);
    connect(m_diagnosticsToggle, &QPushButton::toggled, this, [this](bool visible) {
        m_details->setVisible(visible); m_diagnosticsToggle->setText(visible ? "Hide diagnostics" : "Show diagnostics");
    });
    technicalScroll->setWidget(technical); driveDetails->addTab(technicalScroll, "Technical details");
    if (initialTab == 4) driveDetails->setCurrentIndex(1);
    splitter->addWidget(driveDetails);
    splitter->setSizes({380, 230}); splitter->setStretchFactor(0, 3); splitter->setStretchFactor(1, 2);
    auto *selectionHelp = new QLabel("Check the drives you want to monitor. Select a drive for details.");
    ov->addWidget(selectionHelp); ov->addWidget(splitter);
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
    m_startup = new QCheckBox("Start M45 Drive Watcher when I log in");
    QSettings entry(autostartPath(), QSettings::IniFormat);
    entry.beginGroup("Desktop Entry");
    m_startup->setChecked(!entry.value("Hidden", false).toBool());
    m_startup->setEnabled(!demo);
    form->addRow(m_startup);
    auto *loginWindow = new QCheckBox("Open window at login");
    loginWindow->setObjectName("showWindowAtLogin");
    loginWindow->setChecked(m_settings.value("showWindowAtLogin", false).toBool());
    loginWindow->setEnabled(!demo && m_startup->isChecked());
    form->addRow(loginWindow);
    connect(m_startup, &QCheckBox::toggled, loginWindow, [this, loginWindow](bool on) { loginWindow->setEnabled(!m_demo && on); });
    connect(loginWindow, &QCheckBox::toggled, this, [this](bool on) { if (!m_demo) m_settings.setValue("showWindowAtLogin", on); });
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
    connect(m_drives, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (item->column() != 0) return;
        m_monitor.setMonitored(item->data(Qt::UserRole).toString(), item->checkState() == Qt::Checked);
        save();
        // Render after the signal completes so the changed item stays alive.
        QTimer::singleShot(0, this, &Window::poll);
    });
    connect(ack, &QPushButton::clicked, this, [this] { m_monitor.acknowledge(); save(); render(); });
    connect(refresh, &QPushButton::clicked, this, &Window::poll);
    connect(hide, &QPushButton::clicked, this, &QWidget::hide);
    connect(test, &QPushButton::clicked, this, [this] { notify("M45 Drive Watcher test", "M45 Drive Watcher notifications are working."); });
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
    auto *miniTitle = new QLabel("M45 Drive Watcher");
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
    auto *miniAction = new QWidgetAction(menu);
    m_miniScroll = new QScrollArea;
    m_miniScroll->setFrameShape(QFrame::NoFrame);
    m_miniScroll->setWidgetResizable(true);
    m_miniScroll->setFixedWidth(540);
    m_miniScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_miniScroll->setWidget(m_mini);
    miniAction->setDefaultWidget(m_miniScroll);
    menu->addAction(miniAction);
    menu->addSeparator();
    menu->addAction("Open M45 Drive Watcher", this, &Window::Show);
    menu->addAction("Settings…", this, [this] { m_tabs->setCurrentIndex(2); Show(); });
    menu->addAction("Reload readings", this, &Window::poll);
    menu->addAction("Acknowledge alerts", this, [this] { m_monitor.acknowledge(); save(); render(); });
    menu->addSeparator();
    menu->addAction("Quit M45 Drive Watcher", qApp, &QApplication::quit);
    m_tray.setContextMenu(menu);
    m_tray.setIcon(statusIcon(false));
    m_tray.setToolTip("M45 Drive Watcher — waiting for readings");
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
        QFile f("/run/m45-drive-watcher/snapshot.json");
        if (!f.open(QIODevice::ReadOnly))
            notifications = m_monitor.unavailable("Collector unavailable. Check m45-drive-watcher-collector.timer and service.", now, policy());
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
        if (notifications.size() > 4) text += QString("\nAnd %1 more. Open M45 Drive Watcher for details.").arg(notifications.size() - 4);
        bool critical = false;
        for (const auto v : m_monitor.drives())
            for (const auto c : v.toObject().value("active").toObject())
                critical |= c.toObject().value("level").toString() == "critical";
        notify("M45 Drive Watcher — drive health alert", text, critical);
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
    const QString quietStatus = m_monitor.coverage() == "No drives selected for monitoring." ? "Monitoring paused"
        : m_monitor.coverage().contains("unsupported") ? "Some health readings are unavailable" : "No new health concerns";
    m_status->setText(warn ? (m_monitor.unreadCount() > 0
        ? QString("Attention needed · %1 unread alerts").arg(m_monitor.unreadCount()) : "Attention needed") : quietStatus);
    const auto readings = m_monitor.drives();
    int monitoredCount = 0;
    for (const auto v : readings) monitoredCount += v.toObject().value("monitored").toBool(true);
    QString subtitle = m_demo ? "Preview · Sample drives" : QString("%1 of %2 drives monitored").arg(monitoredCount).arg(readings.size());
    if (!m_monitor.state().value("global").toObject().isEmpty()) subtitle += " · " + m_monitor.coverage();
    const auto stamp = m_monitor.state().value("collected_at").toInteger();
    if (stamp) subtitle += " · " + relativeUpdate(stamp, QDateTime::currentSecsSinceEpoch());
    m_subtitle->setToolTip(stamp ? QDateTime::fromSecsSinceEpoch(stamp).toString("yyyy-MM-dd HH:mm:ss t") : QString());
    if (m_saveFailed) subtitle += "  WARNING: Baseline and alert history could not be saved.";
    m_subtitle->setText(subtitle);
    m_warmF->setText(QString("/ %1°F").arg(qRound(policy().warm * 9.0 / 5.0 + 32)));
    m_hotF->setText(QString("/ %1°F").arg(qRound(policy().hot * 9.0 / 5.0 + 32)));
    m_tray.setIcon(statusIcon(warn));
    m_tray.setToolTip("M45 Drive Watcher — " + m_status->text() + '\n' + m_monitor.coverage());
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
        const bool monitored = r.value("monitored").toBool(true);
        bool unread = false;
        for (const auto e : m_monitor.events())
            if (e.toObject().value("drive_id") == r.value("id") && !e.toObject().value("acknowledged").toBool()) unread = true;
        QString status = driveStatus(r, critical, unread);
        if (selected.isEmpty() && !active.isEmpty()) selectedRow = i;
        const QString friendly = driveName(d);
        const QString model = d.value("model").toString();
        QStringList texts{friendly == model ? "Unlabeled" : friendly, model, status,
            d.contains("temperature") && !d.contains("error") ? temperatureText(d.value("temperature").toDouble()) : "—",
            d.value("external").toBool() ? "External USB" : "Internal"};
        for (int col = 0; col < texts.size(); ++col) {
            auto *item = new QTableWidgetItem(texts[col]);
            item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            if (col == 0) {
                item->setData(Qt::UserRole, r.value("id").toString());
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(monitored ? Qt::Checked : Qt::Unchecked);
                item->setToolTip("Monitor this drive. Turning monitoring off acknowledges its existing alerts; history is retained.");
            }
            if (col == 1) item->setToolTip(model + " · " + capacity(d));
            if (col == 2) {
                if (d.contains("error")) item->setToolTip("SMART unavailable through the current connection.");
                if (monitored) item->setIcon(d.contains("error") && active.isEmpty() && !unread
                    ? style()->standardIcon(QStyle::SP_MessageBoxInformation) : statusIcon(!active.isEmpty() || unread));
                if (critical || !active.isEmpty() || unread) item->setForeground(statColor(critical ? 1.0 : 0.5));
            }
            if (col == 3 && d.contains("temperature") && !d.contains("error")) {
                item->setForeground(statColor(temperatureRisk(d.value("temperature").toDouble(), policy())));
                item->setToolTip(QString("Warning: %1\nCritical: %2").arg(temperatureText(policy().warm), temperatureText(policy().hot)));
            }
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
        if (!r.value("monitored").toBool(true)) continue;
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
            row.detail->setTextFormat(Qt::PlainText);
            row.detail->setWordWrap(true); row.detail->setMaximumWidth(300);
            texts->addWidget(row.detail);
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
        row.icon->setPixmap((d.contains("error") && !active && !unread
            ? style()->standardIcon(QStyle::SP_MessageBoxInformation) : statusIcon(active || unread)).pixmap(24, 24));
        row.model->setText(driveName(d));
        row.model->setWordWrap(true);
        row.model->setMaximumWidth(320);
        QString state = driveStatus(r, critical, unread);
        row.detail->setText(state + (d.value("external").toBool() ? " · External USB" : ""));
        row.model->setToolTip(d.value("model").toString());
        row.detail->setPalette(qApp->palette());
        if (!d.contains("error") || active || unread) colorLabel(row.detail, critical ? 1.0 : ((active || unread) ? 0.5 : 0.0));
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
    m_miniCoverage->setToolTip(m_subtitle->toolTip());
    m_miniCoverage->setVisible(m_monitor.drives().isEmpty() || !m_monitor.state().value("global").toObject().isEmpty() || m_saveFailed);
    m_mini->updateGeometry();
    m_mini->update();
    m_miniScroll->setFixedHeight(std::min(480, m_mini->sizeHint().height() + 4));
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
    if (row < 0 || !m_drives->item(row, 0)) {
        m_detailName->setText("Select a drive"); m_detailModel->clear(); m_detailMessage->clear();
        for (auto *value : m_summaryFields) value->setText("—");
        for (auto *value : m_technicalFields) value->setText("—");
        m_volumes->setRowCount(0); m_counters->setRowCount(0); m_details->clear(); return;
    }
    const auto id = m_drives->item(row, 0)->data(Qt::UserRole).toString();
    for (const auto v : m_monitor.drives()) {
        const auto r = v.toObject(); if (r.value("id").toString() != id) continue;
        const auto d = r.value("sample").toObject();
        const auto active = r.value("active").toObject();
        bool critical = false, unread = false;
        for (const auto c : active) critical |= c.toObject().value("level").toString() == "critical";
        for (const auto e : m_monitor.events())
            unread |= e.toObject().value("drive_id") == r.value("id") && !e.toObject().value("acknowledged").toBool();
        m_detailName->setText(driveName(d));
        m_detailModel->setText(driveName(d) == d.value("model").toString() ? "" : d.value("model").toString());
        m_summaryFields["Status"]->setText(driveStatus(r, critical, unread));
        m_summaryFields["Temperature"]->setText(!d.contains("error") && d.contains("temperature")
            ? temperatureText(d.value("temperature").toDouble()) : "Unavailable");
        m_summaryFields["Monitoring"]->setText(r.value("monitored").toBool(true) ? "On" : "Off");
        m_summaryFields["Capacity"]->setText(capacity(d));
        m_summaryFields["Connection"]->setText(connectionType(d));
        m_summaryFields["Connection speed"]->setText(r.value("connected").toBool()
            ? d.value("connection_speed").toString("Unavailable") : "Unavailable");
        m_summaryFields["Connection speed"]->setToolTip("Negotiated connection speed, not measured file-transfer speed.");
        QStringList message;
        if (!r.value("connected").toBool()) message << "This drive is disconnected.";
        else if (d.contains("error")) message << (d.value("external").toBool()
            ? "SMART monitoring is unavailable through this USB connection. Drive health cannot be assessed."
            : "Health readings are unavailable. See Technical details for the last readings and diagnostics.");
        else if (active.isEmpty()) message << (r.value("monitored").toBool(true)
            ? "No new health concerns detected." : "Monitoring is off for this drive.");
        for (const auto c : active) message << c.toObject().value("message").toString();
        m_detailMessage->setText(message.join("\n"));
        m_technicalFields["Device"]->setText(field(d, "device"));
        m_technicalFields["Serial number"]->setText(field(d, "serial"));
        m_technicalFields["Protocol"]->setText(field(d, "protocol"));
        m_technicalFields["Power-on hours"]->setText(field(d, "power_on_hours"));
        m_technicalFields["Endurance used"]->setText(field(d, "percentage_used", "%"));
        m_technicalFields["Available spare"]->setText(field(d, "available_spare", "%"));
        QJsonArray partitions = d.value("partitions").toArray();
        if (!d.value("label").toString().isEmpty() || !d.value("mountpoints").toArray().isEmpty()) partitions.prepend(d);
        m_volumes->setRowCount(partitions.size());
        for (int i = 0; i < partitions.size(); ++i) {
            const auto part = partitions[i].toObject();
            QStringList mounts; for (const auto m : part.value("mountpoints").toArray()) mounts << m.toString();
            const QStringList cells{field(part, "label"), mounts.isEmpty() ? "—" : mounts.join(", "), field(part, "device")};
            for (int col = 0; col < cells.size(); ++col) {
                auto *item = new QTableWidgetItem(cells[col]); item->setToolTip(cells[col]); m_volumes->setItem(i, col, item);
            }
        }
        const auto counters = d.value("counters").toObject();
        m_counters->setRowCount(counters.size()); int counterRow = 0;
        for (auto it = counters.begin(); it != counters.end(); ++it, ++counterRow) {
            m_counters->setItem(counterRow, 0, new QTableWidgetItem(QString(it.key()).replace('_', ' ')));
            m_counters->setItem(counterRow, 1, new QTableWidgetItem(it.value().toString()));
        }
        QStringList diagnostics;
        if (d.contains("error")) diagnostics << d.value("error").toString();
        if (!d.value("diagnostics").toString().isEmpty()) diagnostics << d.value("diagnostics").toString();
        for (const auto note : d.value("notes").toArray()) diagnostics << note.toString();
        m_details->setPlainText(diagnostics.isEmpty() ? "No diagnostic messages." : diagnostics.join("\n\n"));
        return;
    }
}

bool Window::setAutostart(bool enabled) {
    if (m_demo) return false;
    const auto path = autostartPath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    QByteArray contents = "[Desktop Entry]\nType=Application\nName=M45 Drive Watcher\nExec=m45-drive-watcher --autostart\nIcon=m45-drive-watcher\nTerminal=false\nHidden=";
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
    hints["desktop-entry"] = "com.m45software.DriveWatcher";
    hints["urgency"] = QVariant::fromValue(static_cast<uchar>(critical ? 2 : 1));
    hints["category"] = "device.error";
    if (service.isValid()) {
        service.asyncCall("Notify", "M45 Drive Watcher", uint(0), "m45-drive-watcher", title, text, QStringList{}, hints, int(-1));
    } else {
        m_tray.showMessage(title, text, critical ? QSystemTrayIcon::Critical : QSystemTrayIcon::Warning);
    }
}

void Window::Show() { showNormal(); raise(); activateWindow(); }
void Window::closeEvent(QCloseEvent *e) { hide(); e->ignore(); }
