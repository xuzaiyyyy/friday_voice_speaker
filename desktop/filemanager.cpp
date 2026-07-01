#include "filemanager.h"
#include "ui_filemanager.h"
#include "desktopidle.h"

FileManager::FileManager(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::FileManager)
{
    ui->setupUi(this);

    this->setWindowFlags(Qt::FramelessWindowHint);

    initSideBar(); // 初始化仿 Ubuntu 侧边栏
    initUI();

    // 默认打开“主目录”
    loadFiles(QDir::homePath());
}

FileManager::~FileManager()
{
    delete ui;
}


void FileManager::on_btnGo_clicked()
{
    // 1. 获取输入框里的文字
    QString targetPath = ui->linePath->text();

    // 2. 简单的去空格处理 (防止用户不小心多输了空格)
    targetPath = targetPath.trimmed();

    // 3. 尝试加载
    QDir dir(targetPath);
    if (dir.exists()) {
        loadFiles(targetPath);
    } else {
        // 路径错误的反馈：可以让输入框变红一下，或者弹窗
        qDebug() << "无效路径";
        // 可以在这里加一个 QMessageBox::warning(this, "错误", "路径不存在");

        // 这是一个小技巧：路径错误时，把输入框文字变红提醒用户
        ui->linePath->setStyleSheet("color: red;");
        // 1秒后恢复黑色
        QTimer::singleShot(1000, [=](){
            ui->linePath->setStyleSheet("color: black;");
        });
    }
}


// 在输入框按“回车键” -> 直接触发跳转
void FileManager::on_linePath_returnPressed()
{
    on_btnGo_clicked();
}


void FileManager::on_btnBack_clicked()
{
    if (m_currentDir.cdUp()) {
        loadFiles(m_currentDir.absolutePath());
    }
}


// ==========================================
//              UI 初始化与设置
// ==========================================
void FileManager::initUI()
{
    // 基础样式设置
    ui->fileList->setViewMode(QListWidget::IconMode);
    ui->fileList->setGridSize(QSize(100, 120));
    ui->fileList->setIconSize(QSize(60, 60));
    ui->fileList->setWordWrap(true);
    ui->fileList->setTextElideMode(Qt::ElideMiddle);
    ui->fileList->setResizeMode(QListWidget::Adjust);
    ui->fileList->setMovement(QListWidget::Static);
    ui->fileList->setSpacing(10);

    // =======================================================
    // 🔴 重点修复：开启右键菜单策略 (没有这行右键就没反应)
    // =======================================================
    ui->fileList->setContextMenuPolicy(Qt::CustomContextMenu);

    // 触摸屏惯性滚动
    ui->fileList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    QScroller::grabGesture(ui->fileList, QScroller::LeftMouseButtonGesture);
}


// === 1. 初始化侧边栏 (仿 Ubuntu 风格) ===
void FileManager::initSideBar()
{
    ui->sideBar->clear();

    // 1. 主目录 (Home)
    addSideItem("主目录", ":/images/home.png", QDir::homePath());

    // 2. 桌面 (Desktop)
    QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    addSideItem("桌面", ":/images/desktop.png", desktop);

    // 3. 视频 (Movies)
    QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    addSideItem("视频", ":/images/movies.png", movies);

    // 4. 图片 (Pictures)
    QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    addSideItem("图片", ":/images/pictures.png", pictures);

    // 5. 文档 (Documents)
    QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    addSideItem("文档", ":/images/documents.png", documents);

    // 6. 下载 (Download)
    QString download = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    addSideItem("下载", ":/images/downloads.png", download);

    // 7. 音乐 (Music)
    QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    addSideItem("音乐", ":/images/music.png", music);

    // --- 【新增】回收站 ---
    // 我们使用 Linux 标准回收站路径，或者自己建一个隐藏文件夹
    QString trashPath = getTrashPath();
    // 确保回收站文件夹存在，不存在就创建
    QDir dir(trashPath);
    if (!dir.exists()) dir.mkpath(trashPath);

    addSideItem("回收站", ":/images/trash.png", trashPath);

    // 1. 设置图标模式
    ui->fileList->setViewMode(QListWidget::IconMode);

    // 2. 【核心】设置每个格子的固定大小 (宽, 高)
    // 宽度 100 保证能放下较长的字，高度 120 给文字留出空间
    ui->fileList->setGridSize(QSize(100, 120));

    // 3. 设置图标本身的大小 (要比格子小，留出空隙)
    ui->fileList->setIconSize(QSize(60, 60));

    // 4. 设置自动换行和省略号模式
    // setWordWrap(false) 会强制单行显示，超出的部分显示 "..."
    // setWordWrap(true) 会换行显示，但如果太长还是会切断
    ui->fileList->setWordWrap(true);
    ui->fileList->setTextElideMode(Qt::ElideMiddle); // 省略号在中间 "file...name.txt"

    // 5. 调整间距和布局模式
    ui->fileList->setResizeMode(QListWidget::Adjust);
    ui->fileList->setMovement(QListWidget::Static); // 禁止拖拽移动
    ui->fileList->setSpacing(10);



    // === 设置样式表 (仿 Ubuntu 深色侧边栏或浅色侧边栏) ===
    ui->sideBar->setStyleSheet(
        "QListWidget { background-color: #f0f0f0; border: none; outline: none; font-size: 14px; }"
        "QListWidget::item { height: 40px; padding-left: 10px; color: #333; border: none; }"
        "QListWidget::item:hover { background-color: #e0e0e0; }"
        // 选中时：左侧加一个橙色竖条感觉 (仿 Ubuntu 主题色)
        "QListWidget::item:selected { background-color: #dedede; color: #E95420; border-left: 4px solid #E95420; }"
    );
}

