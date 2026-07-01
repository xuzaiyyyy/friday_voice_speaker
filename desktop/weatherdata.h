#ifndef WEATHERDATA_H
#define WEATHERDATA_H

#include <QString>
#include <QList>

// 单日预报结构体
struct DailyForecast {
    QString date;       // 日期 (2023-10-01)
    QString tempMax;    // 最高温
    QString tempMin;    // 最低温
    QString iconDay;    // 白天天气图标代码 (100, 101...)
    QString textDay;    // 天气文字 (晴, 多云)
};

// 总天气数据结构体
struct WeatherData {
    QString cityId;     // 城市ID
    QString cityName;   // 城市名
    QString curTemp;    // 当前温度
    QString curIcon;    // 当前图标代码
    QString curText;    // 当前天气文字 (晴)

    QList<DailyForecast> dailyList; // 未来7天预报
};

#endif // WEATHERDATA_H
