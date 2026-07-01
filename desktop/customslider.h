#ifndef CUSTOMSLIDER_H
#define CUSTOMSLIDER_H

#include <QSlider>
#include <QMouseEvent>

class CustomSlider : public QSlider
{
    Q_OBJECT
public:
    explicit CustomSlider(QWidget *parent = nullptr);

protected:
    bool event(QEvent *event) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void mouseMoveEvent(QMouseEvent *ev) override;
    void mouseReleaseEvent(QMouseEvent *ev) override;

private:
    int valueFromPoint(const QPoint &pos) const;
    void updateValueFromPoint(const QPoint &pos);
};

#endif // CUSTOMSLIDER_H