// === 辅助：获取回收站路径 ===
QString FileManager::getTrashPath()
{
    // Linux 标准回收站路径在 ~/.local/share/Trash/files
    // 为了简单，我们只用 files 文件夹，忽略 .trashinfo 元数据
    QString path = QDir::homePath() + "/.local/share/Trash/files";

    // 如果你想在 Windows 上测试，或者为了更简单，也可以直接在主目录建个 .RecycleBin
    // QString path = QDir::homePath() + "/.RecycleBin";

    return path;
}

// 辅助函数：封装添加逻辑
void FileManager::addSideItem(QString name, QString iconPath, QString targetPath)
{
    // 如果路径不存在（比如有些系统没有 Music 文件夹），就不添加，防止点进去报错
    QDir dir(targetPath);
    if (!dir.exists()) return;

    QListWidgetItem *item = new QListWidgetItem(QIcon(iconPath), name);
    item->setData(Qt::UserRole, targetPath); // 存入真实路径
    ui->sideBar->addItem(item);
}


// ==========================================
//              交互槽函数
// ==========================================

// 1. 侧边栏点击 -> 跳转
void FileManager::on_sideBar_itemClicked(QListWidgetItem *item)
{
    QString path = item->data(Qt::UserRole).toString();
    loadFiles(path);
}

// 2. 双击列表 -> 进入文件夹
void FileManager::on_fileList_itemDoubleClicked(QListWidgetItem *item)
{
    // =======================================================
    // 🔴 重点修复：过滤掉右键双击
    // =======================================================
    if (QApplication::mouseButtons() != Qt::LeftButton) {
        return; // 如果是右键双击，直接忽略
    }

    QString fullPath = item->data(Qt::UserRole).toString();
    bool isDir = item->data(Qt::UserRole + 1).toBool();

    if (isDir) {
        loadFiles(fullPath);
    } else {
        qDebug() << "打开文件: " << fullPath;
    }
}


// === 2. 加载文件列表 ===
void FileManager::loadFiles(QString path)
{
    QDir dir(path);
    if (!dir.exists()) return;

    // 记录当前路径
    m_currentDir = dir;

    // =======================================================
    // 【关键修复】这里必须调用 setText，否则输入框永远是空的！
    // =======================================================
    ui->linePath->setText(path);

    // 清空旧列表
    ui->fileList->clear();

    // 设置过滤器：显示文件和文件夹，但不显示隐藏文件
    dir.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
    dir.setSorting(QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);

    QFileInfoList list = dir.entryInfoList();

    // 遍历添加文件项
    for (const QFileInfo &fileInfo : list) {
        QListWidgetItem *item = new QListWidgetItem();
        item->setText(fileInfo.fileName());
        item->setIcon(getFileIcon(fileInfo));

        // 存储完整路径（给点击事件用）
        item->setData(Qt::UserRole, fileInfo.absoluteFilePath());
        // 存储类型（是文件夹还是文件）
        item->setData(Qt::UserRole + 1, fileInfo.isDir());

        ui->fileList->addItem(item);
    }
}

