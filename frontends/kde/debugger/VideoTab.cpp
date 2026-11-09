/*
 * The debugger's Video chip tab (Qt6 Widgets).
 *
 * Everything that decides what to show lives in the core (astrovidtab.h): this file is widgets.
 * The picture comes from the controller's cached buffer already opaque (0xFFRRGGBB), so
 * Format_RGB32 is safe on KWin; a mouse move redraws only the marks, never re-renders.
 * Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "VideoTab.h"

#include <QAction>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

namespace {

constexpr int kPalCell = 14;
const QColor kAccent(0xb5, 0x34, 0x2c);

QLabel *monoLabel()
{
    auto *l = new QLabel;
    QFont f = l->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    l->setFont(f);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    return l;
}

QTableWidget *makeTable(const QStringList &heads)
{
    auto *t = new QTableWidget(0, heads.size());
    t->setHorizontalHeaderLabels(heads);
    t->verticalHeader()->hide();
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionMode(QAbstractItemView::NoSelection);
    t->horizontalHeader()->setStretchLastSection(true);
    QFont f = t->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    t->setFont(f);
    return t;
}

QString hex(unsigned v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
}

void setCell(QTableWidget *t, int row, int col, const QString &text, bool hot)
{
    auto *it = t->item(row, col);
    if (!it) {
        it = new QTableWidgetItem;
        t->setItem(row, col, it);
    }
    it->setText(text);
    it->setBackground(hot ? QBrush(kAccent) : QBrush());
    it->setForeground(hot ? QBrush(Qt::white) : QBrush());
}

} // namespace

/* ---- the picture ------------------------------------------------------------ */

class VideoPicture : public QWidget
{
public:
    explicit VideoPicture(VideoTab *owner) : m_owner(owner)
    {
        setMouseTracking(true);
        setMinimumSize(320, 200);
    }
    /* the controller's buffer, or a message when this view cannot be drawn */
    void sync()
    {
        int w = 0, h = 0;
        const uint32_t *px = astrovid_tab_picture(m_owner->m_tab, &w, &h);
        if (px) {
            /* a view over the controller's buffer: valid until its next refresh */
            m_image = QImage(reinterpret_cast<const uchar *>(px), w, h, w * 4, QImage::Format_RGB32);
            m_msg.clear();
            setFixedSize(w, h);
        } else {
            m_image = QImage();
            const char *why = astrovid_tab_why(m_owner->m_tab);
            m_msg = QString::fromUtf8(why ? why : "Nothing to show");
            setMinimumSize(320, 200);
            setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(18, 18, 24));
        if (m_image.isNull()) {
            p.setPen(QColor(140, 140, 148));
            p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, m_msg);
            return;
        }
        p.drawImage(0, 0, m_image);
        int hover[4] = { 0 }, pin[4] = { 0 };
        const int marks = astrovid_tab_marks(m_owner->m_tab, hover, pin);
        if (marks & 1) {
            p.setPen(QPen(QColor(255, 255, 255, 230), 1));
            p.drawRect(hover[0] - 1, hover[1] - 1, hover[2] + 1, hover[3] + 1);   /* just outside the pixel */
        }
        if (marks & 2) {
            p.setPen(QPen(QColor(255, 217, 26), 2));
            p.drawRect(pin[0] - 1, pin[1] - 1, pin[2] + 1, pin[3] + 1);
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const QPoint pt = e->position().toPoint();
        if (astrovid_tab_hover(m_owner->m_tab, pt.x(), pt.y())) {
            update();
            m_owner->updatePixelSection();
        }
    }
    void leaveEvent(QEvent *) override
    {
        if (astrovid_tab_hover(m_owner->m_tab, -1, -1)) {
            update();
            m_owner->updatePixelSection();
        }
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const QPoint pt = e->position().toPoint();
        astrovid_tab_pin(m_owner->m_tab, pt.x(), pt.y());
        update();
        m_owner->updatePixelSection();
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const QPoint pt = e->position().toPoint();
        astrovid_tab_hover(m_owner->m_tab, pt.x(), pt.y());
        m_owner->breakOnPixel();
        update();
    }

private:
    VideoTab *m_owner;
    QImage m_image;
    QString m_msg;
};

/* ---- the 256 reachable palette entries, 16 x 16 ---------------------------------- */

