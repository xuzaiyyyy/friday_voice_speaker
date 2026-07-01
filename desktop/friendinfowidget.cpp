#include "friendinfowidget.h"
#include "ui_friendinfowidget.h"

FriendInfoWidget::FriendInfoWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::FriendInfoWidget)
{
    ui->setupUi(this);
}

FriendInfoWidget::~FriendInfoWidget()
{
    delete ui;
}
