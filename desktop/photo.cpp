#include "photo.h"
#include "ui_photo.h"
#include "apppaths.h"
#include "desktopidle.h"

#include <QResizeEvent>
#include <QTimer>
#include <QtGlobal>

photo::photo(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::photo)
{
    ui->setupUi(this);
    this->setWindowFlags(Qt::FramelessWindowHint);
    this->setAttribute(Qt::WA_AcceptTouchEvents, true);

    // 1. 设置为图标模式 (从左到右排列)
    ui->list_photos->setViewMode(QListWidget::IconMode);

    // 2. 设置图标图片的大小 (宽150, 高150) -> 这一步最关键，不写这个就是你看不到图的原因
    ui->list_photos->setIconSize(QSize(150, 150));

    // 3. 设置每个格子的总大小 (要比图标大一点，给文字留位置)
    ui->list_photos->setGridSize(QSize(180, 200));

    // 4. 让图标自动适应窗口调整换行
    ui->list_photos->setResizeMode(QListWidget::Adjust);


    // 1. 开启像素级滚动 (这是丝滑的关键！)
    // 默认是 ScrollPerItem (按项滚动)，必须改成 ScrollPerPixel
    ui->list_photos->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    ui->list_photos->setSelectionMode(QAbstractItemView::SingleSelection);
    ui->list_photos->setSelectionBehavior(QAbstractItemView::SelectItems);
    ui->list_photos->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // 2. 开启手势拖拽
    QScroller::grabGesture(ui->list_photos, QScroller::LeftMouseButtonGesture);

    // 3. 【进阶】调整物理参数，让它滑起来更有“iPhone感”
    QScroller *scroller = QScroller::scroller(ui->list_photos);
    QScrollerProperties properties = scroller->scrollerProperties();

    // 阻尼感 (数值越大摩擦力越大，滑得越慢)
    properties.setScrollMetric(QScrollerProperties::DecelerationFactor, 0.5);
    // 最大速度
    properties.setScrollMetric(QScrollerProperties::MaximumVelocity, 1);
    // 过冲效果 (滑到底部回弹的幅度)
    properties.setScrollMetric(QScrollerProperties::OvershootDragResistanceFactor, 0.1);
    properties.setScrollMetric(QScrollerProperties::OvershootScrollDistanceFactor, 0.1);

    scroller->setScrollerProperties(properties);

    // 4. 隐藏滚动条
    ui->list_photos->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->list_photos->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->list_menu->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->list_menu->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    ui->list_menu->setCurrentRow(0);
    loadPhotos();
    updateResponsiveLayout();

}

photo::~photo()
{
    delete ui;
}

void photo::loadPhotos()
{
    ui->list_photos->clear();
    QString path = AppPaths::ensureDesktopDir("photos");
    QDir dir(path);

    // 2. 检查文件夹是否存在
    if (!dir.exists()) {
        qDebug() << "错误：找不到照片文件夹！" << path;
        return;
    }

    // 3. 设置过滤器，只看图片文件 (过滤掉 .txt 或其他杂文件)
    QStringList filters;
    filters << "*.jpg" << "*.png" << "*.jpeg";
    dir.setNameFilters(filters);

    // 4. 获取文件列表
    QFileInfoList fileList = dir.entryInfoList();

    for (const QFileInfo &fileInfo : fileList) {
        QListWidgetItem *item = new QListWidgetItem();

        item->setIcon(QIcon(fileInfo.absoluteFilePath()));
        item->setText(fileInfo.baseName());

        // 【新增】视频也要存路径
        item->setData(Qt::UserRole, fileInfo.absoluteFilePath());

        ui->list_photos->addItem(item);
    }

}

void photo::loadVideos()
{
    ui->list_photos->clear();
    QString path = AppPaths::ensureDesktopDir("videos");
    QDir dir(path);

    if (!dir.exists()) {
        qDebug() << "无视频目录:" << path;
        return;
    }

    QStringList filters;
    filters << "*.mp4" << "*.avi" << "*.mkv"<< "*.mov"; // 只看视频文件
    dir.setNameFilters(filters);

    QFileInfoList fileList = dir.entryInfoList();

    for (const QFileInfo &fileInfo : fileList) {
        QListWidgetItem *item = new QListWidgetItem();

        // 【注意】QIcon 默认不支持直接显示视频缩略图
        // 这里我们可以先用一个通用的“视频图标”代替
        // 记得把 video_icon.png 放到你的资源文件里
        item->setIcon(QIcon(AppPaths::existingDesktopFile("images/video_icon.png")));

        // 【新增】视频也要存路径
        item->setData(Qt::UserRole, fileInfo.absoluteFilePath());

        item->setText(fileInfo.baseName());
        ui->list_photos->addItem(item);
    }
}

void photo::on_list_menu_itemClicked(QListWidgetItem *item)
{
    // 1. 每次切换前，先清空右边列表
    ui->list_photos->clear();

    // 2. 判断点击的是哪一项
    QString text = item->text();

    if (text == "照片") {
        loadPhotos(); // 调用你之前写好的加载照片函数
    }
    else if (text == "视频") {
        loadVideos(); // 调用加载视频函数
    }
}

void photo::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateResponsiveLayout();
}