class VideoPalette : public QWidget
{
public:
    explicit VideoPalette(VideoTab *owner) : m_owner(owner)
    {
        setMouseTracking(true);
        setFixedSize(16 * (kPalCell + 2), 16 * (kPalCell + 2));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const astrovid_pal_entry *e;
        const int n = astrovid_tab_palette(m_owner->m_tab, &e);
        QPainter p(this);
        for (int i = 0; i < n && i < 256; ++i) {
            const QRect r((i % 16) * (kPalCell + 2), (i / 16) * (kPalCell + 2), kPalCell, kPalCell);
            p.fillRect(r, QColor::fromRgb(e[i].rgb & 0xFFFFFF));
            if (e[i].regs_mask) {       /* a colour register holds it now */
                p.setPen(QPen(kAccent.lighter(150), 2));
                p.drawRect(r.adjusted(1, 1, -2, -2));
            }
            if (e[i].inspected || i == m_hover) {
                p.setPen(QPen(Qt::white, 2));
                p.drawRect(r.adjusted(-1, -1, 0, 0));
            }
        }
    }
    bool event(QEvent *ev) override
    {
        if (ev->type() == QEvent::ToolTip) {
            auto *he = static_cast<QHelpEvent *>(ev);
            const astrovid_pal_entry *e;
            const int n = astrovid_tab_palette(m_owner->m_tab, &e);
            const int i = indexAt(he->pos());
            if (i >= 0 && i < n) {
                QStringList users;
                for (int k = 0; k < 8; ++k)
                    if (e[i].regs_mask & (1 << k)) users << QStringLiteral("COL%1%2").arg(k & 3).arg(k < 4 ? 'R' : 'L');
                QToolTip::showText(he->globalPos(),
                                   QStringLiteral("reg %1, palette $%2, #%3, %4 pixels%5")
                                       .arg(hex(e[i].reg, 2), hex(e[i].pal, 3), hex(e[i].rgb & 0xFFFFFF, 6))
                                       .arg(e[i].uses)
                                       .arg(users.isEmpty() ? QString() : QStringLiteral(", used by ") + users.join(QLatin1Char(' '))),
                                   this);
            } else {
                QToolTip::hideText();
            }
            return true;
        }
        return QWidget::event(ev);
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const int i = indexAt(e->position().toPoint());
        if (i != m_hover) {
            m_hover = i;
            update();
        }
    }
    void leaveEvent(QEvent *) override
    {
        m_hover = -1;
        update();
    }

private:
    static int indexAt(const QPoint &pt)
    {
        const int c = pt.x() / (kPalCell + 2), r = pt.y() / (kPalCell + 2);
        return (c < 0 || c > 15 || r < 0 || r > 15) ? -1 : r * 16 + c;
    }
    VideoTab *m_owner;
    int m_hover = -1;
};

/* ---- the tab ------------------------------------------------------------------ */

