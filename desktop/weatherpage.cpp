#include "weatherpage.h"
#include "ui_weatherpage.h"
#include "desktopidle.h"
#include <QDebug> // 引入调试头文件
#include <QFrame>
#include <QResizeEvent>
#include <QScrollBar>
#include <QtGlobal>

WeatherPage::WeatherPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::WeatherPage)
{
    ui->setupUi(this);

    this->setWindowFlags(Qt::FramelessWindowHint);
    if (layout() != nullptr)
    {
        layout()->setContentsMargins(0, 0, 0, 0);
        layout()->setSpacing(0);
    }
    ui->scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->scrollArea->setFrameShape(QFrame::NoFrame);

    // 在 initUI() 函数的末尾调用
    this->setStyleSheet(
        // === 1. 全局字体与背景 (深蓝灰渐变) ===
        "QWidget { font-family: 'Microsoft YaHei', 'Segoe UI'; }"
        "WeatherPage { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #2C3E50, stop:1 #4CA1AF); }"

        "QLabel { color: white; }"

        // === 2. 滚动区完全透明 (露出背景) ===
        "QScrollArea { background: transparent; border: none; }"
        "QWidget#scrollWidget { background: transparent; }"

        // === 3. 顶部天气文字样式 ===
        "QLabel#lblLocation { color: white; font-size: 24px; font-weight: bold; }"
        "QLabel#lblTemp { color: white; font-size: 48px; font-weight: 200; margin: 0; }"
        "QLabel#lblCondition { color: rgba(255,255,255,220); font-size: 16px; }"
        "QLabel#lblHiLo { color: white; font-size: 14px; }"

        // === 4. 一周预报容器 (玻璃卡片效果) ===
        "QFrame#weeklyFrame { "
        "   background-color: rgba(0, 0, 0, 50); "       // 半透明黑底
        "   border-radius: 15px; "                        // 圆角
        "   border: 1px solid rgba(255, 255, 255, 30); "  // 细微描边
        "}"
        "QFrame#hLine { background-color: rgba(255,255,255,30); }" // 分割线颜色

        // === 5. 底部导航栏 (半透明磨砂条) ===
        "QFrame#bottomBar { "
        "   background-color: rgba(20, 30, 40, 200); "    // 稍微深一点，且不完全透明，防止文字混淆
        "   border-top: 1px solid rgba(255, 255, 255, 40); "
        "}"

        // === 6. 底部圆形按钮 (核心修改) ===
        "QPushButton#btnLocation, QPushButton#btnBack { "
        "   background-color: rgba(255, 255, 255, 30); "  // 玻璃质感背景
        "   border: 1px solid rgba(255, 255, 255, 60); "  // 亮边框
        "   border-radius: 25px; "                        // 50px的一半，确保正圆
        "   padding: 4px; "                               // 【关键】极小内边距，让图标尽可能大
        "}"

        // === 7. 按钮交互效果 ===
        "QPushButton#btnLocation:hover, QPushButton#btnBack:hover { "
        "   background-color: rgba(255, 255, 255, 50); "  // 悬停变亮
        "}"
        "QPushButton#btnLocation:pressed, QPushButton#btnBack:pressed { "
        "   background-color: rgba(255, 255, 255, 20); "  // 按下变暗
        "   padding: 6px; "                               // 按下时图标微缩，产生按压感
        "}"

        // === 8. 按钮图标映射 (请确保你的资源路径正确) ===
        "QPushButton#btnLocation { image: url(:/pic/weatherselect.png); }"
        "QPushButton#btnBack     { image: url(:/pic/backweather.png); }"
    );
    updateResponsiveLayout();

    m_weatherTool = new weathertool(this);

    // 连接信号：当工具类拿到数据，就更新 UI
    connect(m_weatherTool, &weathertool::weatherDataReceived, this, &WeatherPage::updateUI);

    // 【测试】启动时直接查询 南京浦口 (ID: 101190104)
    m_weatherTool->queryWeather("101190104", "南京浦口");
}

WeatherPage::~WeatherPage()
{
    delete ui;
}

void WeatherPage::on_btnBack_clicked()
{
    auto *w = new MainWindow();
    replaceDesktopTopLevel(this, w);
}

void WeatherPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateResponsiveLayout();
}

