#ifndef CALCULATE_H
#define CALCULATE_H

#include <QWidget>
#include <mainwindow.h>
#include <QStack>

class QResizeEvent;

namespace Ui {
class calculate;
}

class calculate : public QWidget
{
    Q_OBJECT

public:
    explicit calculate(QWidget *parent = nullptr);
    ~calculate();

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void on_pushButton_clicked();

    void on_onebutton_clicked();

    void on_twobutton_clicked();

    void on_threebutton_clicked();

    void on_fourbutton_clicked();

    void on_fivebutton_clicked();

    void on_sixbutton_clicked();

    void on_sevenbutton_clicked();

    void on_eightbutton_clicked();

    void on_ninebutton_clicked();

    void on_zerobutton_clicked();

    void on_leftbutton_clicked();

    void on_rightbutton_clicked();

    void on_equalbutton_clicked();

    void on_divbutton_clicked();

    void on_mulbutton_clicked();

    void on_delebutton_clicked();

    void on_sumbutton_clicked();

    void on_addbutton_clicked();

    void on_clearbutton_clicked();

    int Priority(char ch);

private:
    void updateResponsiveLayout();

    Ui::calculate *ui;
    QString expression;
};


#endif // CALCULATE_H
