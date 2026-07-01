#include "musicpage.h"
#include "ui_musicpage.h"
#include "apppaths.h"
#include "desktopidle.h"

#include <QPushButton>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegExp>
#include <QResizeEvent>
#include <QSlider>
#include <QUrl>
#include <QtGlobal>
#include <limits>

namespace
{
QString formatMs(qint64 value)
{
    if (value < 0)
    {
        value = 0;
    }
    QTime t(0, static_cast<int>((value / 60000) % 60), static_cast<int>((value / 1000) % 60));
    return t.toString("mm:ss");
}

void setImageButton(QPushButton *button, const QString &image)
{
    if (button == nullptr)
    {
        return;
    }
    button->setText(QString());
    button->setStyleSheet(QStringLiteral(
                              "QPushButton { image: url(%1); border: none; background-color: transparent; }"
                              "QPushButton:pressed { background-color: rgba(0, 122, 255, 35); border-radius: 8px; }")
                              .arg(image));
}

QString valueFromKeyLines(const QString &text, const QString &key)
{
    const QString prefix = key + QStringLiteral("=");
    const QStringList lines = text.split(QRegExp(QStringLiteral("[\r\n]+")), QString::SkipEmptyParts);
    for (const QString &rawLine : lines)
    {
        const QString line = rawLine.trimmed();
        if (line.startsWith(prefix))
        {
            return line.mid(prefix.size()).trimmed();
        }
    }
    return QString();
}

QString compactText(const QString &text, int maxChars)
{
    const QString trimmed = text.trimmed();
    if (trimmed.size() <= maxChars)
    {
        return trimmed;
    }
    return trimmed.left(qMax(1, maxChars - 1)) + QStringLiteral("…");
}

int intFromKeyLines(const QString &text, const QString &key, int fallback)
{
    bool ok = false;
    const int value = valueFromKeyLines(text, key).toInt(&ok);
    return ok ? value : fallback;
}

QString usableCoverPath(const QString &cover)
{
    const QString trimmed = cover.trimmed();
    if (trimmed.isEmpty() ||
        trimmed.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) ||
        trimmed.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive))
    {
        return QStringLiteral(":/images/music_icon.png");
    }
    if (trimmed.startsWith(QStringLiteral(":/")) || QFileInfo::exists(trimmed))
    {
        return trimmed;
    }
    return QStringLiteral(":/images/music_icon.png");
}

QUrl mediaUrlForSong(const SongInfo &song)
{
    if (song.filePath.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) ||
        song.filePath.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive))
    {
        return QUrl(song.filePath);
    }
    return QUrl::fromLocalFile(song.filePath);
}

