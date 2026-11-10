#include "monitor.h"
#include <QTest>
#include <QJsonDocument>

namespace {
constexpr qint64 now = 100000;
QJsonObject drive(const QString &errors = "1135", int temperature = 45) {
    return {{"id", "serial-970"}, {"device", "/dev/nvme0"}, {"model", "Test SSD"},
            {"passed", true}, {"temperature", temperature}, {"available_spare", 92},
            {"spare_threshold", 10}, {"percentage_used", 3},
            {"counters", QJsonObject{{"media_errors", errors}, {"command_errors", "5000"}}}};
}
QJsonObject frame(QJsonObject d = drive(), qint64 time = now) {
    return {{"version", 1}, {"collected_at", time}, {"drives", QJsonArray{d}}};
}
}

class Tests : public QObject {
    Q_OBJECT
private slots:
    void firstCollectionGetsABoundedBootGracePeriod() {
        Monitor m;
        for (qint64 uptime : {0, 20, 40, 59}) {
            QVERIFY(m.missingCollector(uptime, now + uptime).isEmpty());
            QVERIFY(!m.warning());
            QVERIFY(m.events().isEmpty());
            QVERIFY(m.coverage().startsWith("Waiting for the first drive readings"));
        }
        QVERIFY(!m.missingCollector(60, now + 60).isEmpty());
        QVERIFY(m.warning());
        m.update(frame(drive(), now + 70), now + 70);
        // A genuine timeout remains in alert history until acknowledged.
        m.acknowledge();
        QVERIFY(!m.warning());
        QVERIFY(!m.coverage().startsWith("Waiting"));
        Monitor quickStart;
        quickStart.missingCollector(20, now + 20);
        quickStart.update(frame(drive(), now + 30), now + 30);
        QVERIFY(!quickStart.warning());
        QVERIFY(quickStart.events().isEmpty());
        Monitor unknownBoot;
        QVERIFY(!unknownBoot.missingCollector(-1, now).isEmpty());
        QVERIFY(unknownBoot.warning());
    }
    void bootGraceKeepsKnownDriveProblemsVisible() {
        Monitor m;
        m.update(frame(drive("1", 80)), now);
        QVERIFY(m.warning());
        const auto count = m.events().size();
        QVERIFY(m.missingCollector(20, now + 20).isEmpty());
        QVERIFY(m.warning());
        QCOMPARE(m.events().size(), count);
    }
    void uncheckedDrivePersistsAndStopsAllItsAlerts() {
        Monitor m;
        m.update(frame(drive("1", 80)), now);
        QVERIFY(m.warning());
        m.setMonitored("serial-970", false);
        QVERIFY(!m.warning());
        QCOMPARE(m.unreadCount(), 0);
        QVERIFY(!m.events().isEmpty());
        auto restored = Monitor(QJsonDocument::fromJson(QJsonDocument(m.state()).toJson()).object());
        auto failed = drive("2", 90); failed["passed"] = false;
        QVERIFY(restored.update(frame(failed, now + 60), now + 60).isEmpty());
        auto empty = frame(); empty["drives"] = QJsonArray(); empty["collected_at"] = now + 120;
        QVERIFY(restored.update(empty, now + 120).isEmpty());
        QVERIFY(!restored.warning());
        QVERIFY(restored.unavailable("Collector unavailable", now + 180).isEmpty());
        restored.setMonitored("serial-970", true);
        QVERIFY(restored.update(frame(drive("999"), now + 240), now + 240).isEmpty());
        QVERIFY(!restored.warning());
        auto hot = drive("999", 80);
        restored.update(frame(hot, now + 300), now + 300);
        QVERIFY(restored.warning());
        QCOMPARE(restored.unreadCount(), 1);
    }
    void confirmedUnsupportedDoesNotProduceHealthAlerts() {
        Monitor m;
        auto d = drive(); d.remove("passed"); d.remove("temperature");
        d["smart_support"] = "unsupported"; d["error"] = "No SMART support";
        for (int i = 0; i < 40; ++i)
            QVERIFY(m.update(frame(d, now + i * 60), now + i * 60).isEmpty());
        QVERIFY(!m.warning());
        QCOMPARE(m.coverage(), QString("No drives selected for monitoring."));
    }
    void externalDrivesSortLastAndSelectionSurvivesIdentityRecovery() {
        Monitor m;
        auto usb = drive(); usb["id"] = "usb"; usb["device"] = "/dev/sdb";
        usb["model"] = "Unknown drive"; usb["external"] = true; usb["error"] = "Cannot open";
        auto f = frame(); f["drives"] = QJsonArray{usb, drive()};
        m.update(f, now);
        QCOMPARE(m.drives().last().toObject().value("id").toString(), QString("usb"));
        m.setMonitored("usb", false);
        usb["id"] = "real-usb"; usb["model"] = "Expansion"; usb["system_id"] = "usbserial";
        f["drives"] = QJsonArray{usb, drive()}; f["collected_at"] = now + 60;
        m.update(f, now + 60);
        QCOMPARE(m.drives().size(), 2);
        QVERIFY(!m.drives().last().toObject().value("monitored").toBool(true));
        usb["id"] = "smart-usb"; usb["device"] = "/dev/sdd"; usb.remove("error");
        f["drives"] = QJsonArray{usb, drive()}; f["collected_at"] = now + 120;
        m.update(f, now + 120);
        QCOMPARE(m.drives().size(), 2);
        QVERIFY(!m.drives().last().toObject().value("monitored").toBool(true));
    }
    void failedSmartRefreshesLegacyUnknownNameAndLabels() {
        Monitor m;
        auto d = drive(); d["model"] = "Unknown drive"; d["error"] = "Cannot open";
        m.update(frame(d), now);
        d["model"] = "Expansion SW"; d["external"] = true; d["smart_support"] = "unsupported";
        d["partitions"] = QJsonArray{QJsonObject{{"label", "Comedy 3"}}};
        m.update(frame(d, now + 60), now + 60);
        const auto sample = m.drives().first().toObject().value("sample").toObject();
        QCOMPARE(sample.value("model").toString(), QString("Expansion SW"));
        QVERIFY(sample.value("external").toBool());
        QCOMPARE(sample.value("smart_support").toString(), QString("unsupported"));
        QCOMPARE(sample.value("partitions").toArray().first().toObject().value("label").toString(), QString("Comedy 3"));
    }
    void unsupportedDefaultsOffButExplicitChoicesPersist() {
        Monitor m;
        auto usb = drive(); usb["external"] = true; usb["error"] = "USB SMART unavailable";
        m.update(frame(usb), now);
        QVERIFY(!m.drives().first().toObject().value("monitored").toBool(true));
        QVERIFY(!m.warning());
        m.setMonitored("serial-970", true);
        Monitor restored(m.state());
        restored.update(frame(usb, now + 60), now + 60);
        QVERIFY(restored.drives().first().toObject().value("monitored").toBool());
        Monitor internal;
        usb["external"] = false;
        internal.update(frame(usb), now);
        QVERIFY(internal.drives().first().toObject().value("monitored").toBool());
    }
    void historicalCountsAreBaseline() {
        Monitor m;
        QVERIFY(m.update(frame(), now).isEmpty());
        QVERIFY(!m.warning());
        QVERIFY(m.update(frame(), now + 1).isEmpty());
    }
    void newMediaErrorLatchesAcrossRestart() {
        Monitor m;
        m.update(frame(), now);
        auto messages = m.update(frame(drive("1136"), now + 1), now + 1);
        QCOMPARE(messages.size(), 1);
        QVERIFY(messages[0].contains("1135 to 1136 (+1)"));
        QVERIFY(m.warning());
        // Exercise real JSON serialization, not just copying the in-memory object.
        Monitor resumed(QJsonDocument::fromJson(QJsonDocument(m.state()).toJson()).object());
        QVERIFY(resumed.warning());
        QVERIFY(resumed.update(frame(drive("1136"), now + 2), now + 2).isEmpty());
        resumed.acknowledge();
        QVERIFY(!resumed.warning());
    }
    void commandErrorsAndShutdownsDoNotAlert() {
        Monitor m;
        auto d = drive(); m.update(frame(d), now);
        auto c = d["counters"].toObject(); c["command_errors"] = "99999"; c["unsafe_shutdowns"] = "370";
        d["counters"] = c;
        QVERIFY(m.update(frame(d, now + 1), now + 1).isEmpty());
        QVERIFY(!m.warning());
    }
    void notificationsThrottleButChangesAreLogged() {
        Monitor m; m.update(frame(), now);
        QCOMPARE(m.update(frame(drive("1136"), now + 60), now + 60).size(), 1);
        QVERIFY(m.update(frame(drive("1137"), now + 120), now + 120).isEmpty());
        QCOMPARE(m.unreadCount(), 2);
        QCOMPARE(m.update(frame(drive("1138"), now + 1900), now + 1900).size(), 1);
    }
    void thermalHysteresisAndEscalation() {
        Monitor m;
        auto initial = m.update(frame(drive("0", 65)), now);
        QCOMPARE(initial.size(), 1);
        QVERIFY(initial[0].contains("65°C / 149°F"));
        m.acknowledge(); QVERIFY(m.warning());
        QVERIFY(m.update(frame(drive("0", 63), now + 60), now + 60).isEmpty());
        QCOMPARE(m.update(frame(drive("0", 76), now + 120), now + 120).size(), 1);
        QVERIFY(m.update(frame(drive("0", 72), now + 180), now + 180).isEmpty());
        QVERIFY(m.update(frame(drive("0", 69), now + 240), now + 240).isEmpty());
        m.acknowledge(); QVERIFY(m.warning());
        m.update(frame(drive("0", 59), now + 300), now + 300);
        QVERIFY(!m.warning());
    }
    void activeFailureNeverHidesOnAcknowledgement() {
        Monitor m; auto d = drive(); d["passed"] = false;
        QVERIFY(!m.update(frame(d), now).isEmpty());
        m.acknowledge(); QVERIFY(m.warning());
        QVERIFY(m.update(frame(d, now + 60), now + 60).isEmpty());
    }
    void additionalCriticalFlagAlerts() {
        Monitor m; auto d = drive(); d["critical_warning"] = 4;
        QCOMPARE(m.update(frame(d), now).size(), 1);
        m.acknowledge(); d["critical_warning"] = 12;
        QVERIFY(!m.update(frame(d, now + 60), now + 60).isEmpty());
    }
    void spareDecreaseAlerts() {
        Monitor m; m.update(frame(), now);
        auto d = drive(); d["available_spare"] = 91;
        QCOMPARE(m.update(frame(d, now + 60), now + 60).size(), 1);
    }
    void wearThresholdAndPendingAlwaysWarn() {
        Monitor m; auto d = drive(); d["percentage_used"] = 100;
        d["counters"] = QJsonObject{{"pending", "1"}};
        QCOMPARE(m.update(frame(d), now).size(), 2);
        m.acknowledge(); QVERIFY(m.warning());
    }
    void pendingTransitionOnlyOneEvent() {
        Monitor m; auto d = drive(); d["counters"] = QJsonObject{{"pending", "0"}};
        m.update(frame(d), now);
        d["counters"] = QJsonObject{{"pending", "1"}};
        QCOMPARE(m.update(frame(d, now + 60), now + 60).size(), 1);
        QCOMPARE(m.events().size(), 1);
    }
    void missingCounterDoesNotEraseBaseline() {
        Monitor m; m.update(frame(), now);
        auto d = drive(); d["counters"] = QJsonObject();
        m.update(frame(d, now + 60), now + 60);
        QCOMPARE(m.update(frame(drive("1136"), now + 120), now + 120).size(), 1);
    }
    void counterResetVisibleAndLargeIntegersPreserved() {
        Monitor m; m.update(frame(drive("9007199254740992")), now);
        QCOMPARE(m.update(frame(drive("9007199254740993"), now + 60), now + 60).size(), 1);
        auto messages = m.update(frame(drive("0"), now + 120), now + 120);
        QCOMPARE(messages.size(), 1); QVERIFY(messages[0].contains("decreased"));
    }
    void staleInvalidAndEmptyNeverShowHealthy() {
        Monitor m; m.update(frame(), now);
        QVERIFY(!m.update(frame(), now + 301).isEmpty());
        m.acknowledge(); QVERIFY(m.warning());
        m.update(frame(drive(), now + 302), now + 302); QVERIFY(!m.warning());
        auto invalid = frame(); invalid["version"] = 999;
        m.update(invalid, now + 303); QVERIFY(m.warning());
        auto empty = frame(); empty["drives"] = QJsonArray();
        m.update(empty, now + 304); QVERIFY(m.warning());
    }
    void serialIdentitySurvivesDeviceRenumbering() {
        Monitor m; m.update(frame(), now);
        auto d = drive("1136"); d["device"] = "/dev/nvme3";
        QCOMPARE(m.update(frame(d, now + 60), now + 60).size(), 1);
        QCOMPARE(m.drives().size(), 1);
    }
    void partialFailurePreservesCounterAndActiveFault() {
        Monitor m; auto d = drive(); d["critical_warning"] = 4; m.update(frame(d), now);
        auto failed = QJsonObject{{"id", "physical-path"}, {"device", "/dev/nvme0"}, {"model", "nvme0"}, {"error", "timeout"}};
        m.update(frame(failed, now + 60), now + 60);
        QCOMPARE(m.drives().size(), 1);
        auto active = m.drives()[0].toObject()["active"].toObject();
        QVERIFY(active.contains("nvme")); QVERIFY(!active.contains("read"));
        QCOMPARE(m.update(frame(drive("1136"), now + 120), now + 120).size(), 1);
    }
    void readFailuresRequireThirtyMinutesAcrossRestart() {
        Monitor m; m.update(frame(), now);
        auto failed = drive(); failed["error"] = "timeout";
        for (int elapsed = 60; elapsed <= 1800; elapsed += 60) {
            QVERIFY(m.update(frame(failed, now + elapsed), now + elapsed).isEmpty());
            QVERIFY(!m.warning());
        }
        Monitor resumed(QJsonDocument::fromJson(QJsonDocument(m.state()).toJson()).object());
        QCOMPARE(resumed.update(frame(failed, now + 1860), now + 1860).size(), 1);
        QVERIFY(resumed.warning());
        resumed.acknowledge();
        QVERIFY(resumed.update(frame(failed, now + 1920), now + 1920).isEmpty());
        QVERIFY(resumed.warning());
        resumed.update(frame(drive(), now + 1980), now + 1980);
        QVERIFY(!resumed.warning());
    }
    void recoveryAndGapsResetReadFailurePeriod() {
        Monitor m; m.update(frame(), now);
        auto failed = drive(); failed["error"] = "timeout";
        for (int elapsed = 60; elapsed <= 1800; elapsed += 60)
            m.update(frame(failed, now + elapsed), now + elapsed);
        m.update(frame(drive(), now + 1860), now + 1860);
        QVERIFY(m.update(frame(failed, now + 1920), now + 1920).isEmpty());
        // Missing observations cannot establish continuous failures, even after a restart.
        Monitor resumed(m.state());
        QVERIFY(resumed.update(frame(failed, now + 4000), now + 4000).isEmpty());
        QVERIFY(!resumed.warning());
        // Polling the same still-fresh snapshot doesn't advance the failure period.
        QVERIFY(resumed.update(frame(failed, now + 4000), now + 4250).isEmpty());
        QVERIFY(!resumed.warning());
    }
    void healthFailuresStillAlertDuringReadGracePeriod() {
        Monitor m;
        auto failed = drive(); failed["error"] = "incomplete read"; failed["passed"] = false;
        QCOMPARE(m.update(frame(failed), now).size(), 1);
        QVERIFY(m.drives()[0].toObject()["active"].toObject().contains("failed"));
        QVERIFY(!m.drives()[0].toObject()["active"].toObject().contains("read"));
    }
    void disconnectedDriveAndForget() {
        Monitor m; m.update(frame(), now);
        auto other = drive("0"); other["id"] = "another-drive"; other["device"] = "/dev/sda";
        QVERIFY(!m.update(frame(other, now + 60), now + 60).isEmpty());
        QCOMPARE(m.drives().size(), 2);
        m.forgetDisconnected(); QCOMPARE(m.drives().size(), 1);
        m.acknowledge(); QVERIFY(!m.warning());
    }
    void invalidDriveFrameDoesNotPartlyAdvanceBaseline() {
        Monitor m; m.update(frame(), now);
        auto f = frame(drive("1136"), now + 60);
        f["drives"] = QJsonArray{drive("1136"), QJsonObject()};
        m.update(f, now + 60);
        QCOMPARE(m.drives()[0].toObject()["sample"].toObject()["counters"].toObject()["media_errors"].toString(), QString("1135"));
    }
};
QTEST_GUILESS_MAIN(Tests)
#include "test_monitor.moc"