void WeatherPage::updateResponsiveLayout()
{
    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0)
    {
        return;
    }

    QWidget *page = ui->scrollWidget;
    page->setMinimumSize(w, h);
    page->resize(w, h);
    ui->scrollArea->setGeometry(0, 0, w, h);

    const int shortSide = qMin(w, h);
    const int margin = qBound(8, shortSide / 40, 14);
    const int bottomHeight = qBound(48, h / 8, 60);
    const int headerHeight = qBound(112, h * 31 / 100, 150);
    const int weeklyY = headerHeight;
    const int weeklyHeight = qMax(120, h - headerHeight - bottomHeight - margin);
    const int contentWidth = qMax(1, w - margin * 2);

    const int locationHeight = qBound(24, h / 15, 34);
    const int tempHeight = qBound(44, h / 8, 62);
    const int conditionHeight = qBound(20, h / 24, 28);
    const int hiLoHeight = qBound(18, h / 28, 24);
    int y = margin;

    ui->lblLocation->setGeometry(margin, y, contentWidth, locationHeight);
    y += locationHeight - 1;
    ui->lblTemp->setGeometry(margin, y, contentWidth, tempHeight);
    y += tempHeight - 2;
    ui->lblCondition->setGeometry(margin, y, contentWidth, conditionHeight);
    y += conditionHeight - 1;
    ui->lblHiLo->setGeometry(margin, y, contentWidth, hiLoHeight);

    ui->frameWeekly->setGeometry(margin, weeklyY, contentWidth, weeklyHeight);
    ui->frameWeekly->setStyleSheet(QStringLiteral(
        "QFrame#frameWeekly { background-color: rgba(0, 0, 0, 45);"
        " border-radius: %1px; border: 1px solid rgba(255, 255, 255, 34); }")
                                       .arg(qBound(8, shortSide / 36, 14)));

    if (auto *weeklyLayout = qobject_cast<QVBoxLayout *>(ui->frameWeekly->layout()))
    {
        const int itemCount = qMax(1, weeklyLayout->count());
        const int forecastRows = qMax(1, (itemCount + 1) / 2);
        m_dailyRowHeight = qBound(28, (weeklyHeight - forecastRows) / forecastRows, 42);
        weeklyLayout->setContentsMargins(qBound(6, margin / 2, 10), 2, qBound(6, margin / 2, 10), 2);
        weeklyLayout->setSpacing(0);
    }
    else
    {
        m_dailyRowHeight = qBound(30, (weeklyHeight - 6) / 7, 42);
    }
    m_dailyFontSize = qBound(12, m_dailyRowHeight / 2, 17);
    m_dailyIconFontSize = qBound(16, m_dailyRowHeight * 3 / 5, 22);
    m_dailyDayWidth = qBound(48, w / 11, 78);
    m_dailyIconWidth = qBound(34, w / 18, 50);
    m_dailyTempWidth = qBound(34, w / 20, 44);
    m_dailyTempBarWidth = qBound(56, w / 7, 116);

    ui->frame->setGeometry(0, h - bottomHeight, w, bottomHeight);
    ui->frame->setStyleSheet(QStringLiteral(
        "QFrame#frame { background-color: rgba(20, 30, 40, 205);"
        " border-top: 1px solid rgba(255, 255, 255, 42); }"));

    const int buttonSize = qBound(40, bottomHeight - margin, 50);
    const int buttonY = (bottomHeight - buttonSize) / 2;
    ui->btnLocation->setGeometry(margin, buttonY, buttonSize, buttonSize);
    ui->btnBack->setGeometry(w - margin - buttonSize, buttonY, buttonSize, buttonSize);
    ui->btnLocation->setIconSize(QSize(buttonSize - 12, buttonSize - 12));
    ui->btnBack->setIconSize(QSize(buttonSize - 12, buttonSize - 12));
    ui->frame->raise();

    ui->lblLocation->setStyleSheet(QStringLiteral("color: white; background: transparent; font-size: %1px; font-weight: 700;")
                                       .arg(qBound(20, h / 16, 30)));
    ui->lblTemp->setStyleSheet(QStringLiteral("color: white; background: transparent; font-size: %1px; font-weight: 300;")
                                   .arg(qBound(40, h / 7, 58)));
    ui->lblCondition->setStyleSheet(QStringLiteral("color: rgba(255,255,255,220); background: transparent; font-size: %1px;")
                                        .arg(qBound(14, h / 28, 20)));
    ui->lblHiLo->setStyleSheet(QStringLiteral("color: white; background: transparent; font-size: %1px;")
                                   .arg(qBound(13, h / 32, 18)));
    ui->lblLocation->raise();
    ui->lblTemp->raise();
    ui->lblCondition->raise();
    ui->lblHiLo->raise();
}