QString playerStyleSheet()
{
    return QStringLiteral(
        "QWidget#MusicPage { background: #0B0F16; font-family: 'Microsoft YaHei'; }"
        "#sideBar { background: #121824; border: none; border-right: 1px solid #273043;"
        " padding-top: 6px; outline: none; }"
        "#sideBar::item { height: 36px; margin: 3px 6px; padding-left: 6px;"
        " border-radius: 9px; color: #AAB4C4; font-size: 13px; }"
        "#sideBar::item:selected { background: #22304A; color: #FFFFFF;"
        " border-left: 3px solid #46D9FF; }"
        "#sideBar::item:hover { background: #1B2435; color: #FFFFFF; }"
        "#listwidget { background: qlineargradient(x1:0, y1:0, x2:1, y2:1,"
        " stop:0 #182032, stop:0.55 #101722, stop:1 #070A10); }"
        "#playerBar { background: rgba(9, 12, 18, 222);"
        " border-bottom: 1px solid rgba(255,255,255,28); }"
        "#lblCoverSmall { background: #252E3D; border-radius: 8px;"
        " border: 1px solid rgba(255,255,255,35); }"
        "#lblSongTitle { color: #F5F7FB; font-size: 13px; font-weight: 600; }"
        "#lblCurrent, #lblTotal, #label { color: #B7C2D6; font-size: 11px; }"
        "#songListWidget { background: transparent; border: none; padding: 8px;"
        " color: #ECF2FF; outline: none; }"
        "#songListWidget::item { margin: 4px; padding: 6px; border-radius: 12px;"
        " color: #EAF0FA; background: rgba(255,255,255,10); }"
        "#songListWidget::item:selected { background: rgba(70,217,255,45);"
        " border: 1px solid rgba(70,217,255,190); color: #FFFFFF; }"
        "#songListWidget::item:hover { background: rgba(255,255,255,22); }"
        "#nowPlayingPanel { background: rgba(255,255,255,18); border-radius: 16px;"
        " border: 1px solid rgba(255,255,255,30); }"
        "#coverLarge { background: #202A3A; border-radius: 14px;"
        " border: 1px solid rgba(255,255,255,38); }"
        "#nowTitle { color: #FFFFFF; font-size: 18px; font-weight: 700; }"
        "#nowSubtitle { color: #AAB4C4; font-size: 12px; }"
        "QScrollBar:vertical { width: 8px; background: transparent; margin: 6px 0; }"
        "QScrollBar::handle:vertical { background: rgba(255,255,255,48);"
        " border-radius: 4px; min-height: 22px; }"
        "QScrollBar::handle:vertical:hover { background: rgba(255,255,255,82); }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"
        "QSlider::groove:horizontal { height: 6px; border-radius: 3px;"
        " background: rgba(255,255,255,48); }"
        "QSlider::sub-page:horizontal { height: 6px; border-radius: 3px;"
        " background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        " stop:0 #45E6FF, stop:1 #007AFF); }"
        "QSlider::handle:horizontal { width: 24px; height: 24px;"
        " margin: -9px 0; border-radius: 12px; background: #F7FBFF;"
        " border: 2px solid #45E6FF; }");
}
} // namespace

MusicPage::MusicPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::MusicPage)
{
    ui->setupUi(this);

    this->setWindowFlags(Qt::FramelessWindowHint);
    this->setAutoFillBackground(true);
    this->setStyleSheet(playerStyleSheet());

    ui->songListWidget->setViewMode(QListWidget::IconMode);
    ui->songListWidget->setIconSize(QSize(120, 120));
    ui->songListWidget->setSpacing(20);
    ui->songListWidget->setResizeMode(QListWidget::Adjust);
    ui->songListWidget->setMovement(QListWidget::Static);
    ui->songListWidget->setUniformItemSizes(true);
    ui->sliderProgress->setTracking(true);
    ui->sliderProgress->setMinimumHeight(44);
    ui->lblCurrent->setAlignment(Qt::AlignCenter);
    ui->lblTotal->setAlignment(Qt::AlignCenter);
    ui->label->hide();
    ui->sliderVolume->setTracking(true);
    ui->sliderVolume->setRange(0, 100);
    ui->sliderVolume->setMinimumHeight(44);

    initOnlineSearchUi();
    initNowPlayingUi();
    initPlayer();
    connectControls();
    initData();

    if(ui->sideBar->count() > 0) {
        ui->sideBar->setCurrentRow(0);
        refreshSongList(ui->sideBar->item(0)->text());
    }
    updateResponsiveLayout();
}

MusicPage::~MusicPage()
{
    if (m_resolverProcess != nullptr)
    {
        m_resolverProcess->kill();
        m_resolverProcess->deleteLater();
        m_resolverProcess = nullptr;
    }
    delete ui;
}

void MusicPage::on_music_back_clicked()
{
    m_player->stop();
    auto *w = new MainWindow();
    replaceDesktopTopLevel(this, w);
}

void MusicPage::initPlayer()
{
    m_player = new QMediaPlayer(this);
    m_playlist = new QMediaPlaylist(this);
    m_playlist->setPlaybackMode(QMediaPlaylist::Loop);
    m_player->setPlaylist(m_playlist);

    connect(m_player, &QMediaPlayer::positionChanged, this, &MusicPage::updatePosition);
    connect(m_player, &QMediaPlayer::durationChanged, this, &MusicPage::updateDuration);
    connect(m_player, &QMediaPlayer::stateChanged, this, &MusicPage::onPlayerStateChanged);

    connect(m_playlist, &QMediaPlaylist::currentIndexChanged,
            this, &MusicPage::onPlaylistIndexChanged);

    m_player->setVolume(50);
    ui->sliderVolume->setValue(50);
    ui->sliderProgress->setRange(0, 0);
    ui->lblCurrent->setText(QStringLiteral("00:00"));
    ui->lblTotal->setText(QStringLiteral("00:00"));
    onPlayerStateChanged(QMediaPlayer::StoppedState);
}

