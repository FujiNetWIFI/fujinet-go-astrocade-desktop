/*
 * DebuggerWindow -- see DebuggerWindow.h.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

#define DISASM_LINES 24
#define MEM_ROWS 16
#define MEM_COLS 16
#define TRACE_MAX 512

enum { PageCpu, PageVideo, PageFujinet };

static QFont monoFont()
{
    QFont f("monospace");
    f.setStyleHint(QFont::TypeWriter);
    return f;
}

static QLabel *monoLabel()
{
    auto *l = new QLabel;
    l->setFont(monoFont());
    l->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

static QLabel *heading(const char *text)
{
    auto *l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

/* Whether a view's reader is at its end (a log then follows new lines). */
static bool atEnd(QPlainTextEdit *v)
{
    return v->verticalScrollBar()->value() >= v->verticalScrollBar()->maximum() - 1;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void setText(QPlainTextEdit *v, const QString &text, bool follow)
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

DebuggerWindow::DebuggerWindow(astrosession *session, QWidget *parent)
    : QWidget(parent, Qt::Window), m_session(session)
{
    m_dbg = astrodebug_get();
    astrodebug_set_engaged(m_dbg, 1);

    setWindowTitle("Debugger");
    resize(900, 640);
    auto *outer = new QVBoxLayout(this);
    auto *tabs = new QTabWidget;

    /* ---- Z80 tab ---- */
    auto *cpu = new QWidget;
    auto *cpuLayout = new QVBoxLayout(cpu);
    auto *ctl = new QHBoxLayout;
    struct { const char *l; std::function<void()> cb; } btns[] = {
        {"Pause/Resume", [this]{ if (astrodebug_is_paused(m_dbg)) astrodebug_resume(m_dbg); else astrodebug_pause(m_dbg); }},
        {"Step", [this]{ astrodebug_step(m_dbg); }},
        {"Step Over", [this]{ astrodebug_step_over(m_dbg); }},
        {"Step Out", [this]{ astrodebug_step_out(m_dbg); }},
    };
    for (auto &b : btns) {
        auto *btn = new QPushButton(b.l);
        connect(btn, &QPushButton::clicked, this, b.cb);
        ctl->addWidget(btn);
    }
    m_status = new QLabel("Running");
    ctl->addWidget(m_status, 1);
    cpuLayout->addLayout(ctl);

    m_regs = monoLabel();
    cpuLayout->addWidget(m_regs);
    auto *cols = new QHBoxLayout;
    m_disasm = monoLabel();
    m_mem = monoLabel();
    cols->addWidget(m_disasm);
    cols->addWidget(m_mem);
    cpuLayout->addLayout(cols);

    auto *entries = new QHBoxLayout;
    auto *memE = new QLineEdit;
    memE->setPlaceholderText("mem addr (hex)");
    connect(memE, &QLineEdit::returnPressed, this, [this, memE] {
        m_memAddr = (std::uint16_t)strtol(memE->text().toUtf8().constData(), nullptr, 16);
        refreshCpu();
    });
    auto *bpE = new QLineEdit;
    bpE->setPlaceholderText("toggle breakpoint (hex)");
    connect(bpE, &QLineEdit::returnPressed, this, [this, bpE] {
        if (!bpE->text().isEmpty()) {
            astrodebug_breakpoint_toggle(m_dbg,
                (std::uint16_t)strtol(bpE->text().toUtf8().constData(), nullptr, 16));
            bpE->clear();
            refreshCpu();
        }
    });
    entries->addWidget(memE);
    entries->addWidget(bpE);
    cpuLayout->addLayout(entries);
    cpuLayout->addStretch(1);
    tabs->addTab(cpu, "Z80");

    /* ---- Video chip tab ---- */
    auto *vid = new QWidget;
    auto *vidLayout = new QVBoxLayout(vid);
    auto *pics = new QHBoxLayout;
    auto framed = [](const char *title, QLabel *l) {
        auto *box = new QGroupBox(title);
        auto *bl = new QVBoxLayout(box);
        bl->addWidget(l);
        return box;
    };
    m_screen = new QLabel; m_screen->setMinimumSize(352, 240);
    m_bitmap = new QLabel; m_bitmap->setMinimumSize(320, 204);
    pics->addWidget(framed("Screen", m_screen));
    pics->addWidget(framed("Screen RAM", m_bitmap));
    vidLayout->addLayout(pics);
    m_palette = new QLabel;
    vidLayout->addWidget(framed("Palette (512 pens)", m_palette));
    m_vidText = monoLabel();
    vidLayout->addWidget(framed("Registers", m_vidText));
    vidLayout->addStretch(1);
    tabs->addTab(vid, "Video chip");

    /* ---- FujiNet tab ---- */
    auto *fuji = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->addWidget(heading("FujiBus packets (SLIP-decoded)"));
    auto *row = new QHBoxLayout;
    m_fujiNote = new QLabel;
    m_fujiNote->setStyleSheet("color: gray;");
    m_fujiNote->setWordWrap(true);
    auto *clear = new QPushButton("Clear");
    connect(clear, &QPushButton::clicked, this, [this] {
        astrodebug_fujinet_trace_clear(m_dbg);
        refreshFujinet(true);
    });
    row->addWidget(m_fujiNote, 1);
    row->addWidget(clear);
    lv->addLayout(row);
    m_fujiText = new QPlainTextEdit;
    m_fujiText->setReadOnly(true);
    m_fujiText->setFont(monoFont());
    m_fujiText->setLineWrapMode(QPlainTextEdit::NoWrap);
    lv->addWidget(m_fujiText, 1);
    left->setMinimumWidth(480);
    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->addWidget(heading("FujiNet cartridge"));
    m_fujiState = monoLabel();
    rv->addWidget(m_fujiState, 1);
    right->setMinimumWidth(340);
    fuji->addWidget(left);
    fuji->addWidget(right);
    fuji->setStretchFactor(0, 3);
    fuji->setStretchFactor(1, 2);
    fuji->setChildrenCollapsible(false);
    tabs->addTab(fuji, "FujiNet");
    connect(tabs, &QTabWidget::currentChanged, this, [this](int page) {
        if (page == PageFujinet) refreshFujinet(false);
    });
    m_tabs = tabs;

    outer->addWidget(tabs);

    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &DebuggerWindow::tick);
    timer->start(100);
}