VideoTab::VideoTab(QWidget *parent) : QWidget(parent)
{
    m_tab = astrovid_tab_new();

    auto *root = new QVBoxLayout(this);

    /* ---- toolbar */
    auto *bar = new QHBoxLayout;
    m_view = new QComboBox;
    for (int i = 0; i < ASTROVID_V_COUNT; ++i) m_view->addItem(QString::fromUtf8(astrovid_view_name(i)));
    m_view->setToolTip(QStringLiteral(
        "Display: the last finished frame, as the TV showed it.\nDecoded: screen RAM through each line's own registers.\n"
        "Beam: this frame above the beam, the last one dimmed below.\nValues: the raw pens 0-3."));
    m_zoom = new QComboBox;
    int nz = 0;
    const int *zooms = astrovid_zooms(&nz);
    for (int i = 0; i < nz; ++i) m_zoom->addItem(zooms[i] ? QStringLiteral("%1×").arg(zooms[i]) : QStringLiteral("Fit"), zooms[i]);
    bar->addWidget(new QLabel(QStringLiteral("View")));
    bar->addWidget(m_view);
    bar->addWidget(new QLabel(QStringLiteral("Zoom")));
    bar->addWidget(m_zoom);

    auto *ovBtn = new QToolButton;
    ovBtn->setText(QStringLiteral("Overlays"));
    ovBtn->setPopupMode(QToolButton::InstantPopup);
    auto *ovMenu = new QMenu(ovBtn);
    int n = 0;
    const astrovid_overlay_info *od = astrovid_overlays(&n);
    for (int i = 0; i < n; ++i) {
        QAction *a = ovMenu->addAction(QString::fromUtf8(od[i].label));
        a->setCheckable(true);
        a->setData(od[i].bit);
        m_ov << a;
    }
    ovBtn->setMenu(ovMenu);
    bar->addWidget(ovBtn);

    m_breakBtn = new QPushButton(QStringLiteral("Break on writes"));
    m_breakBtn->setToolTip(QStringLiteral("Write breakpoints on the inspected pixel's screen RAM byte and its mirror"));
    bar->addWidget(m_breakBtn);
    bar->addStretch(1);
    root->addLayout(bar);

    m_status = monoLabel();
    root->addWidget(m_status);

    auto *split = new QSplitter(Qt::Horizontal);
    root->addWidget(split, 1);

    /* ---- left: picture */
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    m_scroll = new QScrollArea;
    m_pic = new VideoPicture(this);
    m_pic->setObjectName(QStringLiteral("videoPicture"));
    m_scroll->setWidget(m_pic);
    m_scroll->setWidgetResizable(false);
    m_scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    lv->addWidget(m_scroll, 1);
    m_note = new QLabel;
    lv->addWidget(m_note);
    split->addWidget(left);

    /* ---- right: sections, in astrovid_section_name order */
    m_sections = new QTabWidget;
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        m_pixelText = monoLabel();
        m_pixelText->setWordWrap(true);
        v->addWidget(m_pixelText);
        auto *btns = new QHBoxLayout;
        auto *brk = new QPushButton(QStringLiteral("Break on its writes"));
        auto *unpin = new QPushButton(QStringLiteral("Unpin"));
        btns->addWidget(brk);
        btns->addWidget(unpin);
        btns->addStretch(1);
        v->addLayout(btns);
        v->addStretch(1);
        connect(brk, &QPushButton::clicked, this, [this] { breakOnPixel(); });
        connect(unpin, &QPushButton::clicked, this, [this] {
            astrovid_tab_unpin(m_tab);
            m_pic->update();
            updatePixelSection();
        });
        m_sections->addTab(w, QString::fromUtf8(astrovid_section_name(ASTROVID_S_PIXEL)));
    }
    m_regs = makeTable({ QStringLiteral("Name"), QStringLiteral("Value"), QStringLiteral("Meaning") });
    m_sections->addTab(m_regs, QString::fromUtf8(astrovid_section_name(ASTROVID_S_REGISTERS)));
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        m_swatches = makeTable({ QStringLiteral(""), QStringLiteral("Reg"), QStringLiteral("Value"), QStringLiteral("Colour"),
                                 QStringLiteral("Pixels"), QStringLiteral("") });
        m_swatches->setRowCount(8);
        m_swatches->verticalHeader()->setDefaultSectionSize(26);
        m_swatches->setFixedHeight(26 + 8 * 26 + 4);
        m_swatches->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        for (int i = 0; i < 8; ++i) {
            auto *b = new QPushButton(QStringLiteral("Break on OUT"));
            connect(b, &QPushButton::clicked, this, [this, i] {
                const int id = astrovid_tab_break_on_swatch(m_tab, i);
                note(id > 0 ? QStringLiteral("Breakpoint added: break on OUT to that colour register (see Breakpoints)")
                            : QStringLiteral("No breakpoint added"));
            });
            m_swatches->setCellWidget(i, 5, b);
        }
        v->addWidget(m_swatches);
        m_pal = new VideoPalette(this);
        v->addWidget(m_pal, 0, Qt::AlignLeft | Qt::AlignTop);
        v->addStretch(1);
        /* a short window scrolls rather than clips */
        auto *sa = new QScrollArea;
        sa->setWidgetResizable(true);
        sa->setFrameShape(QFrame::NoFrame);
        sa->setWidget(w);
        m_sections->addTab(sa, QString::fromUtf8(astrovid_section_name(ASTROVID_S_COLOURS)));
    }
    m_lines = makeTable({ QStringLiteral("Line"), QStringLiteral("Astro"), QStringLiteral("R0-3"), QStringLiteral("L0-3"),
                          QStringLiteral("Bound"), QStringLiteral("VERBL"), QStringLiteral("Mode") });
    m_sections->addTab(m_lines, QString::fromUtf8(astrovid_section_name(ASTROVID_S_LINES)));
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        m_beam = new QProgressBar;
        m_beam->setRange(0, 1000);
        m_beam->setFormat(QStringLiteral("beam"));
        v->addWidget(m_beam);
        m_timingText = monoLabel();
        v->addWidget(m_timingText);
        v->addStretch(1);
        m_sections->addTab(w, QString::fromUtf8(astrovid_section_name(ASTROVID_S_TIMING)));
    }
    split->addWidget(m_sections);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    m_sections->setMinimumWidth(420);
    split->setSizes({ 700, 450 });

    /* start from the controller's state (the ASTRO_DEBUGGER_VIDEO_* variables) */
    m_updating = true;
    m_view->setCurrentIndex(astrovid_tab_view(m_tab));
    for (int i = 0; i < nz; ++i)
        if (zooms[i] == astrovid_tab_zoom(m_tab)) m_zoom->setCurrentIndex(i);
    for (QAction *a : m_ov) a->setChecked((astrovid_tab_overlays(m_tab) & a->data().toUInt()) != 0);
    m_sections->setCurrentIndex(astrovid_tab_env_section(m_tab));
    m_updating = false;

    connect(m_view, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_updating || i < 0) return;
        astrovid_tab_set_view(m_tab, i);
        refresh(true);
    });
    connect(m_zoom, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_updating || i < 0) return;
        astrovid_tab_set_zoom(m_tab, m_zoom->itemData(i).toInt());
        refresh(true);
    });
    for (QAction *a : m_ov)
        connect(a, &QAction::toggled, this, [this](bool) {
            if (m_updating) return;
            unsigned m = 0;
            for (QAction *k : m_ov)
                if (k->isChecked()) m |= k->data().toUInt();
            astrovid_tab_set_overlays(m_tab, m);
            refresh(true);
        });
    connect(m_breakBtn, &QPushButton::clicked, this, [this] { breakOnPixel(); });
    connect(m_sections, &QTabWidget::currentChanged, this, [this](int) {
        m_regsSig.clear();
        m_linesSig.clear();
        refreshView(ASTROVID_TAB_TABLES);
    });
}