void MusicPage::connectControls()
{
    QObject::disconnect(ui->sideBar, nullptr, this, nullptr);
    QObject::disconnect(ui->songListWidget, nullptr, this, nullptr);
    QObject::disconnect(ui->music_back, nullptr, this, nullptr);
    QObject::disconnect(ui->btnPrev, nullptr, this, nullptr);
    QObject::disconnect(ui->btnPlay, nullptr, this, nullptr);
    QObject::disconnect(ui->btnNext, nullptr, this, nullptr);
    QObject::disconnect(ui->sliderProgress, nullptr, this, nullptr);
    QObject::disconnect(ui->sliderVolume, nullptr, this, nullptr);
    if (m_searchButton != nullptr)
    {
        QObject::disconnect(m_searchButton, nullptr, this, nullptr);
    }
    if (m_searchEdit != nullptr)
    {
        QObject::disconnect(m_searchEdit, nullptr, this, nullptr);
    }

    connect(ui->sideBar, &QListWidget::itemClicked,
            this, &MusicPage::on_sideBar_itemClicked);
    connect(ui->songListWidget, &QListWidget::itemClicked,
            this, &MusicPage::on_songListWidget_itemClicked);
    connect(ui->music_back, &QPushButton::clicked,
            this, &MusicPage::on_music_back_clicked);
    connect(ui->btnPrev, &QPushButton::clicked,
            this, &MusicPage::on_btnPrev_clicked);
    connect(ui->btnPlay, &QPushButton::clicked,
            this, &MusicPage::on_btnPlay_clicked);
    connect(ui->btnNext, &QPushButton::clicked,
            this, &MusicPage::on_btnNext_clicked);
    connect(ui->sliderProgress, &QSlider::sliderMoved,
            this, &MusicPage::on_sliderProgress_sliderMoved);
    connect(ui->sliderProgress, &QSlider::sliderPressed, this, [this]() {
        m_progressDragging = true;
    });
    connect(ui->sliderProgress, &QSlider::sliderReleased, this, [this]() {
        seekToSliderValue();
        m_progressDragging = false;
    });
    connect(ui->sliderVolume, &QSlider::valueChanged,
            this, &MusicPage::on_sliderVolume_valueChanged);
    if (m_searchButton != nullptr)
    {
        connect(m_searchButton, &QPushButton::clicked,
                this, &MusicPage::onOnlineSearchRequested);
    }
    if (m_searchEdit != nullptr)
    {
        connect(m_searchEdit, &QLineEdit::returnPressed,
                this, &MusicPage::onOnlineSearchRequested);
    }
}

void MusicPage::on_sliderVolume_valueChanged(int value)
{
    if (m_player)
    {
        m_player->setVolume(qBound(0, value, 100));
    }
}

void MusicPage::initOnlineSearchUi()
{
    m_searchPanel = new QFrame(ui->listwidget);
    m_searchPanel->setObjectName(QStringLiteral("onlineSearchPanel"));
    m_searchPanel->setStyleSheet(QStringLiteral(
        "#onlineSearchPanel { background-color: rgba(13, 18, 28, 205);"
        " border-bottom: 1px solid rgba(255,255,255,24); }"));

    m_searchEdit = new QLineEdit(m_searchPanel);
    m_searchEdit->setPlaceholderText(QStringLiteral("搜索歌曲 / 歌手"));
    m_searchEdit->setClearButtonEnabled(false);
    m_searchEdit->setStyleSheet(QStringLiteral(
        "QLineEdit { background: rgba(255,255,255,22); border: 1px solid rgba(255,255,255,48);"
        " border-radius: 10px; padding: 5px 9px; color: #F5F7FB; font-size: 13px; }"
        "QLineEdit:focus { border-color: #45E6FF; background: rgba(255,255,255,32); }"));

    m_searchButton = new QPushButton(QStringLiteral("搜索"), m_searchPanel);
    m_searchButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: #007AFF; color: white; border: none; border-radius: 10px;"
        " font-size: 13px; font-weight: 600; }"
        "QPushButton:pressed { background: #005FCC; }"
        "QPushButton:disabled { background: #3B4658; color: #AAB4C4; }"));

    m_searchStatus = new QLabel(QStringLiteral("在线音源：使用项目配置的 music provider"), m_searchPanel);
    m_searchStatus->setStyleSheet(QStringLiteral("QLabel { color: #9DA9BA; font-size: 11px; }"));
    m_searchStatus->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
}