// =========================================================
//  核心修复部分：updateUI
// =========================================================
void WeatherPage::updateUI(WeatherData data)
{
    updateResponsiveLayout();

    // 1. 更新顶部大字
    ui->lblLocation->setText(data.cityName);
    ui->lblTemp->setText(data.curTemp + "°");
    ui->lblCondition->setText(data.curText);

    // 从预报列表里取今天的最高/最低温
    if(!data.dailyList.isEmpty()) {
        ui->lblHiLo->setText(QString("最高 %1°  最低 %2°")
                           .arg(data.dailyList[0].tempMax)
                           .arg(data.dailyList[0].tempMin));
    }

    // ========================================================
    // 【修复开始】: 解决 layout 空指针崩溃问题
    // ========================================================

    // 尝试获取 frameWeekly 的布局
    QVBoxLayout *weeklyLayout = qobject_cast<QVBoxLayout*>(ui->frameWeekly->layout());

    // 如果布局不存在（Designer里忘了设），代码手动创建一个！
    if (!weeklyLayout) {
        qDebug() << "检测到 frameWeekly 没有布局，正在自动创建...";
        weeklyLayout = new QVBoxLayout(ui->frameWeekly);
        ui->frameWeekly->setLayout(weeklyLayout);
    }
    weeklyLayout->setContentsMargins(6, 2, 6, 2);
    weeklyLayout->setSpacing(0);

    // 2. 清空旧列表 (现在使用 weeklyLayout 指针，安全可靠)
    QLayoutItem *child;
    while ((child = weeklyLayout->takeAt(0)) != nullptr) {
        if(child->widget()) {
            child->widget()->deleteLater(); // 删除 Widget
        }
        delete child; // 删除 LayoutItem
    }

    const int forecastCount = qMin(data.dailyList.count(), 7);
    if (forecastCount > 0)
    {
        const int weeklyHeight = qMax(1, ui->frameWeekly->height());
        m_dailyRowHeight = qBound(28, (weeklyHeight - forecastCount - 4) / forecastCount, 42);
        m_dailyFontSize = qBound(12, m_dailyRowHeight / 2, 17);
        m_dailyIconFontSize = qBound(16, m_dailyRowHeight * 3 / 5, 22);
    }

    // 3. 重新填充
    for(int i=0; i<forecastCount; i++) {
        DailyForecast daily = data.dailyList[i];

        QString dayText = (i==0) ? "今天" : getWeekDay(daily.date); // 转换日期

        // 创建一行
        QWidget *row = createDailyItem(
            dayText,
            getWeatherIcon(daily.iconDay), // 获取图标或Emoji
            daily.tempMin,
            daily.tempMax
        );
        weeklyLayout->addWidget(row);

        // 加分割线
        if(i < forecastCount - 1) {
            QFrame *line = new QFrame();
            line->setObjectName("hLine"); // 使用之前的QSS样式
            line->setFixedHeight(1);
            weeklyLayout->addWidget(line);
        }
    }
}

