/*
 * Small helpers the debugger's tabs share: monospace views, hex text, a
 * refresh that keeps the reader's scroll position.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QAbstractItemView>
#include <QFont>
#include <QLineEdit>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QString>

namespace dbgui {

inline QFont monoFont(const QFont &base)
{
    QFont f = base;
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    return f;
}

inline QPlainTextEdit *monoView(bool editable = false)
{
    auto *v = new QPlainTextEdit;
    v->setReadOnly(!editable);
    v->setFont(monoFont(v->font()));
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
}

/* Text with the control characters a debugger message may carry removed. */
inline QString stripControl(const char *s)
{
    QString out;
    for (; *s; ++s)
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') out += QChar(*s);
    return out;
}

inline QString hx(unsigned v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
}

/* Whether a table has a cell open for typing (its state() is protected): a
 * refresh must leave what is being typed alone. */
inline bool editing(QAbstractItemView *v)
{
    return v->viewport()->findChild<QLineEdit *>(QString(), Qt::FindDirectChildrenOnly) != nullptr;
}

inline QString yesNo(int v) { return v ? QStringLiteral("yes") : QStringLiteral("no"); }

/* Whether a view's reader is at its end (a log then follows new lines). */
inline bool atEnd(QPlainTextEdit *v)
{
    return v->verticalScrollBar()->value() >= v->verticalScrollBar()->maximum() - 1;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
inline void setText(QPlainTextEdit *v, const QString &text, bool follow = false)
{
    if (v->toPlainText() == text) {
        if (follow) v->verticalScrollBar()->setValue(v->verticalScrollBar()->maximum());
        return;
    }
    const int top = v->verticalScrollBar()->value(), left = v->horizontalScrollBar()->value();
    v->setPlainText(text);
    v->verticalScrollBar()->setValue(follow ? v->verticalScrollBar()->maximum() : top);
    v->horizontalScrollBar()->setValue(left);
}

inline QLabel *titleLabel(const QString &text)
{
    auto *l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

/* A hint: dimmed, word-wrapped. */
inline QLabel *dimLabel(const QString &text = QString())
{
    auto *l = new QLabel(text);
    l->setStyleSheet(QStringLiteral("color: gray;"));
    l->setWordWrap(true);
    return l;
}

} // namespace dbgui