void MusicPage::initNowPlayingUi()
{
    m_nowPlayingPanel = new QFrame(ui->listwidget);
    m_nowPlayingPanel->setObjectName(QStringLiteral("nowPlayingPanel"));

    m_coverLarge = new QLabel(m_nowPlayingPanel);
    m_coverLarge->setObjectName(QStringLiteral("coverLarge"));
    m_coverLarge->setAlignment(Qt::AlignCenter);
    m_coverLarge->setScaledContents(true);

    m_nowTitle = new QLabel(QStringLiteral("在线音乐"), m_nowPlayingPanel);
    m_nowTitle->setObjectName(QStringLiteral("nowTitle"));
    m_nowTitle->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_nowTitle->setWordWrap(true);

    m_nowSubtitle = new QLabel(QStringLiteral("搜索歌曲，或从本地歌单选择播放"), m_nowPlayingPanel);
    m_nowSubtitle->setObjectName(QStringLiteral("nowSubtitle"));
    m_nowSubtitle->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_nowSubtitle->setWordWrap(true);

    updateCoverLabels(QStringLiteral(":/images/music_icon.png"));
}

void MusicPage::setMusicStatus(const QString &text)
{
    if (m_searchStatus != nullptr)
    {
        m_searchStatus->setText(compactText(text, 24));
    }
    if (m_nowSubtitle != nullptr)
    {
        m_nowSubtitle->setText(compactText(text, 36));
    }
    qInfo() << "music page:" << text;
}

void MusicPage::updateCoverLabels(const QString &coverPath)
{
    QPixmap pix(coverPath);
    if (pix.isNull())
    {
        pix = QPixmap(QStringLiteral(":/images/music_icon.png"));
    }
    if (!pix.isNull())
    {
        ui->lblCoverSmall->setPixmap(pix);
        ui->lblCoverSmall->setScaledContents(true);
        if (m_coverLarge != nullptr)
        {
            m_coverLarge->setPixmap(pix);
            m_coverLarge->setScaledContents(true);
        }
    }
}

void MusicPage::addPlaceholderItem(const QString &text)
{
    QListWidgetItem *item = new QListWidgetItem(text);
    item->setTextAlignment(Qt::AlignCenter);
    item->setFlags(Qt::NoItemFlags);
    ui->songListWidget->addItem(item);
}

void MusicPage::playSongAtIndex(int index)
{
    if(index >= 0 && index < m_playlist->mediaCount()) {
        m_playlist->setCurrentIndex(index);
        m_player->play();
    }
}

void MusicPage::onOnlineSearchRequested()
{
    if (m_searchEdit == nullptr)
    {
        return;
    }

    const QString query = m_searchEdit->text().trimmed();
    if (query.isEmpty())
    {
        setMusicStatus(QStringLiteral("请输入歌名或歌手"));
        return;
    }

    const QString helper = AppPaths::projectFile(QStringLiteral("xunfei_llm_chat.py"));
    if (!QFileInfo::exists(helper))
    {
        setMusicStatus(QStringLiteral("找不到音乐解析脚本"));
        return;
    }

    if (m_resolverProcess != nullptr)
    {
        m_resolverProcess->kill();
        m_resolverProcess->deleteLater();
        m_resolverProcess = nullptr;
    }

    setMusicStatus(QStringLiteral("正在搜索：%1").arg(query));
    if (m_searchButton != nullptr)
    {
        m_searchButton->setEnabled(false);
        m_searchButton->setText(QStringLiteral("搜索中"));
    }

    QProcess *process = new QProcess(this);
    m_resolverProcess = process;
    process->setWorkingDirectory(AppPaths::projectRoot());

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("XIAOMAN_CONFIG_FILE"),
               AppPaths::projectFile(QStringLiteral("friday_voice_speaker.conf")));
    env.insert(QStringLiteral("NEWBOT_CONFIG_FILE"),
               AppPaths::projectFile(QStringLiteral("friday_voice_speaker.conf")));
    process->setProcessEnvironment(env);

    connect(process, &QProcess::started, this, [process, query]() {
        const QByteArray input = QStringLiteral("播放 %1").arg(query).toUtf8();
        process->write(input);
        process->closeWriteChannel();
    });
    connect(process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus) {
                onOnlineResolverFinished(exitCode);
            });
    connect(process,
            &QProcess::errorOccurred,
            this,
            [this](QProcess::ProcessError) {
                setMusicStatus(QStringLiteral("音乐解析进程启动失败"));
                if (m_searchButton != nullptr)
                {
                    m_searchButton->setEnabled(true);
                    m_searchButton->setText(QStringLiteral("搜索"));
                }
            });

    const QString configuredPython = QString::fromLocal8Bit(qgetenv("NEWBOT_PYTHON")).trimmed();
    const QString python = configuredPython.isEmpty() ? QStringLiteral("python3") : configuredPython;
    process->start(python, QStringList() << helper << QStringLiteral("--music-url"));
}

