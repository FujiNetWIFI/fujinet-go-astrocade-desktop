/*
 * The debugger's Video chip tab (Qt6 Widgets): a thin view over core/include/astrovidtab.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

extern "C" {
#include "astrovidtab.h"
}

class QCheckBox;
class QAction;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QTabWidget;
class QTableWidget;
class QVBoxLayout;

class VideoPicture;
class VideoPalette;

class VideoTab : public QWidget
{
    Q_OBJECT
public:
    explicit VideoTab(QWidget *parent = nullptr);
    ~VideoTab() override;

    /* Take a snapshot and redraw what changed (force: everything). Cheap to call every tick. */
    void refresh(bool force);

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    friend class VideoPicture;
    friend class VideoPalette;
    void refreshView(unsigned what);
    void updatePixelSection();
    void fillRegs();
    void fillColours();
    void fillLines();
    void fillTiming();
    void note(const QString &text);
    void breakOnPixel();

    astrovid_tab *m_tab = nullptr;
    QComboBox *m_view = nullptr, *m_zoom = nullptr;
    QList<QAction *> m_ov;
    QPushButton *m_breakBtn = nullptr;
    QLabel *m_status = nullptr, *m_note = nullptr;
    VideoPicture *m_pic = nullptr;
    QScrollArea *m_scroll = nullptr;
    QTabWidget *m_sections = nullptr;
    QLabel *m_pixelText = nullptr, *m_timingText = nullptr;
    QTableWidget *m_regs = nullptr, *m_lines = nullptr, *m_swatches = nullptr;
    VideoPalette *m_pal = nullptr;
    QProgressBar *m_beam = nullptr;
    QString m_regsSig, m_linesSig;
    bool m_updating = false;
};
