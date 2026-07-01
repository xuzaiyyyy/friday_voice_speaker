#ifndef IMAGEPREVIEW_H
#define IMAGEPREVIEW_H

#include <QWidget>

namespace Ui {
class ImagePreview;
}

class ImagePreview : public QWidget
{
    Q_OBJECT

public:
    explicit ImagePreview(QPixmap &pixmap, QWidget *parent = nullptr);
    ~ImagePreview();

private:
    Ui::ImagePreview *ui;
};

#endif // IMAGEPREVIEW_H