void photo::updateResponsiveLayout()
{
    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0)
    {
        return;
    }

    const int shortSide = qMin(w, h);
    const int sideWidth = qBound(82, w * 18 / 100, 142);
    const int margin = qBound(8, shortSide / 42, 14);
    const int buttonSize = qBound(42, shortSide / 10, 54);
    const int actionWidth = buttonSize + margin * 2;
    const int contentWidth = qMax(1, w - sideWidth - actionWidth);
    const int actionX = sideWidth + contentWidth;
    const int iconSize = qBound(72, contentWidth / 5, 120);
    const int gridWidth = iconSize + qBound(24, contentWidth / 18, 42);
    const int gridHeight = iconSize + 44;
    const int menuItemHeight = qBound(44, h / 8, 58);

    ui->list_menu->setGeometry(0, 0, sideWidth, h);
    ui->list_photos->setGeometry(sideWidth, 0, contentWidth, h);
    ui->list_photos->setIconSize(QSize(iconSize, iconSize));
    ui->list_photos->setGridSize(QSize(gridWidth, gridHeight));
    ui->list_photos->setSpacing(qBound(4, shortSide / 80, 8));

    const int buttonX = actionX + margin;
    ui->photo_back_2->setGeometry(buttonX, margin, buttonSize, buttonSize);
    ui->photo_back->setGeometry(buttonX, h - margin - buttonSize, buttonSize, buttonSize);
    ui->photo_back->raise();
    ui->photo_back_2->raise();

    ui->list_menu->setStyleSheet(QStringLiteral(
        "#list_menu { background-color: #F2F2F7; border: none; border-right: 1px solid #D1D1D6; outline: none; padding-top: %1px; font-size: %2px; }"
        "#list_menu::item { height: %3px; color: #484848; margin: 4px 8px; border-radius: 8px; border: none; padding-left: 8px; }"
        "#list_menu::item:selected { background-color: #FFFFFF; color: #007AFF; }")
                                    .arg(margin)
                                    .arg(qBound(14, shortSide / 26, 18))
                                    .arg(menuItemHeight));

    ui->list_photos->setStyleSheet(QStringLiteral(
        "#list_photos { background-color: #FFFFFF; border: none; outline: none; padding: %1px; font-size: %2px; }"
        "#list_photos::item { margin: 4px; }"
        "#list_photos::item:selected { background-color: #F0F8FF; border: 2px solid #007AFF; border-radius: 8px; color: #333333; }")
                                      .arg(margin)
                                      .arg(qBound(11, shortSide / 34, 15)));
}

void photo::on_photo_back_clicked()
{
    auto *m = new MainWindow();
    replaceDesktopTopLevel(this, m);
}

void photo::on_photo_back_2_clicked()
{
    QListWidgetItem *item = ui->list_photos->currentItem();
    const QList<QListWidgetItem *> selectedItems = ui->list_photos->selectedItems();
    if (item == nullptr && !selectedItems.isEmpty())
    {
        item = selectedItems.first();
    }
    if (item == nullptr)
    {
        qDebug() << "photo delete ignored: no selected item";
        return;
    }

    QString filePath = item->data(Qt::UserRole).toString();
    if (filePath.isEmpty())
    {
        qDebug() << "photo delete failed: selected item has no file path";
        return;
    }

    QFile file(filePath);

    if (file.remove())
    {
        int row = ui->list_photos->row(item);
        delete ui->list_photos->takeItem(row);
        if (ui->list_photos->count() > 0)
        {
            const int nextRow = qMin(row, ui->list_photos->count() - 1);
            ui->list_photos->setCurrentRow(nextRow);
        }
        qDebug() << "photo deleted:" << filePath;
        ui->photo_back_2->setText(QStringLiteral("OK"));
        QTimer::singleShot(700, this, [this]()
                           { ui->photo_back_2->setText(QString()); });
    }
    else
    {
        qDebug() << "photo delete failed:" << filePath << file.errorString();
    }
}


void photo::on_list_photos_itemDoubleClicked(QListWidgetItem *item)
{

    //qDebug() << "双击触发";
    // 1. 获取文件路径
    QString filePath = item->data(Qt::UserRole).toString();

    //qDebug() <<"文件路径是"<<filePath;

    // 2. 判断是不是视频文件 (通过后缀名)
    // 简单的判断方法：看路径结尾是不是 .mp4 或 .avi
    if (filePath.endsWith(".mp4", Qt::CaseInsensitive) ||
        filePath.endsWith(".avi", Qt::CaseInsensitive) ||
        filePath.endsWith(".mov", Qt::CaseInsensitive) ||
        filePath.endsWith(".mkv", Qt::CaseInsensitive))
    {
        if (activeVideo)
        {
            activeVideo->raise();
            activeVideo->activateWindow();
            return;
        }

        // 3. 如果是视频，打开独立预览层，避免 QVideoWidget 和相册列表层级混在一起。
        auto *vp = new VideoPlayer(filePath);
        activeVideo = vp;
        vp->setAttribute(Qt::WA_DeleteOnClose, true);
        connect(vp, &QObject::destroyed, this, [this]() {
            activeVideo = nullptr;
            raise();
            activateWindow();
        });
        showVideoTopLevel(vp);

        // 这里的逻辑是：打开新窗口覆盖在上面，旧窗口不动
    }
    else
    {
        // 如果是照片，你也可以在这里写打开大图的逻辑
        // 或者留空，只做视频播放
        printf("Selected item is a photo, skipping video player.\n");
    }
}