DebuggerWindow::~DebuggerWindow()
{
    astrodebug_set_engaged(m_dbg, 0);   /* resumes if paused */
}

static QPixmap pixFromScratch(const std::uint32_t *px, int w, int h)
{
    QImage img(reinterpret_cast<const uchar *>(px), w, h, w * 4,
               QImage::Format_RGB32);
    return QPixmap::fromImage(img.copy());
}

void DebuggerWindow::refreshVideo()
{
    astrovid_snapshot snap;
    astrovid_snapshot_get(&snap);
    int w, h;

    astrovid_render_screen(&snap, m_scratch, &w, &h);
    m_screen->setPixmap(pixFromScratch(m_scratch, w, h));

    astrovid_render_bitmap(&snap, m_scratch, &w, &h);
    m_bitmap->setPixmap(pixFromScratch(m_scratch, ASTROVID_BITMAP_W, h));

    int rows = astrovid_render_palette(&snap, m_scratch, ASTROVID_PAL_COLS,
                                       ASTROVID_PAL_CELL);
    m_palette->setPixmap(pixFromScratch(m_scratch,
        ASTROVID_PAL_COLS * ASTROVID_PAL_CELL, rows * ASTROVID_PAL_CELL));

    char text[4096];
    astrovid_format_state(&snap, text, sizeof text);
    m_vidText->setText(text);
}

