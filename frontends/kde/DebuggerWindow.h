/*
 * DebuggerWindow -- Qt6 three-tab debugger (Z80 / Video chip / FujiNet). See
 * the GNOME dbg_window.c for the shared design.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_KDE_DEBUGGERWINDOW_H
#define ASTRO_KDE_DEBUGGERWINDOW_H

#include <QWidget>
#include <cstdint>

extern "C" {
#include "astrosession.h"
#include "astrodebug.h"
#include "astrovid.h"
}

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QTabWidget;

class DebuggerWindow : public QWidget {
    Q_OBJECT
public:
    explicit DebuggerWindow(astrosession *session, QWidget *parent = nullptr);
    ~DebuggerWindow() override;

private slots:
    void tick();

private:
    void refreshCpu();
    void refreshVideo();
    void refreshFujinet(bool force);

    astrosession *m_session;
    astrodebug *m_dbg;
    std::uint64_t m_lastSerial = ~0ULL;
    std::uint16_t m_memAddr = 0;

    QLabel *m_regs = nullptr;
    QLabel *m_disasm = nullptr;
    QLabel *m_mem = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_screen = nullptr;
    QLabel *m_bitmap = nullptr;
    QLabel *m_palette = nullptr;
    QLabel *m_vidText = nullptr;

    QTabWidget *m_tabs = nullptr;
    QLabel *m_fujiNote = nullptr;
    QPlainTextEdit *m_fujiText = nullptr;
    QLabel *m_fujiState = nullptr;
    int m_fujiCount = -1;
    std::uint64_t m_fujiLast = 0;
    int m_fujiTicks = 0;

    std::uint32_t m_scratch[ASTROVID_MAX_PIXELS];
};

#endif
