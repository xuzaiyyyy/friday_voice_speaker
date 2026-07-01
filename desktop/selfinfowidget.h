#ifndef SELFINFOWIDGET_H
#define SELFINFOWIDGET_H

#include <QWidget>
#include <QGraphicsDropShadowEffect>
#include <client.h>

namespace Ui {
class SelfInfoWidget;
}

class SelfInfoWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SelfInfoWidget(QWidget *parent = nullptr);
    ~SelfInfoWidget();

private slots:
    void on_pushButton_clicked();

    void on_pushBtn_hide_clicked();

    void on_pushBtn_close_clicked();

private:
    Ui::SelfInfoWidget *ui;
};

#endif // SELFINFOWIDGET_H
