/*
 * Where the picture goes inside the display, shared by the GPU and the
 * software display: the console's pixels are not square, so the 352x240
 * frame is shown as 4:3, centred, as large as fits.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QRectF>
#include <QSizeF>

inline QRectF displayRect(QSizeF avail)
{
    const qreal aspect = 4.0 / 3.0;
    const qreal w = avail.width(), h = avail.height();
    qreal dw, dh;

    if (w / h > aspect) {
        dh = h;
        dw = h * aspect;
    } else {
        dw = w;
        dh = w / aspect;
    }
    return QRectF((w - dw) / 2.0, (h - dh) / 2.0, dw, dh);
}
