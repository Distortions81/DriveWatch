#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QStringList>

struct Policy {
    int warm = 65;
    int hot = 75;
    int hysteresis = 5;
    int staleSeconds = 300;
    int cooldownSeconds = 1800;
    int readFailureSeconds = 1800;
};

QString displayTemperature(double celsius);

class Monitor {
public:
    explicit Monitor(QJsonObject saved = {});
    QStringList update(const QJsonObject &snapshot, qint64 now, const Policy &policy = {});
    QStringList unavailable(const QString &reason, qint64 now, const Policy &policy = {});
    void acknowledge();
    void setMonitored(const QString &id, bool enabled);
    void forgetDisconnected();
    bool warning() const;
    int unreadCount() const;
    QJsonObject state() const { return m_state; }
    QJsonArray drives() const;
    QJsonArray events() const { return m_state.value("events").toArray(); }
    QString coverage() const { return m_state.value("coverage").toString(); }

private:
    QJsonObject m_state;
    void acknowledgeDrive(const QString &id);
    void event(const QString &id, const QString &key, const QString &message, const QString &level,
               qint64 now, const Policy &policy, QStringList &notifications);
    void active(QJsonObject &record, const QJsonObject &conditions, qint64 now,
                const Policy &policy, QStringList &notifications);
};
