/*
 * SettingsDialog -- see SettingsDialog.h.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>

#include <vector>

namespace {

const char *const kGray = "color: palette(placeholder-text);";

void fill(QComboBox *box, const char *(*name)(int))
{
    for (int i = 0; ; ++i) {
        const char *n = name(i);
        if (!n) break;
        box->addItem(n);
    }
}

/* The BIOS choices, each one not imported yet saying so. */
void fillBios(QComboBox *box, astrosession *session)
{
    const int cur = box->currentIndex();
    box->clear();
    for (int i = 0; astrosession_bios_name(i); ++i)
        box->addItem(QString::fromUtf8(astrosession_bios_name(i)) +
                     (astrosession_rom_item_available(session, i) ? QString()
                                                                  : QStringLiteral(" (not imported)")));
    if (cur >= 0)
        box->setCurrentIndex(cur);
}

struct State {
    QDialog *dialog;
    astrosession *session;
    QComboBox *bios;
    std::vector<QLabel *> romRows;
    std::function<void()> imported;
};

void refresh(State &st)
{
    for (size_t r = 0; r < st.romRows.size(); r++)
        st.romRows[r]->setText(astrosession_rom_item_available(st.session, (int)r)
                                   ? QStringLiteral("Imported") : QStringLiteral("Not imported"));
    fillBios(st.bios, st.session);
}

void importRoms(State &st, bool folder)
{
    const QString path = folder
        ? QFileDialog::getExistingDirectory(st.dialog, QStringLiteral("Import BIOS from a Folder"))
        : QFileDialog::getOpenFileName(st.dialog, QStringLiteral("Import BIOS"), QString(),
                                       QStringLiteral("BIOS images and ROM sets (*.bin *.BIN *.rom *.ROM "
                                                      "*.zip *.ZIP);;All files (*)"));
    if (path.isEmpty())
        return;
    char msg[1024];
    const int rc = astrosession_rom_import(st.session, path.toUtf8().constData(), msg, sizeof msg);
    QMessageBox::information(st.dialog, rc == 0 ? QStringLiteral("BIOS Imported") : QStringLiteral("Nothing Imported"),
                             QString::fromUtf8(msg));
    if (st.imported)
        st.imported();
    refresh(st);
}

QWidget *romsPage(State &st)
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    QGroupBox *group = nullptr;
    QFormLayout *form = nullptr;
    QString last;
    for (int r = 0; r < astrosession_rom_item_count(); r++) {
        const astro_rom_item *item = astrosession_rom_item(r);
        const QString g = QString::fromUtf8(item->group);
        if (!group || g != last) {
            group = new QGroupBox(g);
            form = new QFormLayout(group);
            if (last.isEmpty()) {
                auto *note = new QLabel(QStringLiteral(
                    "The console's BIOS ROM is not part of the app. Import it from a file, a zip or a "
                    "folder (MAME's astrocde set works); it is recognised by its checksum. Kept in %1.")
                    .arg(QString::fromUtf8(astrosession_roms_path(st.session))));
                note->setWordWrap(true);
                note->setStyleSheet(kGray);
                auto *buttons = new QHBoxLayout;
                auto *files = new QPushButton(QStringLiteral("Import…"));
                auto *dir = new QPushButton(QStringLiteral("Import a Folder…"));
                QObject::connect(files, &QPushButton::clicked, page, [&st] { importRoms(st, false); });
                QObject::connect(dir, &QPushButton::clicked, page, [&st] { importRoms(st, true); });
                buttons->addWidget(files);
                buttons->addWidget(dir);
                buttons->addStretch(1);
                v->addWidget(note);
                v->addLayout(buttons);
            }
            v->addWidget(group);
            last = g;
        }
        auto *state = new QLabel;
        state->setStyleSheet(kGray);
        form->addRow(new QLabel(QString::fromUtf8(item->name)), state);
        st.romRows.push_back(state);
    }
    v->addStretch(1);
    auto *scroll = new QScrollArea;
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
}

} // namespace

bool SettingsDialog::run(QWidget *parent, astrosession *session,
                         std::function<void()> imported, bool romsTab)
{
    QDialog dlg(parent);
    dlg.setWindowTitle("Preferences");
    auto *outer = new QVBoxLayout(&dlg);
    auto *tabs = new QTabWidget;

    auto *machine = new QWidget;
    auto *mv = new QVBoxLayout(machine);
    auto *form = new QFormLayout;
    auto *bios = new QComboBox;
    fillBios(bios, session);
    bios->setCurrentIndex(astrosession_get_int(session, "bios",
                                               ASTROSESSION_BIOS_ASTRO));
    auto *exp = new QComboBox;
    fill(exp, astrosession_exp_name);
    exp->setCurrentIndex(astrosession_get_int(session, "exp",
                                              ASTROSESSION_EXP_NONE));
    form->addRow("System ROM", bios);
    form->addRow("RAM Expansion", exp);
    mv->addLayout(form);
    mv->addWidget(new QLabel("Changes take effect when the machine restarts."));
    mv->addStretch(1);
    tabs->addTab(machine, "Machine");

    State st{ &dlg, session, bios, {}, std::move(imported) };
    tabs->addTab(romsPage(st), "ROMs");
    refresh(st);
    if (romsTab)
        tabs->setCurrentIndex(1);
    outer->addWidget(tabs);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    outer->addWidget(buttons);
    dlg.resize(dlg.sizeHint().expandedTo(QSize(560, 420)));

    if (dlg.exec() != QDialog::Accepted)
        return false;

    bool changed = false;
    if (bios->currentIndex() != astrosession_get_int(session, "bios",
                                                     ASTROSESSION_BIOS_ASTRO)) {
        astrosession_set_int(session, "bios", bios->currentIndex());
        changed = true;
    }
    if (exp->currentIndex() != astrosession_get_int(session, "exp",
                                                    ASTROSESSION_EXP_NONE)) {
        astrosession_set_int(session, "exp", exp->currentIndex());
        changed = true;
    }
    return changed;
}
