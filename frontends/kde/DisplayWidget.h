/*
 * The emulator display on the GPU: a QRhiWidget that keeps one texture the
 * size of the frame and draws one textured quad, only when the session has
 * published a new frame (see FrameFeed). Qt picks Vulkan, Metal, D3D or
 * OpenGL for it; the scaling and filtering happen on the GPU.
 *
 * If no GPU device can be had it emits renderFailed() and the main window
 * swaps in the software SoftDisplayWidget.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QRhiWidget>
#include <cstdint>
#include <memory>
#include <vector>
#include <rhi/qrhi.h>

#include "DisplayView.h"
#include "FrameFeed.h"

extern "C" {
#include "astrosession.h"
}

class DisplayWidget : public QRhiWidget, public DisplayView {
    Q_OBJECT
public:
    explicit DisplayWidget(astrosession *session, QWidget *parent = nullptr);
    ~DisplayWidget() override;

    QWidget *widget() override { return this; }

protected:
    void initialize(QRhiCommandBuffer *cb) override;
    void render(QRhiCommandBuffer *cb) override;
    void releaseResources() override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool eventFilter(QObject *watched, QEvent *e) override;
    bool event(QEvent *e) override;

private:
    void onFrame();
    void updateActive();

    astrosession *m_session;
    FrameFeed *m_feed;
    std::vector<std::uint32_t> m_fb;   /* XRGB straight from the session */
    std::uint64_t m_serial = 0;
    bool m_have = false;       /* m_fb holds a frame */
    bool m_dirty = false;      /* m_fb holds a frame not yet uploaded */

    QRhi *m_rhi = nullptr;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiTexture> m_tex;
    std::unique_ptr<QRhiSampler> m_nearest;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
};
