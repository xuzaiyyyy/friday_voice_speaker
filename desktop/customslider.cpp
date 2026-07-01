#include "customslider.h"

#include <QEvent>
#include <QStyle>
#include <QTouchEvent>
#include <QtGlobal>

CustomSlider::CustomSlider(QWidget *parent) : QSlider(parent)
{
    setTracking(true);
    setMouseTracking(true);
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setMinimumHeight(qMax(minimumHeight(), 44));
    setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal { height: 8px; border-radius: 4px; background: rgba(0,0,0,45); }"
        "QSlider::sub-page:horizontal { height: 8px; border-radius: 4px; background: #22d3ee; }"
        "QSlider::handle:horizontal { width: 30px; height: 30px; margin: -11px 0; border-radius: 15px;"
        " background: white; border: 2px solid #22d3ee; }"));
}

int CustomSlider::valueFromPoint(const QPoint &pos) const
{
    const bool horizontal = orientation() == Qt::Horizontal;
    const int span = qMax(1, horizontal ? width() : height());
    const int rawPixelPos = horizontal ? pos.x() : height() - pos.y();
    const int pixelPos = qBound(0, rawPixelPos, span);
    const bool upsideDown = horizontal ? invertedAppearance()
                                       : !invertedAppearance();

    return QStyle::sliderValueFromPosition(minimum(), maximum(), pixelPos, span, upsideDown);
}

void CustomSlider::updateValueFromPoint(const QPoint &pos)
{
    const int value = valueFromPoint(pos);
    setSliderPosition(value);
    setValue(value);
    emit sliderMoved(value);
}

bool CustomSlider::event(QEvent *event)
{
    switch (event->type())
    {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    {
        auto *touchEvent = static_cast<QTouchEvent *>(event);
        if (touchEvent->touchPoints().isEmpty())
        {
            break;
        }

        if (event->type() == QEvent::TouchBegin)
        {
            setSliderDown(true);
        }

        updateValueFromPoint(touchEvent->touchPoints().first().pos().toPoint());

        if (event->type() == QEvent::TouchEnd)
        {
            setSliderDown(false);
        }

        event->accept();
        return true;
    }
    default:
        break;
    }

    return QSlider::event(event);
}

void CustomSlider::mousePressEvent(QMouseEvent *ev)
{
    if (ev->button() == Qt::LeftButton)
    {
        setSliderDown(true);
        updateValueFromPoint(ev->pos());
        ev->accept();
        return;
    }
    QSlider::mousePressEvent(ev);
}

void CustomSlider::mouseMoveEvent(QMouseEvent *ev)
{
    if (isSliderDown() || (ev->buttons() & Qt::LeftButton))
    {
        updateValueFromPoint(ev->pos());
        ev->accept();
        return;
    }
    QSlider::mouseMoveEvent(ev);
}

void CustomSlider::mouseReleaseEvent(QMouseEvent *ev)
{
    if (ev->button() == Qt::LeftButton)
    {
        updateValueFromPoint(ev->pos());
        setSliderDown(false);
        ev->accept();
        return;
    }
    QSlider::mouseReleaseEvent(ev);
}
