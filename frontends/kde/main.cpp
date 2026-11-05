/*
 * FujiNet Go Channel F -- the KDE (Qt6 Widgets) frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTimer>
#include <csignal>

#include "MainWindow.h"
#include "chfsession.h"

/* SIGTERM (a logout, a service manager) and SIGINT (Ctrl+C in a terminal)
 * quit the way closing the window does, so the session stops the machine
 * and FujiNet and flushes the settings. */
static void onQuitSignal(int)
{
    QMetaObject::invokeMethod(qApp, &QApplication::quit, Qt::QueuedConnection);
}

/* One instance: the FujiNet it starts listens on a port of this app's own,
 * so a second would only fight the first. A second launch hands its file (if
 * any) to the first over a socket and exits. */
static QString instanceSocket()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty()) dir = QDir::tempPath();
    return dir + QStringLiteral("/online.fujinet.go.channelf.kde.socket");
}

static bool handOver(const QString &socketPath, const QString &file)
{
    QLocalSocket s;
    s.connectToServer(socketPath);
    if (!s.waitForConnected(500)) return false;
    s.write(file.toUtf8() + '\n');
    s.waitForBytesWritten(1000);
    return true;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("fujinet-go-channelf-kde"));
    app.setApplicationDisplayName(QStringLiteral("FujiNet Go Channel F"));
    app.setDesktopFileName(QStringLiteral("online.fujinet.go.channelf.kde"));

    /* a path on the command line goes where Open would put it: a cartridge boots in place of CONFIG, anything else goes to
     * the SD folder, once the window is up to say so */
    const QString arg = argc > 1 ? QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath() : QString();
    const QString socketPath = instanceSocket();
    if (handOver(socketPath, arg)) return 0;

    /* The installed icon is named after the desktop-entry id; running out
     * of the build tree, the in-tree artwork stands in. */
    {
        QIcon icon = QIcon::fromTheme(QStringLiteral("online.fujinet.go.channelf.kde"));
        if (icon.isNull()) {
            QIcon::setThemeSearchPaths(QIcon::themeSearchPaths() << QStringLiteral(CHF_SOURCE_ICON_DIR));
            icon = QIcon::fromTheme(QStringLiteral("fujinet-go-channelf"));
        }
        if (!icon.isNull()) app.setWindowIcon(icon);
    }

    chfsession *session = chfsession_new(nullptr);
    if (!session) {
        qCritical("Could not create the session (unusable config/data dirs?)");
        return 1;
    }
    std::signal(SIGTERM, onQuitSignal);
    std::signal(SIGINT, onQuitSignal);

    int rc;
    /* The windows go before the session does: the debugger and the
     * Controllers window still call into it as they are hidden, and the main
     * window can close with either of them up. */
    {
        MainWindow win(session);

        QLocalServer server;
        QLocalServer::removeServer(socketPath);   /* a crashed run's socket */
        server.setSocketOptions(QLocalServer::UserAccessOption);
        QObject::connect(&server, &QLocalServer::newConnection, &win, [&server, &win] {
            while (QLocalSocket *c = server.nextPendingConnection()) {
                QObject::connect(c, &QLocalSocket::disconnected, c, &QObject::deleteLater);
                QObject::connect(c, &QLocalSocket::readyRead, &win, [c, &win] {
                    while (c->canReadLine())
                        win.activateFor(QString::fromUtf8(c->readLine()).trimmed());
                });
            }
        });
        if (!server.listen(socketPath)) qWarning("Single instance: %s", qPrintable(server.errorString()));

        win.show();
        /* FujiNet first, then the machine, once the window has painted */
        QTimer::singleShot(0, &win, [&win, arg] {
            win.start();
            if (!arg.isEmpty()) win.loadMedia(arg);
        });

        rc = app.exec();
    }
    chfsession_stop(session);
    chfsession_free(session);
    return rc;
}
