#include "apppaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QByteArray>

namespace
{
QString cleanPath(const QString &path)
{
    return QDir::cleanPath(path);
}

QString envPath(const char *name)
{
    const QByteArray value = qgetenv(name);
    return value.isEmpty() ? QString() : cleanPath(QString::fromLocal8Bit(value));
}

bool hasDesktopShape(const QString &path)
{
    QDir dir(path);
    if (!dir.exists())
    {
        return false;
    }
    return QFileInfo(dir.filePath("desktop.pro")).exists() ||
           QFileInfo(dir.filePath("images")).isDir() ||
           QFileInfo(dir.filePath("photos")).isDir() ||
           QFileInfo(dir.filePath("music")).isDir();
}

QString firstExistingDesktopRoot(const QStringList &candidates)
{
    for (const QString &candidate : candidates)
    {
        const QString path = cleanPath(candidate);
        if (hasDesktopShape(path))
        {
            return path;
        }
    }
    return QString();
}

QString firstExistingFile(const QStringList &candidates)
{
    for (const QString &candidate : candidates)
    {
        const QString path = cleanPath(candidate);
        if (QFileInfo(path).isFile())
        {
            return path;
        }
    }
    return cleanPath(candidates.isEmpty() ? QString() : candidates.first());
}

QStringList uniquePaths(const QStringList &paths)
{
    QStringList result;
    QSet<QString> seen;
    for (const QString &path : paths)
    {
        const QString cleaned = cleanPath(path);
        if (!cleaned.isEmpty() && !seen.contains(cleaned))
        {
            seen.insert(cleaned);
            result.push_back(cleaned);
        }
    }
    return result;
}
} // namespace

namespace AppPaths
{
QString desktopRoot()
{
    static const QString root = [] {
        const QString overrideRoot = envPath("XIAOMAN_DESKTOP_ROOT");
        if (!overrideRoot.isEmpty())
        {
            return overrideRoot;
        }

        const QString appDir = cleanPath(QCoreApplication::applicationDirPath());
        const QString cwd = cleanPath(QDir::currentPath());
        const QString home = cleanPath(QDir::homePath());

        QStringList candidates;
        candidates << appDir
                   << cwd
                   << QDir(cwd).filePath("desktop")
                   << QDir(appDir).filePath("desktop")
                   << QDir(home).filePath("cpp/friday_voice_speaker/desktop")
                   << QStringLiteral("/home/orangepi/cpp/friday_voice_speaker/desktop");

        const QString found = firstExistingDesktopRoot(candidates);
        return found.isEmpty() ? appDir : found;
    }();
    return root;
}

QString projectRoot()
{
    static const QString root = [] {
        const QString overrideRoot = envPath("XIAOMAN_PROJECT_ROOT");
        if (!overrideRoot.isEmpty())
        {
            return overrideRoot;
        }

        const QDir desktopDir(desktopRoot());
        const QString parent = cleanPath(desktopDir.filePath(".."));
        if (QFileInfo(QDir(parent).filePath("friday_voice_speaker.conf")).exists() ||
            QFileInfo(QDir(parent).filePath("image")).isDir())
        {
            return parent;
        }
        return desktopRoot();
    }();
    return root;
}

QString desktopFile(const QString &relativePath)
{
    return cleanPath(QDir(desktopRoot()).filePath(relativePath));
}

QString projectFile(const QString &relativePath)
{
    return cleanPath(QDir(projectRoot()).filePath(relativePath));
}

QString ensureDesktopDir(const QString &relativePath)
{
    const QString path = desktopFile(relativePath);
    QDir().mkpath(path);
    return path;
}

QString existingDesktopFile(const QString &relativePath)
{
    return firstExistingFile({desktopFile(relativePath), cleanPath(QDir::current().filePath(relativePath))});
}

QString existingProjectOrDesktopFile(const QString &relativePath)
{
    return firstExistingFile({desktopFile(relativePath), projectFile(relativePath), cleanPath(QDir::current().filePath(relativePath))});
}

QStringList emojiRoots()
{
    QStringList roots;
    const QString overrideRoot = envPath("XIAOMAN_DESKTOP_EMOJI_ROOT");
    if (!overrideRoot.isEmpty())
    {
        roots << overrideRoot;
    }

    roots << projectFile("image")
          << desktopFile("../image")
          << desktopFile("image")
          << cleanPath(QDir::current().filePath("../image"))
          << cleanPath(QDir::current().filePath("image"))
          << QDir(QDir::homePath()).filePath("cpp/friday_voice_speaker/image")
          << QStringLiteral("/home/orangepi/cpp/friday_voice_speaker/image");
    return uniquePaths(roots);
}
} // namespace AppPaths
