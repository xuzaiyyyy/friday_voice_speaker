#ifndef APPPATHS_H
#define APPPATHS_H

#include <QString>
#include <QStringList>

namespace AppPaths
{
QString desktopRoot();
QString projectRoot();
QString desktopFile(const QString &relativePath);
QString projectFile(const QString &relativePath);
QString ensureDesktopDir(const QString &relativePath);
QString existingDesktopFile(const QString &relativePath);
QString existingProjectOrDesktopFile(const QString &relativePath);
QStringList emojiRoots();
}

#endif // APPPATHS_H