// 辅助函数：简单转换日期为星期
QString WeatherPage::getWeekDay(QString dateStr)
{
    QDate date = QDate::fromString(dateStr, "yyyy-MM-dd");
    int day = date.dayOfWeek(); // 1=Mon, 7=Sun
    QString weeks[] = {"", "周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    return weeks[day];
}

// 辅助函数：简单映射图标
QString WeatherPage::getWeatherIcon(QString code)
{
    int c = code.toInt();
    if(c == 100) return "☀️";
    if(c >= 101 && c <= 104) return "☁️";
    if(c >= 300 && c < 400) return "🌧️";
    if(c >= 400 && c < 500) return "❄️";
    return "⛅";
}

QWidget* WeatherPage::createDailyItem(const QString &day, const QString &icon, const QString &min, const QString &max)
{
    // 创建一个容器 Widget
    QWidget *widget = new QWidget();
    widget->setFixedHeight(m_dailyRowHeight);

    // 水平布局
    QHBoxLayout *layout = new QHBoxLayout(widget);
    const int sideMargin = qBound(6, m_dailyRowHeight / 4, 12);
    layout->setContentsMargins(sideMargin, 0, sideMargin, 0);
    layout->setSpacing(qBound(4, m_dailyRowHeight / 6, 8));

    // 1. 日期标签
    QLabel *lblDay = new QLabel(day);
    lblDay->setStyleSheet(QStringLiteral("color: white; font-size: %1px; font-weight: bold;")
                              .arg(m_dailyFontSize));
    lblDay->setFixedWidth(m_dailyDayWidth);
    lblDay->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // 2. 天气图标
    QLabel *lblIcon = new QLabel(icon);
    lblIcon->setAlignment(Qt::AlignCenter);
    lblIcon->setStyleSheet(QStringLiteral("font-size: %1px;").arg(m_dailyIconFontSize));
    lblIcon->setFixedWidth(m_dailyIconWidth);

    // 3. 最低温度
    QLabel *lblMin = new QLabel(min + "°");
    lblMin->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 170); font-size: %1px;")
                              .arg(m_dailyFontSize));
    lblMin->setFixedWidth(m_dailyTempWidth);
    lblMin->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    // 4. 温度进度条
    QFrame *tempBar = new QFrame();
    tempBar->setFixedHeight(4);
    tempBar->setFixedWidth(m_dailyTempBarWidth);
    tempBar->setStyleSheet("background-color: rgba(255, 255, 255, 40); border-radius: 2px;");

    // 5. 最高温度
    QLabel *lblMax = new QLabel(max + "°");
    lblMax->setStyleSheet(QStringLiteral("color: white; font-size: %1px; font-weight: 500;")
                              .arg(m_dailyFontSize));
    lblMax->setFixedWidth(m_dailyTempWidth);
    lblMax->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // 按照顺序添加到布局
    layout->addWidget(lblDay);
    layout->addWidget(lblIcon);
    layout->addStretch(1);
    layout->addWidget(lblMin);
    layout->addWidget(tempBar);
    layout->addWidget(lblMax);

    return widget;
}

void WeatherPage::on_btnLocation_clicked()
{
    QMenu *menu = new QMenu(this);

    // 1. 设置样式 (保持不变)
    menu->setStyleSheet(
        "QMenu { background-color: rgba(40, 50, 60, 240); color: white; border: 1px solid rgba(255,255,255,50); border-radius: 5px; font-size: 16px; }"
        "QMenu::item { padding: 8px 25px; }"
        "QMenu::item:selected { background-color: rgba(255, 255, 255, 40); }"
    );

    // 2. 定义城市列表 (保持不变)
    QMap<QString, QString> cityMap;
    cityMap.insert("北京", "101010100");
    cityMap.insert("上海", "101020100");
    cityMap.insert("广州", "101280101");
    cityMap.insert("深圳", "101280601");
    cityMap.insert("南京", "101190101");
    cityMap.insert("南京浦口", "101190104");
    cityMap.insert("苏州", "101190401");
    cityMap.insert("杭州", "101210101");

    // 3. 添加菜单项 (保持不变)
    for (auto city : cityMap.keys()) {
        QAction *action = new QAction(city, this);
        menu->addAction(action);
        connect(action, &QAction::triggered, [=]() {
            m_weatherTool->queryWeather(cityMap.value(city), city);
        });
    }

    // ============================================================
    // 【核心修复】计算弹出位置：让菜单在按钮【上方】弹出
    // ============================================================

    // 1. 获取按钮在屏幕上的左上角坐标
    QPoint btnPos = ui->btnLocation->mapToGlobal(QPoint(0, 0));

    // 2. 预先计算菜单的大小 (必须先 sizeHint 否则高度是0)
    QSize menuSize = menu->sizeHint();

    // 3. 计算最终坐标：x=按钮x, y=按钮y - 菜单高度 (即向上平移)
    // 稍微还要减去一点像素(比如 5px)留出空隙
    int x = btnPos.x();
    int y = btnPos.y() - menuSize.height() - 5;

    // 4. 在计算好的位置弹出
    menu->exec(QPoint(x, y));

    // 5. 用完记得删除，防止内存泄漏
    menu->deleteLater();
}