VideoTab::~VideoTab()
{
    astrovid_tab_free(m_tab);
}

void VideoTab::note(const QString &text)
{
    m_note->setText(text);
}

void VideoTab::breakOnPixel()
{
    const astrovid_pixel *p = astrovid_tab_pixel(m_tab);
    const int id = astrovid_tab_break_on_pixel(m_tab);
    if (id > 0 && p)
        note(QStringLiteral("Break on writes to $%1 and $%2 (see Breakpoints)").arg(hex(p->addr, 4), hex(p->mirror, 4)));
    else
        note(QStringLiteral("Nothing to break on: hover or pin a pixel inside the data area"));
    updatePixelSection();
}

void VideoTab::updatePixelSection()
{
    const astrovid_pixel *p = astrovid_tab_pixel(m_tab);
    m_pixelText->setText(p ? QString::fromUtf8(p->text)
                           : QStringLiteral("Move over the picture to inspect a pixel.\nClick to pin it; "
                                            "double-click to break on writes to it."));
    if (m_pal) m_pal->update();
}

void VideoTab::fillRegs()
{
    const astrovid_reg *r;
    const int n = astrovid_tab_regs(m_tab, &r);
    QString sig;
    for (int i = 0; i < n; ++i)
        sig += QString::fromUtf8(r[i].value) + (r[i].age < ASTROVID_REG_FADE ? QLatin1Char('*') : QLatin1Char('.'));
    sig += QString::number(n);
    if (sig == m_regsSig) return;
    m_regsSig = sig;
    const int vpos = m_regs->verticalScrollBar()->value();
    m_regs->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        const bool hot = r[i].age < ASTROVID_REG_FADE;
        setCell(m_regs, i, 0, QString::fromUtf8(r[i].name), hot);
        setCell(m_regs, i, 1, QString::fromUtf8(r[i].value), hot);
        setCell(m_regs, i, 2, QString::fromUtf8(r[i].meaning), hot);
    }
    m_regs->resizeColumnsToContents();
    m_regs->verticalScrollBar()->setValue(vpos);
}

