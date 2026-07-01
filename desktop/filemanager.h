#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#include <QWidget>
#include <mainwindow.h>
#include <QDir>
#include <QListWidgetItem>
#include <QStandardPaths> // <--- 新增这个
#include <QString>
#include <QProcess>
#include <QInputDialog>
#include <QApplication>

namespace Ui {
class FileManager;
}

class FileManager : public QWidget
{
    Q_OBJECT

public:
    explicit FileManager(QWidget *parent = nullptr);
    ~FileManager();

private slots:

    void on_sideBar_itemClicked(QListWidgetItem *item);
    void on_fileList_itemDoubleClicked(QListWidgetItem *item);

    void on_btnGo_clicked();

    void on_btnBack_clicked();

    void on_file_back_clicked();

    // 【新增】在输入框里按回车 (实现盲打跳转)
    void on_linePath_returnPressed();

    // === 右键/长按菜单 ===
    void on_fileList_customContextMenuRequested(const QPoint &pos);

    // 菜单动作
    void onActionOpenTriggered();
    void onActionRenameTriggered();
    void onActionDeleteTriggered();
    void onActionClearTrashTriggered();

private:
    Ui::FileManager *ui;

    QDir m_currentDir;

    void initUI();      // 界面初始化

    void initSideBar(); // 专门写一个函数初始化侧边栏
    void loadFiles(QString path);
    QString getTrashPath();
    QIcon getFileIcon(QFileInfo fileInfo);

    // 辅助函数：添加侧边栏项目
    void addSideItem(QString name, QString iconPath, QString targetPath);
};

#endif // FILEMANAGER_H
