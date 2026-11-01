/*
 * The software display: the fallback for a machine with no usable GPU
 * device. A QWidget that scales with QPainter, fed by FrameFeed like the GPU
 * one.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QImage>
#include <QWidget>
#include <cstdint>
#include <vector>

#include "DisplayView.h"
#include "FrameFeed.h"

extern "C" {
#include "astrosession.h"
}

class SoftDisplayWidget : public QWidget, public DisplayView {
    Q_OBJECT
public:
    explicit SoftDisplayWidget(astrosession *session, QWidget *parent = nullptr);
    ~SoftDisplayWidget() override;

    QWidget *widget() override { return this; }

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool eventFilter(QObject *watched, QEvent *e) override;

private:
    void onFrame();
    void updateActive();

    astrosession *m_session;
    FrameFeed *m_feed;
    std::vector<std::uint32_t> m_fb;   /* XRGB straight from the session */
    QImage m_image;            /* wraps m_fb, Format_RGB32 */
    std::uint64_t m_serial = 0;
};
