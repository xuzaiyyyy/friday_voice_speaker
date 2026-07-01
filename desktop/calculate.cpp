#include "calculate.h"
#include "ui_calculate.h"
#include "apppaths.h"
#include "desktopidle.h"

#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QtGlobal>

calculate::calculate(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::calculate)
{
    ui->setupUi(this);

    this->setWindowFlags(Qt::FramelessWindowHint);
    this->setAttribute(Qt::WA_TranslucentBackground, false);
    this->setAutoFillBackground(true);
    this->setStyleSheet(QStringLiteral(
        "calculate, QWidget#calculate { background: #ffffff; }"
        "QFrame#frame { background: #ffffff; border: none; }"
        "QLabel#picture { background: #ffffff; border: none; }"
        "QLineEdit#lineEdit { background: #ffffff; color: #111111; border: none; padding: 0 12px; }"
        "QPushButton { background: #f8f8f8; color: #222222; border: 1px solid #b8b8b8; }"
        "QPushButton:pressed { background: #e6f2ff; }"));
    ui->frame->setAutoFillBackground(true);
    ui->picture->setAutoFillBackground(true);
    ui->picture->setAlignment(Qt::AlignCenter);
    ui->picture->setScaledContents(false);
    ui->picture->setStyleSheet(QStringLiteral("background: #ffffff; border: none;"));

    // 阴影设置保持不变
    QGraphicsDropShadowEffect * shadowEffect = new QGraphicsDropShadowEffect();
    shadowEffect->setOffset(0, 0);
    shadowEffect->setColor(QColor(0, 0, 0, 64));
    shadowEffect->setBlurRadius(20);
    ui->frame->setGraphicsEffect(shadowEffect);




    QImage img;
    if (img.load(AppPaths::existingDesktopFile("images/calculator.png")))
    {
        ui->picture->setPixmap(QPixmap::fromImage(img));
    }
    for (QPushButton *button : findChildren<QPushButton *>())
    {
        button->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    }
    updateResponsiveLayout();
}

calculate::~calculate()
{
    delete ui;
}

void calculate::on_pushButton_clicked()
{
    auto *w = new MainWindow();
    replaceDesktopTopLevel(this, w);
}

void calculate::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateResponsiveLayout();
}

void calculate::updateResponsiveLayout()
{
    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0)
    {
        return;
    }

    const int shortSide = qMin(w, h);
    const int margin = qBound(6, shortSide / 50, 12);
    const int displayHeight = qBound(56, h / 6, 92);
    const int backHeight = qBound(44, h / 10, 64);
    const int usableHeight = qMax(1, h - displayHeight - backHeight - margin * 3);
    const int preferredGridWidth = (w >= h) ? qBound(260, w * 38 / 100, 360) : qMax(1, w - margin * 2);
    const int cell = qMax(42, qMin(preferredGridWidth / 4, usableHeight / 5));
    const int gridWidth = cell * 4;
    const int gridHeight = cell * 5;
    const int gridX = margin;
    const int gridY = displayHeight + margin;
    const int pictureX = gridX + gridWidth + margin;
    const int pictureW = qMax(0, w - pictureX - margin);
    const int pictureY = gridY;
    const int pictureH = qMax(0, h - pictureY - margin);

    ui->frame->setGeometry(0, 0, w, h);
    ui->lineEdit->setGeometry(0, 0, w, displayHeight);

    auto place = [&](QPushButton *button, int col, int row, int rowSpan = 1)
    {
        button->setGeometry(gridX + col * cell,
                            gridY + row * cell,
                            cell,
                            cell * rowSpan);
    };

    place(ui->clearbutton, 0, 0);
    place(ui->addbutton, 1, 0);
    place(ui->sumbutton, 2, 0);
    place(ui->delebutton, 3, 0);
    place(ui->sevenbutton, 0, 1);
    place(ui->eightbutton, 1, 1);
    place(ui->ninebutton, 2, 1);
    place(ui->mulbutton, 3, 1);
    place(ui->fourbutton, 0, 2);
    place(ui->fivebutton, 1, 2);
    place(ui->sixbutton, 2, 2);
    place(ui->divbutton, 3, 2);
    place(ui->onebutton, 0, 3);
    place(ui->twobutton, 1, 3);
    place(ui->threebutton, 2, 3);
    place(ui->equalbutton, 3, 3, 2);
    place(ui->leftbutton, 0, 4);
    place(ui->zerobutton, 1, 4);
    place(ui->rightbutton, 2, 4);

    const int backY = qMin(h - backHeight, gridY + gridHeight + margin);
    ui->pushButton->setGeometry(gridX, backY, gridWidth, backHeight);

    if (pictureW > shortSide / 5 && pictureH > shortSide / 5)
    {
        ui->picture->setVisible(true);
        ui->picture->setGeometry(pictureX, pictureY, pictureW, pictureH);
    }
    else
    {
        ui->picture->setVisible(false);
    }

    QFont displayFont = ui->lineEdit->font();
    displayFont.setPointSize(qBound(24, displayHeight / 2, 44));
    ui->lineEdit->setFont(displayFont);

    QFont keyFont;
    keyFont.setPointSize(qBound(15, cell / 3, 28));
    for (QPushButton *button : findChildren<QPushButton *>())
    {
        button->setFont(keyFont);
    }
}

