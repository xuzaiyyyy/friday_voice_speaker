#ifndef MUSICPAGE_H
#define MUSICPAGE_H

#include <QWidget>
#include <mainwindow.h>
#include <QMediaPlayer>
#include <QMediaPlaylist>
#include <QListWidgetItem>
#include <QList>
#include <QTime>
#include <QDebug>

class QResizeEvent;
class QFrame;
class QLabel;
class QLineEdit;
class QProcess;
class QPushButton;

namespace Ui {
class MusicPage;
}

// 定义歌曲信息结构体
struct SongInfo {
    QString title;      // 歌名
    QString artist;     // 歌手 (用于分类)
    QString coverPath;  // 封面图片路径
    QString filePath;   // 本地音乐路径或在线音频 URL
};


class MusicPage : public QWidget
{
    Q_OBJECT

public:
    explicit MusicPage(QWidget *parent = nullptr);
    ~MusicPage();

private slots:
    // === UI 交互槽函数 ===
    void on_sideBar_itemClicked(QListWidgetItem *item);    // 点击左侧歌手
    void on_songListWidget_itemClicked(QListWidgetItem *item); // 点击右侧歌曲封面

    void on_music_back_clicked(); //返回

    void on_btnPrev_clicked(); //上一首

    void on_btnPlay_clicked(); //开始播放

    void on_btnNext_clicked(); // 下一首
    void on_sliderProgress_sliderMoved(int position); // 拖动进度条

    // === 播放器状态槽函数 ===
    void updatePosition(qint64 position);
    void updateDuration(qint64 duration);
    void onPlayerStateChanged(QMediaPlayer::State state);

    //监听播放列表切歌的信号
    void onPlaylistIndexChanged(int index);

    void on_sliderVolume_valueChanged(int value); // 滑块动 -> 调音量
    void onOnlineSearchRequested();
    void onOnlineResolverFinished(int exitCode);

private:
    void resizeEvent(QResizeEvent *event) override;

    Ui::MusicPage *ui;

    QMediaPlayer *m_player = nullptr;
    QMediaPlaylist *m_playlist = nullptr;
    QFrame *m_searchPanel = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QPushButton *m_searchButton = nullptr;
    QLabel *m_searchStatus = nullptr;
    QFrame *m_nowPlayingPanel = nullptr;
    QLabel *m_coverLarge = nullptr;
    QLabel *m_nowTitle = nullptr;
    QLabel *m_nowSubtitle = nullptr;
    QProcess *m_resolverProcess = nullptr;
    bool m_progressDragging = false;

    QList<SongInfo> m_allSongs;      // 数据库：存所有歌曲
    QList<SongInfo> m_onlineSongs;   // 在线搜索结果
    QList<SongInfo> m_currentList;   // 当前显示的歌曲列表 (用于播放索引映射)

    void initPlayer();  // 初始化播放器
    void initData();    // 模拟加载数据
    void refreshSongList(QString artist); // 根据歌手刷新右侧列表
    void connectControls();
    void initOnlineSearchUi();
    void initNowPlayingUi();
    void updateResponsiveLayout();
    void seekToSliderValue();
    void addPlaceholderItem(const QString &text);
    void playSongAtIndex(int index);
    void setMusicStatus(const QString &text);
    void updateCoverLabels(const QString &coverPath);
};

#endif // MUSICPAGE_H