void VideoTab::fillColours()
{
    const astrovid_swatch *s;
    const int n = astrovid_tab_swatches(m_tab, &s);
    for (int i = 0; i < n && i < 8; ++i) {
        auto *sw = m_swatches->item(i, 0);
        if (!sw) {
            sw = new QTableWidgetItem;
            m_swatches->setItem(i, 0, sw);
        }
        sw->setBackground(QBrush(QColor::fromRgb(s[i].rgb & 0xFFFFFF)));
        setCell(m_swatches, i, 1, QString::fromUtf8(s[i].name), false);
        setCell(m_swatches, i, 2, hex(s[i].value, 2), false);
        setCell(m_swatches, i, 3, QStringLiteral("#") + hex(s[i].rgb & 0xFFFFFF, 6), false);
        setCell(m_swatches, i, 4, QStringLiteral("%1 pixels").arg(s[i].uses), false);
    }
    m_swatches->resizeColumnsToContents();
    m_swatches->setColumnWidth(0, 28);
    m_pal->update();
}

void VideoTab::fillLines()
{
    const astrovid_line_row *r;
    const int n = astrovid_tab_lines(m_tab, &r);
    QString sig = QString::number(n);
    for (int i = 0; i < n; ++i)
        sig += QStringLiteral("|%1:").arg(r[i].line) + QString::number(r[i].changed_mask) + hex(r[i].colorsplit, 2) +
               hex(r[i].bgdata, 2) + hex(r[i].vblank, 2) + hex(r[i].mode, 2) + hex(r[i].colors[0] ^ r[i].colors[5], 2);
    if (sig == m_linesSig) return;
    m_linesSig = sig;
    const int vpos = m_lines->verticalScrollBar()->value();
    m_lines->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        const bool hot = r[i].changed_mask != 0;
        QString rr, ll;
        for (int k = 0; k < 4; ++k) {
            rr += hex(r[i].colors[k], 2) + (k < 3 ? QStringLiteral(" ") : QString());
            ll += hex(r[i].colors[4 + k], 2) + (k < 3 ? QStringLiteral(" ") : QString());
        }
        setCell(m_lines, i, 0, QString::number(r[i].line), hot);
        setCell(m_lines, i, 1, QString::number(r[i].astro_line), hot);
        setCell(m_lines, i, 2, rr, hot);
        setCell(m_lines, i, 3, ll, hot);
        setCell(m_lines, i, 4, hex(r[i].colorsplit, 2), hot);
        setCell(m_lines, i, 5, hex(r[i].vblank, 2), hot);
        setCell(m_lines, i, 6, hex(r[i].mode, 2), hot);
    }
    m_lines->resizeColumnsToContents();
    m_lines->verticalScrollBar()->setValue(vpos);
}

void VideoTab::fillTiming()
{
    const char *const *lines;
    const int n = astrovid_tab_timing(m_tab, &lines);
    QString s;
    for (int i = 0; i < n; ++i) {
        const QString line = QString::fromUtf8(lines[i]);
        const int t = line.indexOf(QLatin1Char('\t'));
        s += t >= 0 ? line.left(t).leftJustified(14) + QLatin1Char(' ') + line.mid(t + 1) : line;
        s += QLatin1Char('\n');
    }
    m_timingText->setText(s.isEmpty() ? QStringLiteral("No timing yet") : s);
    m_beam->setValue(astrovid_tab_beam_permille(m_tab));
}

void VideoTab::refreshView(unsigned what)
{
    if (what & ASTROVID_TAB_PICTURE) {
        m_pic->sync();
    }
    if (what & ASTROVID_TAB_TABLES) {
        m_status->setText(QString::fromUtf8(astrovid_tab_status(m_tab)));
        switch (m_sections->currentIndex()) {
        case ASTROVID_S_REGISTERS: fillRegs(); break;
        case ASTROVID_S_COLOURS: fillColours(); break;
        case ASTROVID_S_LINES: fillLines(); break;
        case ASTROVID_S_TIMING: fillTiming(); break;
        default: break;
        }
        updatePixelSection();
    }
}

void VideoTab::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    /* Fit follows the room the picture has */
    if (astrovid_tab_zoom(m_tab) == 0) refresh(false);
}

void VideoTab::refresh(bool force)
{
    const QSize vp = m_scroll->viewport()->size();
    if (vp.width() > 4 && vp.height() > 4) astrovid_tab_set_viewport(m_tab, vp.width() - 2, vp.height() - 2);
    unsigned what = astrovid_tab_refresh(m_tab, force);
    if (force) what |= ASTROVID_TAB_PICTURE | ASTROVID_TAB_TABLES;
    if (what & ASTROVID_TAB_OFF) what |= ASTROVID_TAB_PICTURE | ASTROVID_TAB_TABLES;
    refreshView(what);
}
