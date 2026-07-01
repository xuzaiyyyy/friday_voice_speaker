#ifndef WEATHERPAGE_H
#define WEATHERPAGE_H

#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDate>
#include <QDebug>
#include <QMenu>
#include <QAction>
#include <QCursor>

class QResizeEvent;

// 包含你定义的工具类和数据头文件
#include "weatherdata.h"
#include "weathertool.h"

// 假设你的主窗口头文件叫 mainwindow.h，如果不需要跳转回主页可以去掉
#include "mainwindow.h"

namespace Ui {
class WeatherPage;
}

class WeatherPage : public QWidget
{
    Q_OBJECT

public:
    explicit WeatherPage(QWidget *parent = nullptr);
    ~WeatherPage();

private slots:
    void on_btnBack_clicked();      // 返回按钮槽函数
    void updateUI(WeatherData data); // 核心：更新界面的槽函数

    void on_btnLocation_clicked();

private:
    void resizeEvent(QResizeEvent *event) override;
    void updateResponsiveLayout();

    Ui::WeatherPage *ui;
    weathertool *m_weatherTool;     // 网络工具对象
    int m_dailyRowHeight = 38;
    int m_dailyFontSize = 15;
    int m_dailyIconFontSize = 18;
    int m_dailyDayWidth = 64;
    int m_dailyIconWidth = 42;
    int m_dailyTempWidth = 38;
    int m_dailyTempBarWidth = 96;

    // 辅助函数：动态创建一周预报的单行 Widget
    QWidget* createDailyItem(const QString &day, const QString &icon, const QString &min, const QString &max);

    // 辅助函数：根据天气代码返回图标 (Emoji 或 图片路径)
    QString getWeatherIcon(QString code);

    // 辅助函数：将日期字符串转换为星期几
    QString getWeekDay(QString dateStr);
};

#endif // WEATHERPAGE_H