void MusicPage::onOnlineResolverFinished(int exitCode)
{
    QProcess *process = qobject_cast<QProcess *>(sender());
    if (process == nullptr)
    {
        process = m_resolverProcess;
    }
    if (process == nullptr)
    {
        return;
    }

    const QString stdoutText = QString::fromUtf8(process->readAllStandardOutput());
    const QString stderrText = QString::fromUtf8(process->readAllStandardError()).trimmed();
    if (process == m_resolverProcess)
    {
        m_resolverProcess = nullptr;
    }
    process->deleteLater();

    if (m_searchButton != nullptr)
    {
        m_searchButton->setEnabled(true);
        m_searchButton->setText(QStringLiteral("搜索"));
    }

    const QString error = valueFromKeyLines(stdoutText, QStringLiteral("MUSIC_ERROR"));

    QList<SongInfo> resolvedSongs;
    const int count = qMax(0, intFromKeyLines(stdoutText, QStringLiteral("MUSIC_COUNT"), 0));
    for (int i = 0; i < count; ++i)
    {
        const QString prefix = QStringLiteral("MUSIC_%1_").arg(i);
        const QString url = valueFromKeyLines(stdoutText, prefix + QStringLiteral("URL"));
        if (url.isEmpty())
        {
            continue;
        }
        SongInfo song;
        song.title = valueFromKeyLines(stdoutText, prefix + QStringLiteral("TITLE"));
        song.artist = valueFromKeyLines(stdoutText, prefix + QStringLiteral("ARTIST"));
        song.coverPath = usableCoverPath(valueFromKeyLines(stdoutText, prefix + QStringLiteral("COVER")));
        song.filePath = url;
        if (song.title.isEmpty())
        {
            song.title = QStringLiteral("在线歌曲");
        }
        if (song.artist.isEmpty())
        {
            song.artist = QStringLiteral("在线音源");
        }
        resolvedSongs.append(song);
    }

    if (resolvedSongs.isEmpty())
    {
        const QString url = valueFromKeyLines(stdoutText, QStringLiteral("MUSIC_URL"));
        if (!url.isEmpty())
        {
            SongInfo song;
            song.title = valueFromKeyLines(stdoutText, QStringLiteral("MUSIC_TITLE"));
            song.artist = valueFromKeyLines(stdoutText, QStringLiteral("MUSIC_ARTIST"));
            song.coverPath = usableCoverPath(valueFromKeyLines(stdoutText, QStringLiteral("MUSIC_COVER")));
            song.filePath = url;
            if (song.title.isEmpty())
            {
                song.title = QStringLiteral("在线歌曲");
            }
            if (song.artist.isEmpty())
            {
                song.artist = QStringLiteral("在线音源");
            }
            resolvedSongs.append(song);
        }
    }

    if (exitCode != 0 || resolvedSongs.isEmpty())
    {
        const QString message = !error.isEmpty()
                                    ? error
                                    : (!stderrText.isEmpty() ? stderrText : QStringLiteral("没有找到可播放音源"));
        setMusicStatus(QStringLiteral("搜索失败：%1").arg(message.left(80)));
        return;
    }

    m_onlineSongs = resolvedSongs;

    for (int i = 0; i < ui->sideBar->count(); ++i)
    {
        if (ui->sideBar->item(i)->text() == QStringLiteral("在线"))
        {
            ui->sideBar->setCurrentRow(i);
            break;
        }
    }
    refreshSongList(QStringLiteral("在线"));
    playSongAtIndex(0);
    setMusicStatus(QStringLiteral("找到 %1 首，正在播放：%2 - %3")
                       .arg(m_onlineSongs.size())
                       .arg(m_onlineSongs.first().title)
                       .arg(m_onlineSongs.first().artist));
}


