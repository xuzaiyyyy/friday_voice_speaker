#include "weathertool.h"


weathertool::weathertool(QObject *parent) : QObject(parent)
{
    m_netManager = new QNetworkAccessManager(this);
}

void weathertool::queryWeather(QString cityId, QString cityName)
{
    m_apiKey = "********";

    // 1. 直接保存传进来的 ID 和 名字
    m_tempData.cityId = cityId;
    m_tempData.cityName = cityName; // 直接赋值，不需要写一堆 if-else 了！

    // 2. HTTPS 请求 (保持不变)
    QString urlNow = QString("https://********/v7/weather/now?location=%1&key=%2")
                        .arg(cityId).arg(m_apiKey);

    QNetworkRequest request((QUrl(urlNow)));
    QSslConfiguration config = request.sslConfiguration();
    config.setPeerVerifyMode(QSslSocket::VerifyNone);
    config.setProtocol(QSsl::AnyProtocol);
    request.setSslConfiguration(config);

    QNetworkReply *reply = m_netManager->get(request);

    connect(reply, &QNetworkReply::finished, [=]() {
        onNowWeatherReceived(reply);
    });
}

void weathertool::onNowWeatherReceived(QNetworkReply *reply)
{
    // 检查网络错误
    if(reply->error() != QNetworkReply::NoError) {
        qDebug() << "!!! 实时天气请求出错:" << reply->errorString();
        emit errorOccurred("网络错误: " + reply->errorString());
        reply->deleteLater();
        return;
    }

    // 【修正3】只读取一次数据，存入变量
    QByteArray bytes = reply->readAll();
    qDebug() << "2. 收到实时数据(JSON):" << bytes; // 这里必须看到 JSON

    parseNowJson(bytes);
    reply->deleteLater(); // 销毁旧请求

    // ---------------------------------------------------------
    // 【修正4】域名必须统一！不能用 devapi，必须用 nq6r...
    // ---------------------------------------------------------
    QString urlDaily = QString("https://********/v7/weather/7d?location=%1&key=%2")
            .arg(m_tempData.cityId).arg(m_apiKey);

    qDebug() << "3. 开始请求7天预报:" << urlDaily;

    QNetworkRequest request = QNetworkRequest(QUrl(urlDaily));
    // 同样忽略 SSL 错误
    QSslConfiguration config = request.sslConfiguration();
    config.setPeerVerifyMode(QSslSocket::VerifyNone);
    config.setProtocol(QSsl::AnyProtocol);
    request.setSslConfiguration(config);

    QNetworkReply *replyDaily = m_netManager->get(request);

    connect(replyDaily, &QNetworkReply::finished, [=](){
        // 【修正5】绝对不能在这里用 reply！要用 replyDaily
        onDailyWeatherReceived(replyDaily);
    });
}

void weathertool::onDailyWeatherReceived(QNetworkReply *reply)
{
    if(reply->error() != QNetworkReply::NoError) {
        qDebug() << "!!! 预报请求出错:" << reply->errorString();
        emit errorOccurred("预报获取失败");
        reply->deleteLater();
        return;
    }

    QByteArray bytes = reply->readAll();
    qDebug() << "4. 收到预报数据(JSON):" << bytes;

    parseDailyJson(bytes);
    reply->deleteLater();

    // 发送信号刷新界面
    qDebug() << "5. 数据全部就绪，更新 UI";
    emit weatherDataReceived(m_tempData);
}

void weathertool::parseNowJson(QByteArray bytes)
{
    QJsonDocument doc = QJsonDocument::fromJson(bytes);
    QJsonObject root = doc.object();

    // 只有 code 为 200 才解析，否则打印错误
    if(root["code"].toString() == "200") {
        QJsonObject now = root["now"].toObject();
        m_tempData.curTemp = now["temp"].toString();
        m_tempData.curText = now["text"].toString();
        m_tempData.curIcon = now["icon"].toString();
    } else {
        qDebug() << "解析实时天气失败，Code:" << root["code"].toString();
    }
}

void weathertool::parseDailyJson(QByteArray bytes)
{
    QJsonDocument doc = QJsonDocument::fromJson(bytes);
    QJsonObject root = doc.object();

    if(root["code"].toString() == "200") {
        m_tempData.dailyList.clear();
        QJsonArray dailyArr = root["daily"].toArray();

        for(auto v : dailyArr) {
            QJsonObject dayObj = v.toObject();
            DailyForecast forecast;
            forecast.date = dayObj["fxDate"].toString();
            forecast.tempMax = dayObj["tempMax"].toString();
            forecast.tempMin = dayObj["tempMin"].toString();
            forecast.textDay = dayObj["textDay"].toString();
            forecast.iconDay = dayObj["iconDay"].toString();
            m_tempData.dailyList.append(forecast);
        }
    } else {
        qDebug() << "解析预报失败，Code:" << root["code"].toString();
    }
}