// === 3. 获取图标逻辑 (保持不变) ===
QIcon FileManager::getFileIcon(QFileInfo fileInfo)
{
    // 1. 如果是文件夹 (且不是根目录等特殊路径)
    if (fileInfo.isDir()) {
        return QIcon(":/images/file_icon.png"); // 确保你有这个图标！
    }

    // 2. 获取后缀名 (转小写)
    QString suffix = fileInfo.suffix().toLower();

    // 3. 匹配类型
    if (suffix == "jpg" || suffix == "png" || suffix == "jpeg" || suffix == "bmp") {
        return QIcon(":/images/picture_icon.png"); // 你的图库图标
    }
    else if (suffix == "mp3" || suffix == "flac" || suffix == "wav") {
        return QIcon(":/images/music_icon.png"); // 你的音乐图标
    }
    else if (suffix == "mp4" || suffix == "avi" || suffix == "mkv" || suffix == "mov") {
        return QIcon(":/images/video_icon1.png"); // 你的视频图标
    }
    else if (suffix == "cpp" || suffix == "h" || suffix == "txt") {
        return QIcon(":/images/notepad1.png"); // 你的文本/代码图标
    }

    // 4. 其他未知文件 -> 返回一个通用的“白纸”图标
    // 千万不要用文件夹图片当默认值！
    return QIcon(":/images/file.png");
}



void FileManager::on_file_back_clicked()
{
    auto *w = new MainWindow();
    replaceDesktopTopLevel(this, w);
}


// ==========================================
//              右键菜单逻辑 (核心)
// ==========================================
void FileManager::on_fileList_customContextMenuRequested(const QPoint &pos)
{
    QListWidgetItem *item = ui->fileList->itemAt(pos);

    // 如果有点到东西，强制选中它（修复之前的 bug）
    if (item) {
        ui->fileList->setCurrentItem(item);
    }

    QMenu menu(this);
    // =================================================================
    // 🎨 【UI美化】核心修改：添加选中高亮、圆角和边框阴影感
    // =================================================================
    menu.setStyleSheet(
        // 1. 菜单整体样式：白底、灰色细边框、圆角
        "QMenu {"
        "    background-color: white;"
        "    border: 1px solid #cccccc;"
        "    border-radius: 6px;"
        "    font-size: 14px;"
        "    padding: 6px;"  // 给菜单边缘留点空隙，更好看
        "}"

        // 2. 每一个选项的样式
        "QMenu::item {"
        "    padding: 8px 30px;"     // 增加内边距，让选项宽一点
        "    border-radius: 4px;"    // 选项也搞成圆角
        "    margin: 2px 4px;"       // 选项之间留点缝隙
        "    color: #333333;"        // 字体颜色
        "}"

        // 3. 【关键】当鼠标悬停/选中时的样式 (高亮/阴影感)
        "QMenu::item:selected {"
        "    background-color: #e5e5e5;" // 浅灰色背景，模拟阴影/高亮
        "    color: black;"              // 选中时文字变黑
        "}"

        // (可选) 如果你喜欢蓝色高亮（像 Windows），就把上面的 background-color 改成 #0078d7，color 改成 white
    );

    // === 定义动作 ===
    if (item) {
        // --- 场景 A：点到了文件/文件夹 ---
        QAction *actOpen = menu.addAction(QIcon(":/images/open.png"), "打开");
        QAction *actRename = menu.addAction(QIcon(":/images/edit.png"), "重命名");
        QAction *actDelete = menu.addAction(QIcon(":/images/delete.png"), "删除");

        // 特殊处理：回收站里
        if (m_currentDir.absolutePath() == getTrashPath()) {
            actDelete->setText("永久删除");
            actRename->setVisible(false); // 回收站内不许重命名，隐藏该选项
        }

        connect(actOpen, &QAction::triggered, this, &FileManager::onActionOpenTriggered);
        connect(actRename, &QAction::triggered, this, &FileManager::onActionRenameTriggered);
        connect(actDelete, &QAction::triggered, this, &FileManager::onActionDeleteTriggered);

    } else {
        // --- 场景 B：点到了空白处 ---
        // 只有在回收站里点击空白处，才加“清空”选项
        if (m_currentDir.absolutePath() == getTrashPath()) {
            QAction *actClear = menu.addAction("清空回收站");
            connect(actClear, &QAction::triggered, this, &FileManager::onActionClearTrashTriggered);
        }
    }

    // =======================================================
    // 🔴 核心修复：如果菜单是空的（没有添加任何 Action），就不显示！
    // =======================================================
    if (menu.isEmpty()) {
        return;
    }

    // 只有非空才弹出
    menu.exec(ui->fileList->mapToGlobal(pos));
}