// === 1. 模拟数据 (请修改为你真实的 MP3 路径) ===
// === 核心：加载数据 ===
void MusicPage::initData()
{
    bool hasOnlineItem = false;
    for (int i = 0; i < ui->sideBar->count(); ++i)
    {
        if (ui->sideBar->item(i)->text() == QStringLiteral("在线"))
        {
            hasOnlineItem = true;
            break;
        }
    }
    if (!hasOnlineItem)
    {
        ui->sideBar->insertItem(0, QStringLiteral("在线"));
    }

    // 添加数据 (请确保你的 music 和 page 文件夹里真有这些文件)
    // 格式: { 歌名, 歌手, 封面路径, 音乐路径 }

    // --- 周杰伦 ---
    m_allSongs.append({"等你下课", "周杰伦",
                       AppPaths::existingDesktopFile("page/jay_cover1.jpg"),
                       AppPaths::existingDesktopFile("music/xiake.flac")}); // 支持 .mp3 和 .mp4
    m_allSongs.append({"我是如此相信", "周杰伦",
                       AppPaths::existingDesktopFile("page/jay_cover2.jpg"),
                       AppPaths::existingDesktopFile("music/xiangxin.flac")});
}

// === 2. 核心：点击左侧歌手，刷新右侧列表 ===
void MusicPage::on_sideBar_itemClicked(QListWidgetItem *item)
{
    QString artist = item->text(); // 获取点击的歌手名字 (如 "周杰伦")
    refreshSongList(artist);
}

void MusicPage::refreshSongList(QString artist)
{
    ui->songListWidget->clear();
    m_currentList.clear();
    m_playlist->clear();

    const bool onlineCategory = (artist == QStringLiteral("在线"));
    const QList<SongInfo> &source = onlineCategory ? m_onlineSongs : m_allSongs;
    for(const SongInfo &song : source) {
        if(onlineCategory || song.artist == artist) {
            QListWidgetItem *item = new QListWidgetItem(QIcon(song.coverPath), song.title);
            item->setTextAlignment(Qt::AlignHCenter | Qt::AlignBottom);
            ui->songListWidget->addItem(item);

            m_currentList.append(song);
            m_playlist->addMedia(mediaUrlForSong(song));
        }
    }
    if (m_currentList.isEmpty())
    {
        addPlaceholderItem(onlineCategory
                               ? QStringLiteral("输入歌名，然后点搜索")
                               : QStringLiteral("这个分类暂时没有歌曲"));
    }
    ui->sliderProgress->setRange(0, 0);
    ui->sliderProgress->setValue(0);
    ui->lblCurrent->setText(QStringLiteral("00:00"));
    ui->lblTotal->setText(QStringLiteral("00:00"));
}

// === 3. 点击右侧封面 -> 播放 ===
void MusicPage::on_songListWidget_itemClicked(QListWidgetItem *item)
{
    int index = ui->songListWidget->row(item);
    playSongAtIndex(index);
}



void MusicPage::on_btnPrev_clicked()
{
    if (!m_playlist || m_playlist->mediaCount() <= 0)
    {
        return;
    }
    if (m_playlist->currentIndex() < 0)
    {
        m_playlist->setCurrentIndex(0);
    }
    else
    {
        m_playlist->previous();
    }
    m_player->play();
}

