#ifndef FRIENDINFOWIDGET_H
#define FRIENDINFOWIDGET_H

#include <QWidget>

namespace Ui {
class FriendInfoWidget;
}

class FriendInfoWidget : public QWidget
{
    Q_OBJECT

public:
    explicit FriendInfoWidget(QWidget *parent = nullptr);
    ~FriendInfoWidget();

private:
    Ui::FriendInfoWidget *ui;
};

#endif // FRIENDINFOWIDGET_H
