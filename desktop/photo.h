#ifndef PHOTO_H
#define PHOTO_H

#include <QWidget>
#include <QScroller>
#include <QDebug>
#include <QFileInfoList>
#include <QDir>
#include <QScrollerProperties>
#include <QListWidgetItem>
#include <mainwindow.h>
#include <QFile>
#include <QPointer>
#include <videoplayer.h>

class QResizeEvent;

namespace Ui {
class photo;
}

class photo : public QWidget
{
    Q_OBJECT

public:
    explicit photo(QWidget *parent = nullptr);
    ~photo();

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void on_list_menu_itemClicked(QListWidgetItem *item);

    void on_photo_back_clicked();

    void on_photo_back_2_clicked();
    void on_list_photos_itemDoubleClicked(QListWidgetItem *item);

private:
    void loadPhotos();
    void loadVideos();
    void updateResponsiveLayout();


private:
    Ui::photo *ui;
    QPointer<VideoPlayer> activeVideo;
};

#endif // PHOTO_H
