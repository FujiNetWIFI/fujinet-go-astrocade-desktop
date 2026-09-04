/*
 * DisplayWidget -- see DisplayWidget.h. The core's XRGB8888 maps directly to
 * QImage::Format_RGB32 (0xffRRGGBB on a little-endian host); a QTimer pulls
 * the latest frame and the paint scales it to 4:3 with nearest-neighbour.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DisplayWidget.h"

#include <QPainter>
#include <QTimer>

DisplayWidget::DisplayWidget(astrosession *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 240);
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &DisplayWidget::tick);
    timer->start(16);   /* ~60 Hz redraw opportunity; the core paces itself */
}

void DisplayWidget::tick()
{
    if (m_session &&
        astrosession_copy_frame(m_session, m_fb, &m_serial)) {
        m_image = QImage(reinterpret_cast<const uchar *>(m_fb),
                         ASTROSESSION_FB_WIDTH, ASTROSESSION_FB_HEIGHT,
                         ASTROSESSION_FB_WIDTH * 4, QImage::Format_RGB32);
        update();
    }
}

void DisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (m_image.isNull())
        return;

    const double aspect = 4.0 / 3.0;
    double w = width(), h = height();
    double dw, dh;
    if (w / h > aspect) { dh = h; dw = h * aspect; }
    else                { dw = w; dh = w / aspect; }
    QRectF dest((w - dw) / 2.0, (h - dh) / 2.0, dw, dh);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(dest, m_image);
}
