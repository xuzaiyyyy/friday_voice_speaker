#include "selfinfowidget.h"
#include "ui_selfinfowidget.h"

SelfInfoWidget::SelfInfoWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::SelfInfoWidget)
{
    ui->setupUi(this);

    this->setWindowFlags(Qt::FramelessWindowHint);

    setStyleSheet("QWidget{border-radius:4px;background:rgba(255,255,255,1);}");  //设置圆角

    this->setAttribute(Qt::WA_TranslucentBackground,true);
    //实例阴影shadow
    QGraphicsDropShadowEffect *shadow = new QGraphicsDropShadowEffect(this);
    //设置阴影距离
    shadow->setOffset(0, 0);
    //设置阴影颜色
    shadow->setColor(QColor(39,40,43,100));
    //设置阴影圆角
    shadow->setBlurRadius(10);
    //给嵌套QWidget设置阴影
    setGraphicsEffect(shadow);
}

SelfInfoWidget::~SelfInfoWidget()
{
    delete ui;
}

void SelfInfoWidget::on_pushButton_clicked()
{

}

void SelfInfoWidget::on_pushBtn_hide_clicked()
{
    QWidget* pWindow = this->window();
    if(pWindow->isWindow())
    {
        pWindow->raise();
        pWindow->activateWindow();
    }
}

void SelfInfoWidget::on_pushBtn_close_clicked()
{
    this->close();
}