void FileManager::onActionOpenTriggered()
{
    // 1. 获取当前选中的文件
    QListWidgetItem *item = ui->fileList->currentItem();

    // 防空指针保护
    if (!item) {
        return;
    }

    // 2. 直接执行打开逻辑 (不要调用 on_fileList_itemDoubleClicked)
    QString fullPath = item->data(Qt::UserRole).toString();
    bool isDir = item->data(Qt::UserRole + 1).toBool();

    if (isDir) {
        // 如果是文件夹，进入
        loadFiles(fullPath);
    } else {
        // 如果是文件，打印调试或执行打开
        qDebug() << "右键菜单打开文件: " << fullPath;
        // 这里可以加 QDesktopServices::openUrl(...)
    }
}


void FileManager::onActionRenameTriggered()
{
    QListWidgetItem *item = ui->fileList->currentItem();
    if (!item) return;

    QString oldPath = item->data(Qt::UserRole).toString();
    QFileInfo oldInfo(oldPath);

    bool ok;
    QString newName = QInputDialog::getText(this, "重命名", "请输入新名称:",
                                            QLineEdit::Normal, oldInfo.fileName(), &ok);
    if (ok && !newName.isEmpty() && newName != oldInfo.fileName()) {
        QString newPath = oldInfo.absoluteDir().filePath(newName);
        if (QFile::rename(oldPath, newPath)) {
            loadFiles(m_currentDir.absolutePath()); // 刷新
        } else {
            QMessageBox::warning(this, "失败", "重命名失败，可能重名或无权限。");
        }
    }
}

void FileManager::onActionDeleteTriggered()
{
    QListWidgetItem *item = ui->fileList->currentItem();
    if (!item) return;

    QString fullPath = item->data(Qt::UserRole).toString();
    QFileInfo fileInfo(fullPath);
    bool isTrash = (m_currentDir.absolutePath() == getTrashPath());

    QString title = isTrash ? "永久删除" : "放入回收站";
    QString text = isTrash ? "确定要永久删除吗？不可恢复！" : "确定删除到回收站吗？";

    if (QMessageBox::Yes == QMessageBox::question(this, title, text, QMessageBox::Yes|QMessageBox::No)) {
        if (isTrash) {
            // 1. 永久删除
            if (fileInfo.isDir()) QDir(fullPath).removeRecursively();
            else QFile::remove(fullPath);
        } else {
            // 2. 移入回收站
            QString trashPath = getTrashPath();
            QString targetPath = trashPath + "/" + fileInfo.fileName();

            // 处理重名: file.txt -> file_1.txt
            int i = 1;
            while (QFile::exists(targetPath)) {
                QString base = fileInfo.baseName();
                QString ext = fileInfo.completeSuffix();
                if (!ext.isEmpty()) ext = "." + ext;
                targetPath = trashPath + "/" + base + "_" + QString::number(i++) + ext;
            }
            QFile::rename(fullPath, targetPath);
        }
        loadFiles(m_currentDir.absolutePath()); // 刷新
    }
}

void FileManager::onActionClearTrashTriggered()
{
    if (QMessageBox::Yes == QMessageBox::question(this, "清空", "清空回收站？", QMessageBox::Yes|QMessageBox::No)) {
        QDir trashDir(getTrashPath());
        trashDir.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
        for (const QFileInfo &info : trashDir.entryInfoList()) {
            if (info.isDir()) QDir(info.absoluteFilePath()).removeRecursively();
            else QFile::remove(info.absoluteFilePath());
        }
        loadFiles(m_currentDir.absolutePath());
    }
}
