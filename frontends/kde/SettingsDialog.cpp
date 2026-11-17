/*
 * SettingsDialog -- see SettingsDialog.h.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsDialog.h"
#include "help/HelpWindow.h"

extern "C" {
#include "bindings.h"
}

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QTabWidget>
#include <QTimer>
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
    QStringList paths;
    if (folder) {
        const QString dir = QFileDialog::getExistingDirectory(st.dialog, QStringLiteral("Import BIOS from a Folder"));
        if (!dir.isEmpty())
            paths << dir;
    } else {
        paths = QFileDialog::getOpenFileNames(st.dialog, QStringLiteral("Import BIOS"), QString(),
                                              QStringLiteral("BIOS images and ROM sets (*.bin *.BIN *.rom *.ROM "
                                                             "*.zip *.ZIP);;All files (*)"));
    }
    if (paths.isEmpty())
        return;
    QStringList log;
    int imported = 0;
    for (const QString &p : paths) {
        char msg[1024];
        if (astrosession_rom_import(st.session, p.toUtf8().constData(), msg, sizeof msg) == 0)
            imported++;
        log << QString::fromUtf8(msg);
    }
    QMessageBox::information(st.dialog, imported ? QStringLiteral("BIOS Imported") : QStringLiteral("Nothing Imported"),
                             log.join(QLatin1Char('\n')));
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

/* One row per pad: the player it is set to (applied at once and remembered, so
 * not part of what OK restarts for) and the player it plays now. */
void fillPadRows(QFormLayout *form, std::vector<QWidget *> &rows, astrosession *session)
{
    for (QWidget *row : rows)
        form->removeRow(row);
    rows.clear();
    const int count = astrosession_gamepad_count(session);
    if (count == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected - plug one in: it is picked up as it appears."));
        none->setWordWrap(true);
        none->setStyleSheet(kGray);
        form->addRow(none);
        rows.push_back(none);
        return;
    }
    for (int idx = 0; idx < count && idx < 8; ++idx) {
        char name[128];
        astrosession_gamepad_name(session, idx, name, sizeof name);
        auto *box = new QComboBox;
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Player 1"), QStringLiteral("Player 2"),
                        QStringLiteral("Player 3"), QStringLiteral("Player 4") });
        const int assign = astrosession_gamepad_assignment(session, idx);
        box->setCurrentIndex(assign < 0 || assign > 3 ? 0 : assign + 1);
        auto tip = [session, idx](QComboBox *combo) {
            const int eff = astrosession_gamepad_effective_port(session, idx);
            combo->setToolTip(eff >= 0 ? QStringLiteral("Playing player %1").arg(eff + 1)
                                       : QStringLiteral("Playing nobody"));
        };
        tip(box);
        QObject::connect(box, &QComboBox::currentIndexChanged, box, [session, idx, box, tip](int sel) {
            astrosession_gamepad_assign(session, idx, sel <= 0 ? -1 : sel - 1);
            tip(box);
        });
        form->addRow(QString::fromUtf8(name), box);
        rows.push_back(box);
    }
}

} // namespace

