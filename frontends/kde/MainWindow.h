/*
 * The main window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QTimer>
#include <functional>

#include "chfsession.h"

class DisplayView;
class QAction;
class QBoxLayout;
class QFrame;
class QPushButton;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(chfsession *session, QWidget *parent = nullptr);

    /* Starts the session with the saved settings: FujiNet first, then the
     * machine.  */
    void start();
    /* A dropped (or command-line) file: a cartridge is opened, anything else
     * is copied to FujiNet's SD folder. */
    void loadMedia(const QString &path);
    /* A second launch hands over to this window: raise it, and open what it
     * was asked to. */
    void activateFor(const QString &path);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool event(QEvent *e) override;

private:
    void buildMenus();
    void syncMenus();
    void tick();
    void updateStatus();
    void drainSysactions();
    void runSysaction(int sa);
    void pollGamepads();
    void toast(const QString &text);
    void placeToast();

    void openCart(const QString &path);
    void toggleControllers();
    void useSoftwareDisplay();
    void applyPicture();
    void showSettings();
    void restartSession();
    void showAbout();
    void openAtLaunch();

    chfsession *m_session;
    DisplayView *m_display = nullptr;
    QBoxLayout *m_stack = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_dot = nullptr;
    QLabel *m_toast = nullptr;
    QTimer m_toastTimer;
    QTimer m_tick;
    int m_ticks = 0;
    QAction *m_tvAction = nullptr;
    QAction *m_smoothAction = nullptr;
    QAction *m_pauseAction = nullptr;
    /* the Machine menu's console control, labelled with its bound key */
    QAction *m_resetAction = nullptr;
    unsigned m_padGen = 0;
    bool m_sysactDown[CHF_SYSACT_COUNT] = {};
};
