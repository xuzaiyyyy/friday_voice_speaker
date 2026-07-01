#ifndef EXPRESSIONWINDOW_H
#define EXPRESSIONWINDOW_H

#include <QLabel>
#include <QEvent>
#include <QHBoxLayout>
#include <QPoint>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QWidget>

class QPushButton;

class ExpressionWindow : public QWidget
{
    Q_OBJECT

public:
    explicit ExpressionWindow(QWidget *parent = nullptr);

protected:
    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void showNextFrame();
    void switchEmojiSet();

private:
    void discoverEmojiSets();
    void loadFramesFromPaths(const QStringList &imagePaths);
    QPushButton *createToolbarButton(const QString &label);
    void addCommandButton(QHBoxLayout *layout, const QString &label, const QString &command);
    void addLiveCameraButton(QHBoxLayout *layout, const QString &label, const QString &command);
    void renderCurrentFrame();
    void enterDesktop();
    void openLiveCamera(const QString &title, const QString &command);

    QLabel *imageLabel_ = nullptr;
    QTimer *timer_ = nullptr;
    QTimer *switchTimer_ = nullptr;
    QVector<QStringList> emojiSets_;
    QVector<QPixmap> frames_;
    int emojiSetIndex_ = 0;
    int frameIndex_ = 0;
    QPoint pressPos_;
    QPoint lastTouchPos_;
    bool desktopEntered_ = false;
};

#endif // EXPRESSIONWINDOW_H
