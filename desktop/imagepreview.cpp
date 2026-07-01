#include "imagepreview.h"
#include "ui_imagepreview.h"

ImagePreview::ImagePreview(QPixmap &pixmap, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ImagePreview)
{
    ui->setupUi(this);
}

ImagePreview::~ImagePreview()
{
    delete ui;
}
