#include "desktopidle.h"

#include "expressionwindow.h"

#include <QApplication>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QCursor>
#include <QEvent>
#include <QFont>
#include <QList>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <QtGlobal>

#include <algorithm>

namespace
{
constexpr int kWindowReplaceCloseDelayMs = 180;

int idleTimeoutMs()
{
    bool ok = false;
    const int seconds = qEnvironmentVariableIntValue("XIAOMAN_DESKTOP_IDLE_SECONDS", &ok);
    return std::max(5, ok ? seconds : 30) * 1000;
}

bool desktopFullscreenEnabledImpl()
{
    if (qEnvironmentVariableIsSet("XIAOMAN_DESKTOP_FULLSCREEN"))
    {
        bool ok = false;
        return qEnvironmentVariableIntValue("XIAOMAN_DESKTOP_FULLSCREEN", &ok) != 0;
    }
    return true;
}

bool desktopBypassWindowManagerEnabled()
{
    if (qEnvironmentVariableIsSet("XIAOMAN_DESKTOP_BYPASS_WM"))
    {
        bool ok = false;
        return qEnvironmentVariableIntValue("XIAOMAN_DESKTOP_BYPASS_WM", &ok) != 0;
    }
    return desktopFullscreenEnabledImpl();
}

bool isActivityEvent(QEvent::Type type)
{
    switch (type)
    {
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
        return true;
    default:
        return false;
    }
}

bool isDesktopHomeWindow(QWidget *window)
{
    if (window == nullptr)
    {
        return false;
    }
    return QString::fromLatin1(window->metaObject()->className()) == QStringLiteral("MainWindow");
}

void prepareTouchWindow(QWidget *window)
{
    Qt::WindowFlags flags = window->windowFlags();
    flags |= Qt::FramelessWindowHint;
    flags |= Qt::WindowStaysOnTopHint;
    if (desktopBypassWindowManagerEnabled())
    {
        flags |= Qt::X11BypassWindowManagerHint;
    }
    window->setWindowFlags(flags);
    window->setCursor(Qt::BlankCursor);
    window->setAttribute(Qt::WA_AcceptTouchEvents, true);

    for (QAbstractButton *button : window->findChildren<QAbstractButton *>())
    {
        button->setCursor(Qt::BlankCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setAttribute(Qt::WA_AcceptTouchEvents, true);
        button->setMinimumSize(button->minimumSize().expandedTo(QSize(44, 38)));
    }

    for (QAbstractItemView *view : window->findChildren<QAbstractItemView *>())
    {
        view->setCursor(Qt::BlankCursor);
        view->setAttribute(Qt::WA_AcceptTouchEvents, true);
        view->setEditTriggers(QAbstractItemView::NoEditTriggers);
        view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    }
}

bool hasItemViewAncestor(QWidget *window, QWidget *child)
{
    QWidget *parent = child != nullptr ? child->parentWidget() : nullptr;
    while (parent != nullptr && parent != window)
    {
        if (qobject_cast<QAbstractItemView *>(parent) != nullptr)
        {
            return true;
        }
        parent = parent->parentWidget();
    }
    return false;
}

class FixedDesignScaler : public QObject
{
public:
    explicit FixedDesignScaler(QWidget *window)
        : QObject(window),
          window_(window)
    {
        capture();
        window_->installEventFilter(this);
        QTimer::singleShot(0, this, [this]
                           { apply(); });
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == window_ && event->type() == QEvent::Resize)
        {
            QTimer::singleShot(0, this, [this]
                               { apply(); });
        }
        return false;
    }

private:
    struct Item
    {
        QWidget *widget = nullptr;
        QRect geometry;
        int pointSize = -1;
    };

    void capture()
    {
        items_.clear();
        if (window_ == nullptr)
        {
            return;
        }

        const QList<QWidget *> children =
            window_->findChildren<QWidget *>(QString(), Qt::FindChildrenRecursively);
        for (QWidget *child : children)
        {
            if (child == nullptr || child->isWindow() ||
                child->objectName().startsWith(QStringLiteral("qt_")) ||
                hasItemViewAncestor(window_, child))
            {
                continue;
            }
            Item item;
            item.widget = child;
            item.geometry = child->geometry();
            item.pointSize = child->font().pointSize();
            items_.push_back(item);
        }
    }

    void apply()
    {
        if (window_ == nullptr || window_->width() <= 0 || window_->height() <= 0)
        {
            return;
        }

        const qreal sx = static_cast<qreal>(window_->width()) / 1024.0;
        const qreal sy = static_cast<qreal>(window_->height()) / 600.0;
        const qreal sizeScale = qMin(sx, sy);

        for (const Item &item : items_)
        {
            QWidget *child = item.widget;
            if (child == nullptr)
            {
                continue;
            }

            const QRect &original = item.geometry;
            child->setGeometry(qRound(original.x() * sx),
                               qRound(original.y() * sy),
                               qMax(12, qRound(original.width() * sx)),
                               qMax(10, qRound(original.height() * sy)));

            if (item.pointSize > 0)
            {
                QFont font = child->font();
                font.setPointSize(qMax(7, qRound(item.pointSize * sizeScale)));
                child->setFont(font);
            }
        }
    }

    QWidget *window_ = nullptr;
    QVector<Item> items_;
};

bool needsFixedDesignScaler(QWidget *window)
{
    if (window == nullptr)
    {
        return false;
    }

    const QString className = QString::fromLatin1(window->metaObject()->className());
    static const QStringList excluded{
        QStringLiteral("ExpressionWindow"),
        QStringLiteral("MainWindow"),
        QStringLiteral("calculate"),
        QStringLiteral("photo"),
        QStringLiteral("CameraPage"),
        QStringLiteral("LiveCameraWindow"),
        QStringLiteral("ServerPage"),
        QStringLiteral("sketchpad"),
        QStringLiteral("VideoPlayer")};
    return !excluded.contains(className);
}

void installFixedDesignScaler(QWidget *window)
{
    if (needsFixedDesignScaler(window) && !window->property("_xiaomanFixedDesignScaler").toBool())
    {
        window->setProperty("_xiaomanFixedDesignScaler", true);
        new FixedDesignScaler(window);
    }
}

class DesktopIdleReturn : public QObject
{
public:
    explicit DesktopIdleReturn(QObject *parent = nullptr)
        : QObject(parent)
    {
        timer_.setSingleShot(true);
        connect(&timer_, &QTimer::timeout, this, &DesktopIdleReturn::returnToExpression);
        qApp->installEventFilter(this);
    }

