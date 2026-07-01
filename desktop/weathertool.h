#ifndef WEATHERTOOL_H
#define WEATHERTOOL_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "weatherdata.h"
#include <QDebug>
#include <QNetworkRequest>
#include <QSslConfiguration> // 必须引入，用于处理虚拟机SSL问题
#include <QSslSocket>


class weathertool : public QObject
{
    Q_OBJECT
public:
    explicit weathertool(QObject *parent = nullptr);

    // 输入城市ID (例如南京浦口是 101190104)
    void queryWeather(QString cityId, QString cityName);

signals:
    // 数据准备好后发送信号给界面
    void weatherDataReceived(WeatherData data);
    void errorOccurred(QString errorMsg);

private slots:
    void onNowWeatherReceived(QNetworkReply *reply);
    void onDailyWeatherReceived(QNetworkReply *reply);

private:
    QNetworkAccessManager *m_netManager;
    QString m_apiKey = "********"; // 【重要】填入你的Key

    // 暂存数据，因为我们需要请求两次接口 (实时+7天)
    WeatherData m_tempData;

    void parseNowJson(QByteArray bytes);
    void parseDailyJson(QByteArray bytes);
};

#endif // WEATHERTOOL_H
