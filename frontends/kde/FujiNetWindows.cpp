/*
 * FujiNetWindows -- see FujiNetWindows.h.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FujiNetWindows.h"

#include <QColor>
#include <QDesktopServices>
#include <QFont>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

QColor chfAccentColor()
{
    return QColor((CHFSESSION_ACCENT_RGB >> 16) & 0xff,
                  (CHFSESSION_ACCENT_RGB >> 8) & 0xff,
                  CHFSESSION_ACCENT_RGB & 0xff);
}

QColor chfAccentDarkColor()
{
    return QColor((CHFSESSION_ACCENT_DARK_RGB >> 16) & 0xff,
                  (CHFSESSION_ACCENT_DARK_RGB >> 8) & 0xff,
                  CHFSESSION_ACCENT_DARK_RGB & 0xff);
}

QColor chfAccentTextColor(const QWidget *w)
{
    /* a light theme is one whose window background is light */
    if (w && w->palette().color(QPalette::Window).lightness() < 128) return chfAccentColor();
    return chfAccentDarkColor();
}

QString chfAccentFill()
{
    return QStringLiteral("background: %1; color: #1b1b1b;").arg(chfAccentColor().name());
}

void fujinet_log_show(QWidget *parent, chfsession *session)
{
    static QPointer<QWidget> win;
    if (win) {
        win->raise();
        win->activateWindow();
        return;
    }
    win = new QWidget(parent, Qt::Window);
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setWindowTitle(QStringLiteral("FujiNet Console Log"));
    win->resize(820, 560);

    auto *view = new QPlainTextEdit(win);
    view->setReadOnly(true);
    QFont mono = view->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    view->setFont(mono);

    auto *layout = new QVBoxLayout(win);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view);

    auto *timer = new QTimer(win);
    auto refresh = [view, session]() {
        static char buf[128 * 1024];
        static QByteArray last;
        const int n = chfsession_fujinet_copy_log(session, buf, sizeof(buf));
        /* nothing new: leave the document (and its relayout) alone */
        const QByteArray now = n > 0 ? QByteArray(buf) : QByteArray();
        if (now == last && view->document()->characterCount() > 1)
            return;
        last = now;
        const bool atEnd = view->verticalScrollBar()->value() ==
                           view->verticalScrollBar()->maximum();
        view->setPlainText(n > 0 ? QString::fromUtf8(buf)
                                 : QStringLiteral("(no FujiNet output yet)"));
        if (atEnd)
            view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
    };
    QObject::connect(timer, &QTimer::timeout, view, refresh);
    timer->start(1000);
    refresh();
    win->show();
}

void fujinet_config_show(QWidget *parent, chfsession *session)
{
    (void)parent;
    QDesktopServices::openUrl(QUrl(QString::fromUtf8(chfsession_fujinet_webui_url(session))));
}
