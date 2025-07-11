/* firewall_rules_dialog.cpp
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <config.h>

#include "firewall_rules_dialog.h"
#include <ui_firewall_rules_dialog.h>

#include "epan/packet_info.h"
#include "epan/to_str.h"

#include "ui/all_files_wildcard.h"
#include "ui/firewall_rules.h"
#include "ui/help_url.h"

#include "wsutil/file_util.h"
#include "wsutil/utf8_entities.h"

#include "main_application.h"
#include "ui/qt/widgets/wireshark_file_dialog.h"

#include <QClipboard>
#include <QMessageBox>
#include <QPushButton>
#include <QTextCursor>

// XXX As described in bug 2482, some of the generated rules don't
// make sense. We could generate rules for every conceivable use case,
// but that would add complexity. We could also add controls to let
// users fine-tune rule output, but that would also add complexity.

FirewallRulesDialog::FirewallRulesDialog(QWidget &parent, CaptureFile &cf) :
    WiresharkDialog(parent, cf),
    ui(new Ui::FirewallRulesDialog),
    prod_(0)
{
    ui->setupUi(this);

    setWindowSubtitle(tr("Firewall ACL Rules"));

    ui->buttonBox->button(QDialogButtonBox::Apply)->setText(tr("Copy"));

    file_name_ = cf.fileName(); // XXX Add extension?
    packet_num_ = cf.packetInfo()->num;

    packet_info *pinfo = cf.packetInfo();
    copy_address(&dl_src_, &(pinfo->dl_src));
    copy_address(&dl_dst_, &(pinfo->dl_dst));
    copy_address(&net_src_, &(pinfo->net_src));
    copy_address(&net_dst_, &(pinfo->net_dst));
    ptype_ = pinfo->ptype;
    src_port_ = pinfo->srcport;
    dst_port_ = pinfo->destport;
    int nf_item = 0;

    for (size_t prod = 0; prod < firewall_product_count(); prod++) {
        QString prod_name = firewall_product_name(prod);

        // Default to Netfilter since it's likely the most popular.
        if (prod_name.contains("Netfilter")) nf_item = ui->productComboBox->count();
        ui->productComboBox->addItem(prod_name);
    }
    ui->productComboBox->setCurrentIndex(nf_item);

    ui->buttonBox->button(QDialogButtonBox::Close)->setDefault(true);
}

FirewallRulesDialog::~FirewallRulesDialog()
{
    delete ui;
}

void FirewallRulesDialog::updateWidgets()
{
    WiresharkDialog::updateWidgets();

    QString comment_pfx = firewall_product_comment_prefix(prod_);
    QString rule_hint = firewall_product_rule_hint(prod_);
    QString rule_line;

    rule_line = QStringLiteral("%1 %2 rules for %3, packet %4.")
            .arg(comment_pfx)
            .arg(firewall_product_name(prod_))
            .arg(file_name_)
            .arg(packet_num_);

    if (!rule_hint.isEmpty()) rule_line += " " + rule_hint;

    ui->textBrowser->clear();
    ui->textBrowser->append(rule_line);

    syntax_func v4_func = firewall_product_ipv4_func(prod_);
    syntax_func port_func = firewall_product_port_func(prod_);
    syntax_func v4_port_func = firewall_product_ipv4_port_func(prod_);
    syntax_func mac_func = firewall_product_mac_func(prod_);

    if (v4_func && net_src_.type == AT_IPv4) {
        addRule(tr("IPv4 source address."), v4_func, &net_src_, src_port_);
        addRule(tr("IPv4 destination address."), v4_func, &net_dst_, dst_port_);
    }

    if (port_func && (ptype_ == PT_TCP || ptype_ == PT_UDP)) {
        addRule(tr("Source port."), port_func, &net_src_, src_port_);
        addRule(tr("Destination port."), port_func, &net_dst_, dst_port_);
    }

    if (v4_port_func && net_src_.type == AT_IPv4 &&
            (ptype_ == PT_TCP || ptype_ == PT_UDP)) {
        addRule(tr("IPv4 source address and port."), v4_port_func, &net_src_, src_port_);
        addRule(tr("IPv4 destination address and port."), v4_port_func, &net_dst_, dst_port_);
    }

    if (mac_func && dl_src_.type == AT_ETHER) {
        addRule(tr("MAC source address."), mac_func, &dl_src_, src_port_);
        addRule(tr("MAC destination address."), mac_func, &dl_dst_, dst_port_);
    }

    ui->textBrowser->moveCursor(QTextCursor::Start);

    ui->inboundCheckBox->setEnabled(firewall_product_does_inbound(prod_));
}

#define ADDR_BUF_LEN 200
void FirewallRulesDialog::addRule(QString description, syntax_func rule_func, address *addr, uint32_t port)
{
    if (!rule_func) return;

    char addr_buf[ADDR_BUF_LEN];
    QString comment_pfx = firewall_product_comment_prefix(prod_);
    GString *rule_str = g_string_new("");
    bool inbound = ui->inboundCheckBox->isChecked();
    bool deny = ui->denyCheckBox->isChecked();

    address_to_str_buf(addr, addr_buf, ADDR_BUF_LEN);
    rule_func(rule_str, addr_buf, port, ptype_, inbound, deny);
    ui->textBrowser->append(QString());

    QString comment_line = QStringLiteral("%1 %2").arg(comment_pfx, description);
    ui->textBrowser->append(comment_line);
    ui->textBrowser->append(rule_str->str);

    g_string_free(rule_str, TRUE);
}


void FirewallRulesDialog::on_productComboBox_currentIndexChanged(int new_idx)
{
    prod_ = (size_t) new_idx;
    updateWidgets();
}

void FirewallRulesDialog::on_inboundCheckBox_toggled(bool)
{
    updateWidgets();
}

void FirewallRulesDialog::on_denyCheckBox_toggled(bool)
{
    updateWidgets();
}

void FirewallRulesDialog::on_buttonBox_clicked(QAbstractButton *button)
{
    if (button == ui->buttonBox->button(QDialogButtonBox::Save)) {
        QString save_title = QStringLiteral("Save %1 rules as%2")
            .arg(firewall_product_name(prod_), UTF8_HORIZONTAL_ELLIPSIS);
        QByteArray file_name = WiresharkFileDialog::getSaveFileName(this,
                                                 save_title,
                                                 mainApp->openDialogInitialDir().canonicalPath(),
                                                 tr("Text file (*.txt);;All Files (" ALL_FILES_WILDCARD ")")
                                                 ).toUtf8();
        if (file_name.length() > 0) {
            QFile save_file(file_name);
            QByteArray rule_text = ui->textBrowser->toPlainText().toUtf8();

            if (save_file.open(QIODevice::WriteOnly)) {
                save_file.write(rule_text);
                save_file.close();
            }

            if (save_file.error() != QFile::NoError) {
                QMessageBox::warning(this, tr("Warning"), tr("Unable to save %1").arg(save_file.fileName()));
                return;
            }

            /* Save the directory name for future file dialogs. */
            mainApp->setLastOpenDirFromFilename(file_name);
        }
    } else if (button == ui->buttonBox->button(QDialogButtonBox::Apply)) {
        if (ui->textBrowser->textCursor().hasSelection()) {
            ui->textBrowser->copy();
        } else {
            mainApp->clipboard()->setText(ui->textBrowser->toPlainText());
        }
    }
}

void FirewallRulesDialog::on_buttonBox_helpRequested()
{
    mainApp->helpTopicAction(HELP_FIREWALL_DIALOG);
}
