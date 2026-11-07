#include "monitor.h"
#include <QSet>
#include <algorithm>

QString displayTemperature(double celsius) {
    return QString("%1°C / %2°F").arg(qRound(celsius)).arg(qRound(celsius * 9.0 / 5.0 + 32.0));
}

namespace {
QJsonObject condition(const QString &message, const QString &level = "warning") {
    return {{"message", message}, {"level", level}};
}
QString name(const QJsonObject &d) {
    return d.value("model").toString() + " (" + d.value("device").toString() + ")";
}
const QMap<QString, QString> watched = {
    {"media_errors", "media/data integrity errors"}, {"uncorrectable", "reported uncorrectable errors"},
    {"reallocated", "reallocated sectors/blocks"}, {"pending", "pending sectors"},
    {"offline_uncorrectable", "offline uncorrectable sectors"}, {"end_to_end", "end-to-end errors"},
    {"crc_errors", "interface CRC errors"}
};
}

Monitor::Monitor(QJsonObject saved) : m_state(std::move(saved)) {
    m_state["version"] = 1;
}

void Monitor::event(const QString &id, const QString &key, const QString &message, const QString &level,
                    qint64 now, const Policy &policy, QStringList &notifications) {
    auto log = events();
    auto cooldowns = m_state.value("cooldowns").toObject();
    const auto token = id + ":" + key + ":" + level;
    const qint64 previous = cooldowns.value(token).toInteger(0);
    if (!previous || now - previous >= policy.cooldownSeconds) {
        notifications << message;
        cooldowns[token] = now;
    }
    // Keep every change in the log, even when its desktop notification is throttled.
    log.append(QJsonObject{{"drive_id", id}, {"key", key}, {"message", message},
                           {"level", level}, {"at", now}, {"acknowledged", false}});
    while (log.size() > 500) {
        // Never silently discard an unread warning to make room for history.
        auto it = std::find_if(log.begin(), log.end(), [](const QJsonValue &v) {
            return v.toObject().value("acknowledged").toBool();
        });
        if (it != log.end()) log.erase(it);
        else {
            // Coalesce oldest unread entries into a visible summary, bounding file growth.
            auto first = log.first().toObject();
            int count = first.value("coalesced").toInt(1) + 1;
            first["message"] = QString("%1 older unacknowledged alerts; review drive counters.").arg(count);
            first["coalesced"] = count;
            first["drive_id"] = "history";
            log[0] = first;
            log.removeAt(1);
        }
    }
    m_state["events"] = log;
    m_state["cooldowns"] = cooldowns;
}

void Monitor::active(QJsonObject &record, const QJsonObject &conditions, qint64 now,
                     const Policy &policy, QStringList &notifications) {
    const auto old = record.value("active").toObject();
    for (auto it = conditions.begin(); it != conditions.end(); ++it) {
        // A level change is a new event; acknowledgements never hide an active fault.
        const auto prior = old.value(it.key()).toObject();
        const auto current = it.value().toObject();
        const bool escalated = prior.value("level").toString() != "critical" && current.value("level").toString() == "critical";
        const bool signatureChanged = current.contains("signature") &&
            (current.value("signature").toInt() & ~prior.value("signature").toInt()) != 0;
        bool alreadyLogged = false;
        for (const auto v : events()) {
            const auto e = v.toObject();
            if (e.value("at").toInteger() == now && e.value("drive_id") == record.value("id")
                && e.value("key").toString() == it.key()) alreadyLogged = true;
        }
        if ((!old.contains(it.key()) || escalated || signatureChanged) && !alreadyLogged) {
            const auto c = it.value().toObject();
            const QString eventKey = c.contains("signature") ? it.key() + "_" + QString::number(c.value("signature").toInt()) : it.key();
            event(record.value("id").toString(), eventKey, c.value("message").toString(),
                  c.value("level").toString(), now, policy, notifications);
        }
    }
    record["active"] = conditions;
}

QStringList Monitor::unavailable(const QString &reason, qint64 now, const Policy &policy) {
    QStringList notices;
    QJsonObject record = m_state.value("global").toObject();
    record["id"] = "monitor";
    active(record, {{"coverage", condition(reason)}}, now, policy, notices);
    m_state["global"] = record;
    m_state["coverage"] = reason;
    return notices;
}