void DebuggerWindow::refreshCpu()
{
    if (!astrodebug_is_paused(m_dbg)) {
        m_regs->setText("(running — Pause to inspect)");
        m_disasm->clear();
        m_mem->clear();
        return;
    }
    astrodebug_regs r;
    if (!astrodebug_regs_get(m_dbg, &r))
        return;

    char buf[512];
    std::snprintf(buf, sizeof buf,
        "AF %04X   BC %04X   DE %04X   HL %04X\n"
        "AF'%04X   BC'%04X   DE'%04X   HL'%04X\n"
        "IX %04X   IY %04X   SP %04X   PC %04X\n"
        "I %02X  R %02X  IM %d  IFF1 %d  IFF2 %d  %s  WZ %04X",
        r.af, r.bc, r.de, r.hl, r.af2, r.bc2, r.de2, r.hl2,
        r.ix, r.iy, r.sp, r.pc, r.i, r.r, r.im, r.iff1, r.iff2,
        r.halt ? "HALT" : "    ", r.wz);
    m_regs->setText(buf);

    astrodebug_dasm_line lines[DISASM_LINES];
    int n = astrodebug_disassemble(m_dbg, r.pc, lines, DISASM_LINES);
    QString dis;
    for (int i = 0; i < n; ++i) {
        int isbp = astrodebug_breakpoint_is_set(m_dbg, lines[i].addr);
        dis += QString::asprintf("%s%c %04X  %s\n", i == 0 ? "▶" : " ",
                                 isbp ? '*' : ' ', lines[i].addr, lines[i].text);
    }
    m_disasm->setText(dis);

    QString mem;
    for (int row = 0; row < MEM_ROWS; ++row) {
        std::uint16_t base = (std::uint16_t)(m_memAddr + row * MEM_COLS);
        std::uint8_t bytes[MEM_COLS];
        astrodebug_read(m_dbg, base, bytes, MEM_COLS);
        mem += QString::asprintf("%04X ", base);
        for (int c = 0; c < MEM_COLS; ++c)
            mem += QString::asprintf("%02X ", bytes[c]);
        mem += " ";
        for (int c = 0; c < MEM_COLS; ++c) {
            std::uint8_t b = bytes[c];
            mem += (b >= 0x20 && b < 0x7f) ? QChar(b) : QChar('.');
        }
        mem += "\n";
    }
    m_mem->setText(mem);
}

void DebuggerWindow::refreshFujinet(bool force)
{
    static astrodebug_fuji_packet p[TRACE_MAX];
    char state[2048];
    const int n = astrodebug_fujinet_trace(m_dbg, p, TRACE_MAX);

    if (astrosession_fujinet_link(m_session))
        m_fujiNote->setText(QStringLiteral("FujiNet cartridge, connected."));
    else if (astrosession_fujinet_running(m_session))
        m_fujiNote->setText(QStringLiteral("FujiNet cartridge, not connected."));
    else
        m_fujiNote->setText(QStringLiteral(
            "FujiNet cartridge, not connected (the FujiNet runtime is not running)."));
    astrodebug_fujinet_state(m_dbg, state, sizeof state);
    m_fujiState->setText(QString::fromUtf8(state));

    if (!force && n == m_fujiCount && (n == 0 || p[n - 1].serial == m_fujiLast))
        return;
    m_fujiCount = n;
    m_fujiLast = n > 0 ? p[n - 1].serial : 0;
    QByteArray t;
    char hex[16];
    for (int i = 0; i < n; ++i) {
        if (i) t += '\n';
        t += p[i].text;
        if (p[i].repeat > 1) {
            std::snprintf(hex, sizeof hex, "%u", p[i].repeat);
            t += " \xc3\x97";
            t += hex;
        }
        t += "\n     ";
        for (int k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; ++k) {
            std::snprintf(hex, sizeof hex, "%02X ", p[i].data[k]);
            t += hex;
        }
        if (p[i].nbytes > (int)sizeof p[i].data) t += "\xe2\x80\xa6";
    }
    if (n == 0) t = "(no FujiBus packets since the trace was cleared)";
    setText(m_fujiText, QString::fromUtf8(t), force || atEnd(m_fujiText));
}

void DebuggerWindow::tick()
{
    refreshVideo();
    std::uint64_t s = astrodebug_stop_serial(m_dbg);
    /* FujiNet: on a stop, and twice a second while running */
    if (m_tabs->currentIndex() == PageFujinet &&
        (s != m_lastSerial || (!astrodebug_is_paused(m_dbg) && ++m_fujiTicks >= 5))) {
        m_fujiTicks = 0;
        refreshFujinet(false);
    }
    if (s != m_lastSerial || !astrodebug_is_paused(m_dbg)) {
        m_lastSerial = s;
        refreshCpu();
        m_status->setText(astrodebug_is_paused(m_dbg) ? "Paused" : "Running");
    }
}
