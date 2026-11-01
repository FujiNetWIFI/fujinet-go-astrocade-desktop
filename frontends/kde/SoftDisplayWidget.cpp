/*
 * SoftDisplayWidget -- see SoftDisplayWidget.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SoftDisplayWidget.h"

#include "DisplayGeometry.h"

#include <QEvent>
#include <QPainter>

SoftDisplayWidget::SoftDisplayWidget(astrosession *session, QWidget *parent)
    : QWidget(parent), m_session(session), m_feed(new FrameFeed(session, this))
{
    m_fb.assign((size_t)ASTROSESSION_FB_WIDTH * ASTROSESSION_FB_HEIGHT, 0);
    m_image = QImage(reinterpret_cast<const uchar *>(m_fb.data()),
                     ASTROSESSION_FB_WIDTH, ASTROSESSION_FB_HEIGHT,
                     ASTROSESSION_FB_WIDTH * 4, QImage::Format_RGB32);
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    connect(m_feed, &FrameFeed::frameReady, this, &SoftDisplayWidget::onFrame);
}

SoftDisplayWidget::~SoftDisplayWidget()
{
    m_feed->setActive(false);
}

void SoftDisplayWidget::updateActive()
{
    m_feed->setActive(isVisible() && window() && !window()->isMinimized());
}

void SoftDisplayWidget::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (window())
        window()->installEventFilter(this);
    updateActive();
}

void SoftDisplayWidget::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    if (window())
        window()->removeEventFilter(this);
    updateActive();
}

bool SoftDisplayWidget::eventFilter(QObject *watched, QEvent *e)
{
    if (e->type() == QEvent::WindowStateChange || e->type() == QEvent::Show ||
        e->type() == QEvent::Hide)
        updateActive();
    return QWidget::eventFilter(watched, e);
}

void SoftDisplayWidget::onFrame()
{
    if (astrosession_copy_frame(m_session, m_fb.data(), &m_serial))
        update();
}

void SoftDisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(displayRect(QSizeF(size())), m_image);
}