bool SettingsDialog::run(QWidget *parent, astrosession *session,
                         std::function<void()> imported, const QString &startTab,
                         bool *openControllers)
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

    auto *fujinetBox = new QGroupBox(QStringLiteral("FujiNet"));
    auto *fujinetForm = new QFormLayout(fujinetBox);
    auto *external = new QCheckBox(QStringLiteral("Use external FujiNet"));
    external->setChecked(astrosession_get_int(session, "fujinet_external", 0) != 0);
    auto *externalAddr = new QLineEdit(
        QString::fromUtf8(astrosession_get_str(session, "fujinet_external_addr", "")));
    externalAddr->setPlaceholderText(QStringLiteral("127.0.0.1:11500"));
    externalAddr->setEnabled(external->isChecked());
    QObject::connect(external, &QCheckBox::toggled, externalAddr, &QWidget::setEnabled);
    fujinetForm->addRow(external);
    fujinetForm->addRow(QStringLiteral("Address:port"), externalAddr);
    auto *hint = new QLabel(QStringLiteral(
        "Skips the built-in FujiNet and connects to another instance: its "
        "[BOIP] listener's host and port."));
    hint->setWordWrap(true);
    hint->setStyleSheet(kGray);
    fujinetForm->addRow(hint);
    mv->addWidget(fujinetBox);
    mv->addStretch(1);
    tabs->addTab(machine, "Machine");

    /* The Input tab: the pads, and the controls' two shortcuts. Keys and
     * gamepad buttons are re-mapped in the Controllers window (Map). */
    auto *input = new QWidget;
    auto *iv = new QVBoxLayout(input);
    auto *padsBox = new QGroupBox(QStringLiteral("Gamepads"));
    auto *padsLayout = new QVBoxLayout(padsBox);
    auto *enablePads = new QCheckBox(QStringLiteral("Enable gamepads (restarts the machine)"));
    enablePads->setChecked(astrosession_get_int(session, "enable_gamepad", 1) != 0);
    padsLayout->addWidget(enablePads);
    auto *padsNote = new QLabel(QStringLiteral(
        "The first pad plays player 1, the next player 2 and so on, unless one is chosen here."));
    padsNote->setWordWrap(true);
    padsNote->setStyleSheet(kGray);
    padsLayout->addWidget(padsNote);
    auto *padForm = new QFormLayout;
    padsLayout->addLayout(padForm);
    iv->addWidget(padsBox);
    std::vector<QWidget *> padRows;
    unsigned padGen = astrosession_gamepad_generation(session);
    fillPadRows(padForm, padRows, session);
    auto *padTimer = new QTimer(&dlg);
    QObject::connect(padTimer, &QTimer::timeout, &dlg, [&] {
        const unsigned gen = astrosession_gamepad_generation(session);
        if (gen == padGen) return;
        padGen = gen;
        fillPadRows(padForm, padRows, session);
    });
    padTimer->start(1000);

    auto *controls = new QGroupBox(QStringLiteral("Controls"));
    auto *controlsLayout = new QVBoxLayout(controls);
    auto *controlsNote = new QLabel(QStringLiteral(
        "Keys and gamepad buttons are re-mapped in the Controllers window (Map)."));
    controlsNote->setWordWrap(true);
    controlsNote->setStyleSheet(kGray);
    controlsLayout->addWidget(controlsNote);
    auto *controlsRow = new QHBoxLayout;
    auto *resetControls = new QPushButton(QStringLiteral("Reset Controls"));
    resetControls->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    auto *openWindow = new QPushButton(QStringLiteral("Open Controllers"));
    openWindow->setToolTip(QStringLiteral("Close Preferences and open the Controllers window"));
    controlsRow->addWidget(resetControls);
    controlsRow->addWidget(openWindow);
    controlsRow->addStretch(1);
    controlsLayout->addLayout(controlsRow);
    auto *controlsState = new QLabel;
    controlsLayout->addWidget(controlsState);
    iv->addWidget(controls);
    iv->addStretch(1);
    QObject::connect(resetControls, &QPushButton::clicked, &dlg, [session, controlsState] {
        bindings_reset_defaults(session);
        controlsState->setText(QStringLiteral("Every key and gamepad button is back to its default."));
    });
    QObject::connect(openWindow, &QPushButton::clicked, &dlg, [&dlg, openControllers] {
        if (openControllers) *openControllers = true;
        dlg.accept();
    });
    const int inputTab = tabs->addTab(input, "Input");

    State st{ &dlg, session, bios, {}, std::move(imported) };
    const int romsIndex = tabs->addTab(romsPage(st), "ROMs");
    refresh(st);
    if (startTab == QLatin1String("ROMs"))
        tabs->setCurrentIndex(romsIndex);
    else if (startTab == QLatin1String("Input"))
        tabs->setCurrentIndex(inputTab);
    outer->addWidget(tabs);

    /* Help, F1 and Ctrl+Shift+H (the app's shortcuts don't reach a modal dialog) */
    auto pageContext = [inputTab, romsIndex](int i) {
        return i == romsIndex ? QStringLiteral("prefs/ROMs")
             : i == inputTab  ? QStringLiteral("prefs/Input") : QStringLiteral("prefs/Machine");
    };
    dlg.setProperty("astroHelpContext", pageContext(tabs->currentIndex()));
    QObject::connect(tabs, &QTabWidget::currentChanged, &dlg,
                     [&dlg, pageContext](int i) { dlg.setProperty("astroHelpContext", pageContext(i)); });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                                         QDialogButtonBox::Help);
    QObject::connect(buttons, &QDialogButtonBox::helpRequested, &dlg, [&dlg] { astroHelpFor(&dlg); });
    for (const char *key : { "F1", "Ctrl+Shift+H" })
        new QShortcut(QKeySequence(QString::fromLatin1(key)), &dlg, [&dlg] { astroHelpFor(&dlg); });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
        char host[64];
        int port;
        const QByteArray addr = externalAddr->text().trimmed().toUtf8();
        if (external->isChecked() &&
            astrosession_parse_hostport(addr.constData(), host, sizeof host, &port) != 0) {
            QMessageBox::warning(&dlg, QStringLiteral("External FujiNet"),
                                 QStringLiteral("\"%1\" is not an address:port. Example: "
                                                "127.0.0.1:11500")
                                     .arg(externalAddr->text()));
            externalAddr->setFocus();
            return;
        }
        dlg.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    outer->addWidget(buttons);
    dlg.resize(dlg.sizeHint().expandedTo(QSize(560, 420)));

    if (dlg.exec() != QDialog::Accepted)
        return false;

    bool changed = false;
    if (enablePads->isChecked() != (astrosession_get_int(session, "enable_gamepad", 1) != 0)) {
        astrosession_set_int(session, "enable_gamepad", enablePads->isChecked() ? 1 : 0);
        changed = true;   /* the pad thread starts with the machine */
    }
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
    if (external->isChecked() != (astrosession_get_int(session, "fujinet_external", 0) != 0)) {
        astrosession_set_int(session, "fujinet_external", external->isChecked() ? 1 : 0);
        changed = true;
    }
    const QString addr = externalAddr->text().trimmed();
    if (addr != QString::fromUtf8(astrosession_get_str(session, "fujinet_external_addr", ""))) {
        astrosession_set_str(session, "fujinet_external_addr", addr.toUtf8().constData());
        changed = true;
    }
    return changed;
}
