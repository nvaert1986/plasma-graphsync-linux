// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "folderbackup.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace GraphSync;

namespace
{
RootConfig location(const QString &path)
{
    RootConfig root;
    root.id = QStringLiteral("r1");
    root.localPath = path;
    return root;
}
}

class FolderBackupTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void threeFolders()
    {
        const QList<FolderBackup::Folder> folders = FolderBackup::folders();
        QCOMPARE(folders.size(), 3);
        // The names Windows' OneDrive uses, so a Windows PC backs up into the same folders
        QCOMPARE(folders.at(0).onlineName, QStringLiteral("Desktop"));
        QCOMPARE(folders.at(1).onlineName, QStringLiteral("Documents"));
        QCOMPARE(folders.at(2).onlineName, QStringLiteral("Pictures"));
        QVERIFY(FolderBackup::folder(QStringLiteral("documents")));
        QVERIFY(!FolderBackup::folder(QStringLiteral("music")));
    }

    void aFolderOfItsOwnIsFine()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath(QStringLiteral("Documents"));
        QVERIFY(FolderBackup::problem(dir.filePath(QStringLiteral("Documents")), {}).isEmpty());
        // Not there yet: made when the backup starts
        QVERIFY(FolderBackup::problem(dir.filePath(QStringLiteral("Pictures")), {}).isEmpty());
    }

    void notTheHomeFolderOrAFile()
    {
        QVERIFY(!FolderBackup::problem(QDir::homePath(), {}).isEmpty());
        QVERIFY(!FolderBackup::problem(QStringLiteral("/"), {}).isEmpty());
        QVERIFY(!FolderBackup::problem(QString(), {}).isEmpty());
        QVERIFY(!FolderBackup::problem(QStringLiteral("relative/Documents"), {}).isEmpty());
        QTemporaryDir dir;
        QFile file(dir.filePath(QStringLiteral("Documents")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QVERIFY(!FolderBackup::problem(dir.filePath(QStringLiteral("Documents")), {}).isEmpty());
    }

    void notInsideAroundOrOnALocation()
    {
        QTemporaryDir dir;
        const QString onedrive = dir.filePath(QStringLiteral("OneDrive"));
        QDir().mkpath(onedrive + QStringLiteral("/Documents"));
        const QList<RootConfig> roots{location(onedrive)};
        QVERIFY(!FolderBackup::problem(onedrive, roots).isEmpty());
        QVERIFY(!FolderBackup::problem(onedrive + QStringLiteral("/Documents"), roots).isEmpty());
        QVERIFY(!FolderBackup::problem(dir.path(), roots).isEmpty());
        // Only a shared prefix is not inside
        QDir().mkpath(onedrive + QStringLiteral("2"));
        QVERIFY(FolderBackup::problem(onedrive + QStringLiteral("2"), roots).isEmpty());
    }

    void symbolicLinksAreFollowed()
    {
        // ~/Documents as a link into the OneDrive folder must not become a second location of it
        QTemporaryDir dir;
        const QString onedrive = dir.filePath(QStringLiteral("OneDrive"));
        QDir().mkpath(onedrive + QStringLiteral("/Documents"));
        const QString link = dir.filePath(QStringLiteral("Documents"));
        QVERIFY(QFile::link(onedrive + QStringLiteral("/Documents"), link));
        QVERIFY(!FolderBackup::problem(link, {location(onedrive)}).isEmpty());
    }
};

QTEST_GUILESS_MAIN(FolderBackupTest)

#include "folderbackuptest.moc"
