/*
 * DisplayWidget -- paints the emulator's latest frame, letterboxed to 4:3.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_KDE_DISPLAYWIDGET_H
#define ASTRO_KDE_DISPLAYWIDGET_H

#include <QImage>
#include <QWidget>
#include <cstdint>

extern "C" {
#include "astrosession.h"
}

class DisplayWidget : public QWidget {
    Q_OBJECT
public:
    explicit DisplayWidget(astrosession *session, QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *) override;

private slots:
    void tick();

private:
    astrosession *m_session;
    QImage m_image;
    std::uint32_t m_fb[ASTROSESSION_FB_WIDTH * ASTROSESSION_FB_HEIGHT];
    std::uint64_t m_serial = 0;
};

#endif