void calculate::on_onebutton_clicked()
{
    expression += "1";
    ui->lineEdit->setText(expression);
}

void calculate::on_twobutton_clicked()
{
    expression += "2";
    ui->lineEdit->setText(expression);
}

void calculate::on_threebutton_clicked()
{
    expression += "3";
    ui->lineEdit->setText(expression);
}

void calculate::on_fourbutton_clicked()
{
    expression += "4";
    ui->lineEdit->setText(expression);
}

void calculate::on_fivebutton_clicked()
{
    expression += "5";
    ui->lineEdit->setText(expression);
}

void calculate::on_sixbutton_clicked()
{
    expression += "6";
    ui->lineEdit->setText(expression);
}

void calculate::on_sevenbutton_clicked()
{
    expression += "7";
    ui->lineEdit->setText(expression);
}

void calculate::on_eightbutton_clicked()
{
    expression += "8";
    ui->lineEdit->setText(expression);
}

void calculate::on_ninebutton_clicked()
{
    expression += "9";
    ui->lineEdit->setText(expression);
}

void calculate::on_zerobutton_clicked()
{
    expression += "0";
    ui->lineEdit->setText(expression);
}

void calculate::on_leftbutton_clicked()
{
    expression += "(";
    ui->lineEdit->setText(expression);
}

void calculate::on_rightbutton_clicked()
{
    expression += ")";
    ui->lineEdit->setText(expression);
}

void calculate::on_divbutton_clicked()
{
    expression += "/";
    ui->lineEdit->setText(expression);
}

void calculate::on_mulbutton_clicked()
{
    expression += "*";
    ui->lineEdit->setText(expression);
}

void calculate::on_delebutton_clicked()
{
    expression.chop(1);
    ui->lineEdit->setText(expression);
}

void calculate::on_sumbutton_clicked()
{
    expression += "-";
    ui->lineEdit->setText(expression);
}

void calculate::on_addbutton_clicked()
{
    expression += "+";
    ui->lineEdit->setText(expression);
}

void calculate::on_clearbutton_clicked()
{
    expression.clear();
    ui->lineEdit->clear();
}

void calculate::on_equalbutton_clicked()
{
    QStack<int> s_num, s_opt;

    char opt[128] = {0};
    int i = 0, tmp = 0, num1, num2;

    //把QString转换成char *
    QByteArray ba;
    ba.append(expression);   //把QString转换成QByteArray
    strcpy(opt, ba.data());  //data可以把QByteArray转换成const char *

    while (opt[i] != '\0' || s_opt.empty() != true)
    {
        if (opt[i] >= '0' && opt[i] <= '9')
        {
            tmp = tmp * 10 + opt[i] - '0';
            i++;
            if (opt[i] < '0' || opt[i] > '9')
            {
                s_num.push(tmp);
                tmp = 0;
            }
        }
        else           //操作符
        {
            if (s_opt.empty() == true || Priority(opt[i]) > Priority(s_opt.top()) ||
                    (s_opt.top() == '(' && opt[i] != ')'))
            {
                s_opt.push(opt[i]);
                i++;
                continue;
            }

            if (s_opt.top() == '(' && opt[i] == ')')
            {
                s_opt.pop();
                i++;
                continue;
            }

            if (Priority(opt[i]) <= Priority(s_opt.top()) || (opt[i] == ')' && s_opt.top() != '(') ||
                (opt[i] == '\0' && s_opt.empty() != true))
            {
                char ch = s_opt.top();
                s_opt.pop();
                /*减法和除法，先出栈的作为第二个参数   后缀表达式*/
                switch(ch)
                {
                    case '+':
                        num1 = s_num.top();
                        s_num.pop();
                        num2 = s_num.top();
                        s_num.pop();
                        s_num.push(num1 + num2);
                        break;
                    case '-':
                        num1 = s_num.top();
                        s_num.pop();
                        num2 = s_num.top();
                        s_num.pop();
                        s_num.push(num2 - num1);
                        break;
                    case '*':
                        num1 = s_num.top();
                        s_num.pop();
                        num2 = s_num.top();
                        s_num.pop();
                        s_num.push(num1 * num2);
                        break;
                    case '/':
                        num1 = s_num.top();
                        s_num.pop();
                        num2 = s_num.top();
                        s_num.pop();
                        s_num.push(num2 / num1);
                        break;
                }
            }
        }
    }
    ui->lineEdit->setText(QString::number(s_num.top()));
    expression.clear();
}

int calculate::Priority(char ch)
{
    switch(ch)
    {
        case '(':
            return 3;
        case '*':
        case '/':
            return 2;
        case '+':
        case '-':
            return 1;
        default:
            return 0;
    }
}
