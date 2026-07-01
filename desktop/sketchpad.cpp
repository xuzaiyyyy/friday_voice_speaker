#include "sketchpad.h"
#include "ui_sketchpad.h"
#include "apppaths.h"
#include "desktopidle.h"

#include <QList>
#include <QPushButton>
#include <QResizeEvent>
#include <QtGlobal>

sketchpad::sketchpad(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::sketchpad)
{
    ui->setupUi(this);
    this->setWindowFlags(Qt::FramelessWindowHint);

    draw_image = QImage(qMax(1, width() - 84), qMax(1, height()), QImage::Format_RGB32);
    draw_image.fill(Qt::white);
    updateResponsiveLayout();
}

sketchpad::~sketchpad()
{
    delete ui;
}

void sketchpad::on_sketchpad_back_clicked()
{
    auto *m = new MainWindow();
    replaceDesktopTopLevel(this, m);
}

void sketchpad::mousePressEvent(QMouseEvent *event){
    const QRect canvas = canvasRect();
    if((drawingEnabled || erasingEnabled) && (event->buttons() & Qt::LeftButton) && canvas.contains(event->pos())){
        printf("mousePressEvent\r\n");
        lastPoint = event->pos() - canvas.topLeft();
    }
}

void sketchpad::mouseMoveEvent(QMouseEvent *event){
    const QRect canvas = canvasRect();
    if((drawingEnabled || erasingEnabled) && (event->buttons() & Qt::LeftButton) && canvas.contains(event->pos())){
        printf("mouseMoveEvent\r\n");
        const QPoint localPoint = event->pos() - canvas.topLeft();
        QPainter painter(&draw_image);
        if(drawingEnabled){
            painter.setPen(QPen(draw_brush,draw_width,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        } else if(erasingEnabled){
            painter.setPen(QPen(Qt::white,draw_width+5,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        }
        painter.drawLine(lastPoint, localPoint);
        lastPoint = localPoint;
        update();
    }
}

void sketchpad::wheelEvent(QWheelEvent *event){
    Q_UNUSED(event);
    if((drawingEnabled || erasingEnabled)){
        printf("wheelEvent\r\n");
        int numDegrees = event->angleDelta().y() / 8;
        int numSteps = numDegrees / 15;
        draw_width = draw_width + numSteps;
        draw_width = draw_width>10?10:draw_width;
        draw_width = draw_width<1?1:draw_width;
    }
}

void sketchpad::paintEvent(QPaintEvent *event){
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), Qt::white);
    painter.drawImage(canvasRect().topLeft(), draw_image);
}

void sketchpad::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateResponsiveLayout();
}

QRect sketchpad::canvasRect() const
{
    const int shortSide = qMin(width(), height());
    const int toolbarWidth = qBound(58, shortSide / 7, 86);
    return QRect(0, 0, qMax(1, width() - toolbarWidth), qMax(1, height()));
}

void sketchpad::ensureCanvasSize(const QSize &size)
{
    if (!size.isValid() || draw_image.size() == size)
    {
        return;
    }

    QImage resized(size, QImage::Format_RGB32);
    resized.fill(Qt::white);
    if (!draw_image.isNull())
    {
        QPainter painter(&resized);
        painter.drawImage(QRect(QPoint(0, 0), size), draw_image);
    }
    draw_image = resized;
}

void sketchpad::updateResponsiveLayout()
{
    const QRect canvas = canvasRect();
    ensureCanvasSize(canvas.size());

    const int toolbarX = canvas.right() + 1;
    const int toolbarWidth = qMax(1, width() - toolbarX);
    ui->widget->setGeometry(toolbarX, 0, toolbarWidth, height());

    QList<QPushButton *> buttons{
        ui->brush,
        ui->eraser,
        ui->colors,
        ui->clear,
        ui->save,
        ui->images,
        ui->sketchpad_back};
    const int count = buttons.size();
    const int margin = qBound(4, qMin(width(), height()) / 80, 8);
    const int maxButton = qMax(34, toolbarWidth - margin * 2);
    const int buttonSize = qBound(34, qMin(maxButton, (height() - margin * (count + 1)) / count), 58);
    const int spacing = qMax(margin, (height() - buttonSize * count) / (count + 1));
    int y = spacing;
    for (QPushButton *button : buttons)
    {
        button->setGeometry((toolbarWidth - buttonSize) / 2, y, buttonSize, buttonSize);
        y += buttonSize + spacing;
    }
    update();
}

bool sketchpad::saveImage()
{
    QString saveDir = AppPaths::ensureDesktopDir("photos");

    QString savePath = saveDir + "/draw_image_" +
            QString::number(QRandomGenerator::global()->bounded(0,9999)) + ".jpg";

    if(draw_image.save(savePath)){
        return true;
    }
    else{
        return false;
    }
}



void sketchpad::on_brush_clicked(bool checked)
{
    printf("on_brush_clicked %d\r\n", checked);
    if(checked){
        drawingEnabled = true;
        erasingEnabled = false;
        ui->eraser->setChecked(false);
    }
    else
        drawingEnabled = false;
}

void sketchpad::on_eraser_clicked(bool checked)
{
    printf("on_eraser_clicked %d\r\n", checked);
    if(checked){
        erasingEnabled = true;
        drawingEnabled = false;
        ui->brush->setChecked(false);
    }
    else
        erasingEnabled = false;
}

void sketchpad::on_save_clicked()
{
    printf("on_save_clicked \r\n");

    if(isNotEmpty(draw_image)){
        if(saveImage()){
            QMessageBox::information(this, "Saved successfully", "Image saved successfully!");
        }
        else{
            QMessageBox::warning(this, "Save failed", "Image saving failed!");
        }
    } else {
        QMessageBox::warning(this, "Save failed", "Image is empty!");
    }
}

void sketchpad::on_images_clicked()
{
    printf("on_images_clicked\r\n");

    QString openDir = AppPaths::ensureDesktopDir("photos");

    // 2. 打开文件选择对话框，只允许选择图片格式
    QString fileName = QFileDialog::getOpenFileName(this,
                                                    tr("Open Image"), // 对话框标题
                                                    openDir,          // 默认路径
                                                    tr("Images (*.png *.jpg *.jpeg *.bmp)")); // 文件过滤器

    // 如果用户点了取消，fileName 会为空，直接返回
    if (fileName.isEmpty()) {
        return;
    }

    // 3. 加载外部图片到一个临时的 QImage 对象中
    QImage loadedImage;
    if (!loadedImage.load(fileName)) {
        QMessageBox::warning(this, tr("Error"), tr("Could not load image."));
        return;
    }

    // ================== 核心逻辑开始 ==================

    // 4. 图片缩放处理
    // 外部图片可能很大(例如相机照片)也可能很小。我们需要把它缩放到适合画布的大小(900x600)。
    // Qt::KeepAspectRatio: 保持宽高比缩放，不会把图片拉变形。
    // Qt::SmoothTransformation: 使用平滑缩放算法，图片质量更好。
    QImage scaledImage = loadedImage.scaled(draw_image.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);

    // 5. 清空当前的画布
    // 这一步看你的需求：是想保留之前的涂鸦再叠加上去，还是清空了只显示新图片？
    // 通常打开新图片意味着清空过去。
    draw_image.fill(Qt::white);

    // 6. 计算居中位置
    // 因为是保持比例缩放，缩放后的图片可能填不满整个 900x600 的区域。
    // 我们需要计算坐标，把它画在画布的正中央。
    int x = (draw_image.width() - scaledImage.width()) / 2;
    int y = (draw_image.height() - scaledImage.height()) / 2;

    // 7. 将缩放好的图片画到我们的主画布(draw_image)上
    QPainter painter(&draw_image);
    // 提示：如果图片有透明通道，最好先设置组合模式，不过JPG通常不需要
    // painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.drawImage(x, y, scaledImage);
    painter.end(); // 结束绘制

    // ================== 核心逻辑结束 ==================

    // 8. 触发重绘，让界面显示新的内容
    update();

    // 9. 可选：加载图片后自动切换回画笔模式，方便接着画
    ui->brush->setChecked(true);
    on_brush_clicked(true);

    printf("Image loaded successfully and drawn onto canvas.\r\n");
}

void sketchpad::on_colors_clicked()
{
    QColor color = QColorDialog::getColor(Qt::white, this, "Choose Color");

    if (color.isValid()) {
        qDebug() << color.name() << endl;
        draw_brush.setColor(color);
    }
}

bool sketchpad::isNotEmpty(const QImage &image)
{
    // 获取图像的尺寸
    int width = image.width();
    int height = image.height();

    // 遍历图像的每个像素
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // 获取当前像素的颜色
            QColor color = image.pixelColor(x, y);

            // 检查颜色是否为白色
            if (color != QColor(Qt::white)) {
                return true;  // 如果找到不为白色的像素，则图像不为空
            }
        }
    }

    return false;  // 如果所有像素都为白色，则图像为空
}


void sketchpad::on_clear_clicked()
{
    printf("on_clear_clicked\r\n");
    draw_image.fill(Qt::white);
    update();
}