void MusicPage::on_btnPlay_clicked()
{
    if (!m_player || !m_playlist)
    {
        return;
    }
    if(m_player->state() == QMediaPlayer::PlayingState) {
        m_player->pause();
    } else {
        if(m_playlist->mediaCount() > 0) {
            if (m_playlist->currentIndex() < 0)
            {
                m_playlist->setCurrentIndex(0);
            }
            m_player->play();
        }
    }
}

void MusicPage::on_btnNext_clicked()
{
    if (!m_playlist || m_playlist->mediaCount() <= 0)
    {
        return;
    }
    if (m_playlist->currentIndex() < 0)
    {
        m_playlist->setCurrentIndex(0);
    }
    else
    {
        m_playlist->next();
    }
    m_player->play();
}


// === 5. 进度条与状态更新 ===
void MusicPage::updateDuration(qint64 duration)
{
    const int sliderMax = static_cast<int>(qMin<qint64>(duration, std::numeric_limits<int>::max()));
    ui->sliderProgress->setRange(0, qMax(0, sliderMax));
    ui->lblTotal->setText(formatMs(duration));
}

void MusicPage::updatePosition(qint64 position)
{
    if(!m_progressDragging && !ui->sliderProgress->isSliderDown()) {
        ui->sliderProgress->setValue(static_cast<int>(qMin<qint64>(position, std::numeric_limits<int>::max())));
    }
    ui->lblCurrent->setText(formatMs(position));
}

void MusicPage::on_sliderProgress_sliderMoved(int position)
{
    ui->lblCurrent->setText(formatMs(position));
}

void MusicPage::seekToSliderValue()
{
    if (m_player)
    {
        m_player->setPosition(ui->sliderProgress->value());
    }
}

void MusicPage::onPlayerStateChanged(QMediaPlayer::State state)
{
    ui->btnPlay->setText("");

    if(state == QMediaPlayer::PlayingState) {
        setImageButton(ui->btnPlay, QStringLiteral(":/images/start.svg"));
    } else {
        setImageButton(ui->btnPlay, QStringLiteral(":/images/pause.svg"));
    }
}

// 只要切歌，这个函数就会瞬间触发，更新UI
void MusicPage::onPlaylistIndexChanged(int index)
{
    if(index < 0 || index >= m_currentList.size()) {
        return;
    }

    SongInfo song = m_currentList[index];

    ui->lblSongTitle->setText(song.title + " - " + song.artist);
    if (m_nowTitle != nullptr)
    {
        m_nowTitle->setText(song.title);
    }
    if (m_nowSubtitle != nullptr)
    {
        m_nowSubtitle->setText(song.artist);
    }
    ui->songListWidget->setCurrentRow(index);

    updateCoverLabels(song.coverPath);
}

void MusicPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateResponsiveLayout();
}