    void setEnabled(bool enabled)
    {
        enabled_ = enabled;
        if (enabled_)
        {
            restartTimer();
        }
        else
        {
            timer_.stop();
        }
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        Q_UNUSED(watched)
        if (enabled_ && isActivityEvent(event->type()))
        {
            restartTimer();
        }
        return false;
    }

private:
    void restartTimer()
    {
        timer_.start(idleTimeoutMs());
    }

    void returnToExpression()
    {
        if (!enabled_)
        {
            return;
        }
        enabled_ = false;
        timer_.stop();

        showExpressionWindowAndCloseOthers();
    }

    bool enabled_ = false;
    QTimer timer_;
};

DesktopIdleReturn *g_idleReturn = nullptr;
} // namespace

void installDesktopIdleReturn()
{
    if (g_idleReturn == nullptr)
    {
        g_idleReturn = new DesktopIdleReturn(qApp);
    }
}

void setDesktopIdleReturnEnabled(bool enabled)
{
    installDesktopIdleReturn();
    g_idleReturn->setEnabled(enabled);
}

bool desktopFullscreenEnabled()
{
    return desktopFullscreenEnabledImpl();
}

void showDesktopTopLevel(QWidget *window)
{
    if (window == nullptr)
    {
        return;
    }
    prepareTouchWindow(window);
    installFixedDesignScaler(window);
    if (desktopFullscreenEnabled())
    {
        if (desktopBypassWindowManagerEnabled() && QApplication::primaryScreen() != nullptr)
        {
            window->setGeometry(QApplication::primaryScreen()->geometry());
        }
        window->showFullScreen();
    }
    else
    {
        window->show();
    }
    window->raise();
    window->activateWindow();
    setDesktopIdleReturnEnabled(isDesktopHomeWindow(window));
}

void showVideoTopLevel(QWidget *window)
{
    if (window == nullptr)
    {
        return;
    }
    Qt::WindowFlags flags = window->windowFlags();
    flags |= Qt::FramelessWindowHint;
    flags |= Qt::WindowStaysOnTopHint;
    if (desktopBypassWindowManagerEnabled())
    {
        flags |= Qt::X11BypassWindowManagerHint;
    }
    window->setWindowFlags(flags);
    window->setCursor(Qt::BlankCursor);
    window->setAttribute(Qt::WA_AcceptTouchEvents, true);
    if (desktopFullscreenEnabled())
    {
        if (QApplication::primaryScreen() != nullptr)
        {
            window->setGeometry(QApplication::primaryScreen()->geometry());
        }
        window->showFullScreen();
    }
    else
    {
        window->show();
    }
    window->raise();
    window->activateWindow();
}

void replaceDesktopTopLevel(QWidget *current, QWidget *next)
{
    if (next == nullptr)
    {
        return;
    }
    next->setAttribute(Qt::WA_DeleteOnClose, true);
    showDesktopTopLevel(next);
    if (current != nullptr && current != next)
    {
        QPointer<QWidget> oldWindow(current);
        QTimer::singleShot(kWindowReplaceCloseDelayMs, next, [oldWindow]()
                           {
                               if (oldWindow)
                               {
                                   oldWindow->close();
                               }
                           });
    }
}

void showExpressionWindowAndCloseOthers()
{
    auto *expression = new ExpressionWindow();
    expression->setAttribute(Qt::WA_DeleteOnClose, true);
    showDesktopTopLevel(expression);

    const QList<QWidget *> windows = QApplication::topLevelWidgets();
    QList<QPointer<QWidget>> oldWindows;
    for (QWidget *window : windows)
    {
        if (window != expression && window != nullptr)
        {
            oldWindows.push_back(QPointer<QWidget>(window));
        }
    }
    QTimer::singleShot(kWindowReplaceCloseDelayMs, expression, [oldWindows, expression]()
                       {
                           for (const QPointer<QWidget> &window : oldWindows)
                           {
                               if (window && window != expression)
                               {
                                   window->close();
                               }
                           }
                       });
}