QStringList Monitor::update(const QJsonObject &snapshot, qint64 now, const Policy &policy) {
    if (snapshot.value("version").toInt() != 1 || !snapshot.value("drives").isArray()
        || !snapshot.value("collected_at").isDouble())
        return unavailable("Collector data is invalid or uses an unsupported version.", now, policy);
    const qint64 collected = snapshot.value("collected_at").toInteger();
    if (collected > now + 60 || now - collected > policy.staleSeconds)
        return unavailable("Drive readings are stale. Check the collector service.", now, policy);
    if (snapshot.value("drives").toArray().isEmpty())
        return unavailable("No drives were detected. Monitoring coverage is unavailable.", now, policy);

    QStringList notices;
    auto records = m_state.value("drives").toObject();
    QSet<QString> seen;
    bool incomplete = false;
    // Validate the whole frame before changing any baselines.
    for (const auto value : snapshot.value("drives").toArray()) {
        const auto d = value.toObject();
        const auto id = d.value("id").toString();
        if (id.isEmpty() || d.value("device").toString().isEmpty() || seen.contains(id))
            return unavailable("Collector returned an invalid drive record.", now, policy);
        seen.insert(id);
    }
    seen.clear();
    for (const auto value : snapshot.value("drives").toArray()) {
        auto d = value.toObject();
        auto id = d.value("id").toString();
        if (id.isEmpty() || d.value("device").toString().isEmpty())
            return unavailable("Collector returned an invalid drive record.", now, policy);
        // A timed-out drive may have no serial; associate it with its last known path.
        if (d.contains("error") && !records.contains(id)) {
            for (auto it = records.begin(); it != records.end(); ++it)
                if (it.value().toObject().value("sample").toObject().value("device") == d.value("device")) {
                    id = it.key();
                    d["id"] = id;
                    break;
                }
        }
        seen.insert(id);
        auto record = records.value(id).toObject();
        const auto previous = record.value("sample").toObject();
        const bool baseline = !record.contains("sample");
        record["id"] = id;
        record["connected"] = true;
        record["last_seen"] = collected;
        QJsonObject conditions;
        const QString label = name(d);
        if (d.contains("error")) {
            incomplete = true;
            conditions["read"] = condition(label + ": " + d.value("error").toString());
            // Keep prior faults visible while the device cannot be queried.
            const auto old = record.value("active").toObject();
            for (auto it = old.begin(); it != old.end(); ++it)
                if (it.key() != "missing" && it.key() != "read") conditions[it.key()] = it.value();
        }
        if (d.value("passed").isBool() && !d.value("passed").toBool())
            conditions["failed"] = condition(label + ": SMART reports a failing drive.", "critical");
        const int flags = d.value("critical_warning").toInt();
        if (flags) {
            QStringList descriptions;
            if (flags & 1) descriptions << "spare capacity below threshold";
            if (flags & 2) descriptions << "temperature outside device limits";
            if (flags & 4) descriptions << "reliability degraded";
            if (flags & 8) descriptions << "media is read-only";
            if (flags & 16) descriptions << "volatile-memory backup failure";
            if (flags & ~31) descriptions << "additional device warning";
            conditions["nvme"] = condition(label + ": " + descriptions.join(", ") + ".", "critical");
            auto c = conditions.value("nvme").toObject();
            c["signature"] = flags;
            conditions["nvme"] = c;
        }
        if (d.contains("available_spare") && d.contains("spare_threshold")
            && d.value("available_spare").toInt() <= d.value("spare_threshold").toInt())
            conditions["spare"] = condition(label + ": available spare is at or below its threshold.", "critical");
        if (!d.value("failing_attributes").toArray().isEmpty())
            conditions["attributes"] = condition(label + ": a SMART attribute is currently failing.", "critical");
        for (const QString &key : {QString("pending"), QString("offline_uncorrectable")}) {
            if (d.value("counters").toObject().value(key).toString().toULongLong() > 0)
                conditions[key] = condition(label + ": nonzero " + watched.value(key) + ".");
        }
        if (d.value("percentage_used").isDouble() && d.value("percentage_used").toInt() >= 90) {
            int used = d.value("percentage_used").toInt();
            conditions["wear"] = condition(label + QString(": estimated endurance used is %1%.").arg(used),
                                            used >= 100 ? "critical" : "warning");
        }
        if (d.value("temperature").isDouble()) {
            double temp = d.value("temperature").toDouble();
            const auto oldTemp = record.value("active").toObject().value("temperature").toObject();
            const bool wasHot = oldTemp.value("level").toString() == "critical";
            const bool wasWarm = !oldTemp.isEmpty();
            if (temp >= policy.hot || (wasHot && temp >= policy.hot - policy.hysteresis))
                conditions["temperature"] = condition(label + QString(": temperature is %1 (critical at %2).")
                                                       .arg(displayTemperature(temp), displayTemperature(policy.hot)), "critical");
            else if (temp >= policy.warm || (wasWarm && temp >= policy.warm - policy.hysteresis))
                conditions["temperature"] = condition(label + QString(": temperature is %1 (warning at %2).")
                                                       .arg(displayTemperature(temp), displayTemperature(policy.warm)));
        } else if (!d.contains("error")) {
            conditions["temperature_unknown"] = condition(label + ": temperature is unavailable.");
        }
        if (!baseline && !d.contains("error")) {
            const auto oldCounters = previous.value("counters").toObject();
            const auto counters = d.value("counters").toObject();
            for (auto it = watched.begin(); it != watched.end(); ++it) {
                if (!oldCounters.contains(it.key()) || !counters.contains(it.key())) continue;
                bool oldOk, newOk;
                auto old = oldCounters.value(it.key()).toString().toULongLong(&oldOk);
                auto current = counters.value(it.key()).toString().toULongLong(&newOk);
                if (!oldOk || !newOk) continue;
                if (current > old)
                    event(id, it.key(), label + QString(": %1 increased from %2 to %3 (+%4).")
                          .arg(it.value()).arg(old).arg(current).arg(current - old), "warning", now, policy, notices);
                else if (current < old)
                    event(id, "counter_reset", label + ": a health counter decreased; baseline was updated.",
                          "warning", now, policy, notices);
            }
            if (d.contains("available_spare") && previous.contains("available_spare")
                && d.value("available_spare").toInt() < previous.value("available_spare").toInt())
                event(id, "spare_change", label + QString(": available spare decreased from %1% to %2%.")
                      .arg(previous.value("available_spare").toInt()).arg(d.value("available_spare").toInt()),
                      "warning", now, policy, notices);
        }
        active(record, conditions, now, policy, notices);
        // Preserve last known counter values across partial reads for future comparisons.
        if (!d.contains("error")) {
            auto merged = previous.value("counters").toObject();
            const auto current = d.value("counters").toObject();
            for (auto it = current.begin(); it != current.end(); ++it) merged[it.key()] = it.value();
            d["counters"] = merged;
            if (!d.contains("available_spare") && previous.contains("available_spare"))
                d["available_spare"] = previous.value("available_spare");
            record["sample"] = d;
        } else {
            auto retained = previous.isEmpty() ? d : previous;
            retained["device"] = d.value("device");
            retained["error"] = d.value("error");
            record["sample"] = retained;
        }
        records[id] = record;
    }
    for (auto it = records.begin(); it != records.end(); ++it) {
        if (seen.contains(it.key())) continue;
        auto record = it.value().toObject();
        record["connected"] = false;
        active(record, {{"missing", condition(name(record.value("sample").toObject()) + ": drive is no longer detected.")}},
               now, policy, notices);
        it.value() = record;
    }
    m_state["drives"] = records;
    m_state["collected_at"] = collected;
    m_state["coverage"] = incomplete ? "Some drives could not be read." : "Monitoring is up to date.";
    m_state["global"] = QJsonObject();
    return notices;
}

void Monitor::acknowledge() {
    auto log = events();
    for (int i = 0; i < log.size(); ++i) {
        auto row = log[i].toObject();
        row["acknowledged"] = true;
        log[i] = row;
    }
    m_state["events"] = log;
}

void Monitor::forgetDisconnected() {
    auto records = m_state.value("drives").toObject();
    for (auto it = records.begin(); it != records.end();) {
        if (!it.value().toObject().value("connected").toBool()) it = records.erase(it);
        else ++it;
    }
    m_state["drives"] = records;
}

int Monitor::unreadCount() const {
    int count = 0;
    for (const auto v : events()) if (!v.toObject().value("acknowledged").toBool()) ++count;
    return count;
}

bool Monitor::warning() const {
    if (unreadCount() || !m_state.value("global").toObject().value("active").toObject().isEmpty()) return true;
    for (const auto v : m_state.value("drives").toObject())
        if (!v.toObject().value("active").toObject().isEmpty()) return true;
    return false;
}

QJsonArray Monitor::drives() const {
    QJsonArray result;
    const auto records = m_state.value("drives").toObject();
    for (auto it = records.begin(); it != records.end(); ++it) result.append(it.value());
    return result;
}