void MusicPage::updateResponsiveLayout()
{
    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0)
    {
        return;
    }

    const int shortSide = qMin(w, h);
    const int margin = qBound(5, shortSide / 36, 10);
    const int sideW = qBound(58, w / 8, 88);
    const int contentW = qMax(1, w - sideW);
    const int playerH = qBound(86, h / 5, 108);
    const int searchH = qBound(60, h / 7, 74);
    const int nowH = qBound(72, h / 6, 98);
    const int bottomH = qBound(42, h / 10, 56);

    ui->sideBar->setGeometry(0, 0, sideW, h);
    ui->listwidget->setGeometry(sideW, 0, contentW, h);
    ui->playerBar->setGeometry(0, 0, contentW, playerH);

    const int topRowH = qBound(38, playerH / 2, 48);
    const int cover = qBound(34, topRowH, 46);
    ui->lblCoverSmall->setGeometry(margin, margin, cover, cover);

    const int btn = qBound(32, topRowH, 42);
    const int controlsW = btn * 3 + margin * 2;
    const int titleX = margin + cover + margin;
    const int controlsX = qMax(titleX + 40 + margin, contentW - margin - controlsW);
    const int titleW = qMax(40, controlsX - titleX - margin);
    ui->lblSongTitle->setGeometry(titleX, margin, titleW, cover);
    ui->lblSongTitle->setWordWrap(true);

    int x = controlsX;
    ui->btnPrev->setGeometry(x, margin, btn, btn);
    x += btn + margin;
    ui->btnPlay->setGeometry(x, margin, btn, btn);
    x += btn + margin;
    ui->btnNext->setGeometry(x, margin, btn, btn);

    const int timeW = qBound(34, contentW / 14, 48);
    const int progressY = margin + cover + qMax(2, margin / 2);
    const int progressH = qMax(28, playerH - progressY - margin);
    ui->lblCurrent->setGeometry(margin, progressY, timeW, progressH);
    ui->lblTotal->setGeometry(contentW - margin - timeW, progressY, timeW, progressH);
    ui->label->hide();
    ui->sliderProgress->setGeometry(margin + timeW + margin,
                                    progressY + progressH / 2 - 22,
                                    qMax(80, contentW - timeW * 2 - margin * 4 - 10),
                                    44);

    if (m_searchPanel != nullptr)
    {
        m_searchPanel->setGeometry(0, playerH, contentW, searchH);
        const int searchButtonW = qBound(56, contentW / 7, 78);
        const int fieldH = qBound(28, searchH / 2, 34);
        const int statusH = qMax(18, searchH - fieldH - margin * 2);
        const int editW = qMax(110, contentW - margin * 3 - searchButtonW);
        m_searchEdit->setGeometry(margin, margin, editW, fieldH);
        m_searchButton->setGeometry(margin + editW + margin, margin,
                                    searchButtonW, fieldH);
        m_searchStatus->setGeometry(margin, margin + fieldH,
                                    qMax(40, contentW - margin * 2),
                                    statusH);
    }

    const int nowY = playerH + searchH;
    if (m_nowPlayingPanel != nullptr)
    {
        m_nowPlayingPanel->setGeometry(margin, nowY + margin,
                                       qMax(1, contentW - margin * 2),
                                       qMax(1, nowH - margin));
        const int panelW = m_nowPlayingPanel->width();
        const int panelH = m_nowPlayingPanel->height();
        const int panelPad = qBound(8, panelH / 8, 14);
        const int largeCover = qBound(52, panelH - panelPad * 2, 92);
        m_coverLarge->setGeometry(panelPad, panelPad, largeCover, largeCover);
        const int textX = panelPad + largeCover + panelPad;
        const int textW = qMax(80, panelW - textX - panelPad);
        m_nowTitle->setGeometry(textX, panelPad, textW, qMax(28, panelH / 2 - panelPad));
        m_nowSubtitle->setGeometry(textX, panelPad + panelH / 2 - 2,
                                   textW, qMax(24, panelH / 2 - panelPad));
    }

    const int listTop = playerH + searchH + nowH;
    const int listHeight = qMax(1, h - playerH - searchH - nowH - bottomH);
    ui->songListWidget->setGeometry(0, listTop, contentW, listHeight);
    const int iconSize = qBound(70, shortSide / 4, 120);
    ui->songListWidget->setIconSize(QSize(iconSize, iconSize));
    ui->songListWidget->setGridSize(QSize(iconSize + margin * 2, iconSize + 34));
    ui->songListWidget->setSpacing(qBound(6, margin, 14));

    const int bottomY = h - bottomH;
    const int bottomBtn = qBound(34, bottomH - margin, 46);
    ui->btnVolume->setGeometry(margin, bottomY + (bottomH - bottomBtn) / 2, bottomBtn, bottomBtn);
    const int backSize = bottomBtn;
    ui->music_back->setGeometry(contentW - margin - backSize,
                                bottomY + (bottomH - backSize) / 2,
                                backSize, backSize);
    const int volumeX = margin + bottomBtn + margin;
    const int volumeW = qMax(80, contentW - volumeX - backSize - margin * 3);
    ui->sliderVolume->setGeometry(volumeX, bottomY + bottomH / 2 - 22, volumeW, 44);

    setImageButton(ui->btnPrev, QStringLiteral(":/images/previous.svg"));
    setImageButton(ui->btnNext, QStringLiteral(":/images/next.svg"));
    setImageButton(ui->btnVolume, QStringLiteral(":/images/volume.svg"));
    ui->music_back->setStyleSheet(QStringLiteral(
        "QPushButton { image: url(:/images/back.svg); border: none; background-color: transparent; }"
        "QPushButton:pressed { background-color: rgba(0, 122, 255, 35); border-radius: 8px; }"));
}
