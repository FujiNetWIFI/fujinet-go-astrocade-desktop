/*
 * SettingsDialog -- see SettingsDialog.h.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

static void fill(QComboBox *box, const char *(*name)(int))
{
    for (int i = 0; ; ++i) {
        const char *n = name(i);
        if (!n) break;
        box->addItem(n);
    }
}

bool SettingsDialog::run(QWidget *parent, astrosession *session)
{
    QDialog dlg(parent);
    dlg.setWindowTitle("Preferences");
    auto *outer = new QVBoxLayout(&dlg);

    auto *form = new QFormLayout;
    auto *bios = new QComboBox;
    fill(bios, astrosession_bios_name);
    bios->setCurrentIndex(astrosession_get_int(session, "bios",
                                               ASTROSESSION_BIOS_ASTRO));
    auto *exp = new QComboBox;
    fill(exp, astrosession_exp_name);
    exp->setCurrentIndex(astrosession_get_int(session, "exp",
                                              ASTROSESSION_EXP_NONE));
    form->addRow("System ROM", bios);
    form->addRow("RAM Expansion", exp);
    outer->addLayout(form);
    outer->addWidget(new QLabel("Changes take effect when the machine restarts."));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    outer->addWidget(buttons);

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
