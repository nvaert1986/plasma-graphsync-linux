// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "autofreeup.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace GraphSync;

namespace
{
void touch(const QString &path, qint64 atime, qint64 mtime)
{
    const struct timespec times[2] = {{static_cast<time_t>(atime), 0}, {static_cast<time_t>(mtime), 0}};
    QVERIFY(utimensat(AT_FDCWD, QFile::encodeName(path).constData(), times, 0) == 0);
}

void write(const QString &path)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x");
}
}

class AutoFreeUpTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void lastUsedIsTheLatestTime()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("a.txt"));
        write(file);
        // The inode change time (now: just written) cannot be set back; it counts as use
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        touch(file, 1000, 5000);
        const qint64 changed = AutoFreeUp::lastUsed(file, false).value_or(-1);
        QVERIFY(changed >= now - 5 && changed <= now + 5);
        // A read later than everything counts only where access times are kept
        touch(file, now + 100000, 5000);
        QCOMPARE(AutoFreeUp::lastUsed(file, true).value_or(-1), now + 100000);
        QVERIFY(AutoFreeUp::lastUsed(file, false).value_or(-1) < now + 100000);
    }

    void lastUsedOnlyForFiles()
    {
        QTemporaryDir dir;
        QVERIFY(!AutoFreeUp::lastUsed(dir.path(), true));
        QVERIFY(!AutoFreeUp::lastUsed(dir.filePath(QStringLiteral("missing")), true));
        const QString file = dir.filePath(QStringLiteral("a.txt"));
        write(file);
        const QString link = dir.filePath(QStringLiteral("link"));
        QVERIFY(QFile::link(file, link));
        QVERIFY(!AutoFreeUp::lastUsed(link, true));
    }

    void officeLocks()
    {
        QTemporaryDir dir;
        const QString odt = dir.filePath(QStringLiteral("Report.odt"));
        const QString docx = dir.filePath(QStringLiteral("Document.docx"));
        const QString xlsx = dir.filePath(QStringLiteral("a.xlsx"));
        write(odt);
        write(docx);
        write(xlsx);
        QVERIFY(!AutoFreeUp::hasOfficeLock(odt));
        QVERIFY(!AutoFreeUp::hasOfficeLock(docx));
        QVERIFY(!AutoFreeUp::hasOfficeLock(xlsx));
        write(dir.filePath(QStringLiteral(".~lock.Report.odt#")));
        write(dir.filePath(QStringLiteral("~$cument.docx")));
        write(dir.filePath(QStringLiteral("~$a.xlsx")));
        QVERIFY(AutoFreeUp::hasOfficeLock(odt));
        QVERIFY(AutoFreeUp::hasOfficeLock(docx));
        QVERIFY(AutoFreeUp::hasOfficeLock(xlsx));
        // A 7-character base name: Word drops one character ("Report1.docx" -> "~$eport1.docx")
        const QString report = dir.filePath(QStringLiteral("Report1.docx"));
        write(report);
        QVERIFY(!AutoFreeUp::hasOfficeLock(report));
        write(dir.filePath(QStringLiteral("~$eport1.docx")));
        QVERIFY(AutoFreeUp::hasOfficeLock(report));
    }

    void openFilesListsWhatIsOpen()
    {
        QTemporaryDir dir;
        const QString path = QDir(dir.path()).canonicalPath() + QStringLiteral("/open.txt");
        write(path);
        QVERIFY(!AutoFreeUp::openFiles().contains(path));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(AutoFreeUp::openFiles().contains(path));
        file.close();
        QVERIFY(!AutoFreeUp::openFiles().contains(path));
    }

    void accessTimesOnThisDisk()
    {
        // No assertion on the answer (it depends on the mount); it must not fail on a real path
        QTemporaryDir dir;
        qInfo() << "access times kept here:" << AutoFreeUp::accessTimesKept(dir.path());
        QVERIFY(!AutoFreeUp::accessTimesKept(dir.filePath(QStringLiteral("missing/deeper"))));
    }
};

QTEST_GUILESS_MAIN(AutoFreeUpTest)

#include "autofreeuptest.moc"
