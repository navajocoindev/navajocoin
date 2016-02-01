#include "sendcoinsdialog.h"
#include "ui_sendcoinsdialog.h"

#include "init.h"
#include "walletmodel.h"
#include "addresstablemodel.h"
#include "addressbookpage.h"

#include "bitcoinunits.h"
#include "addressbookpage.h"
#include "optionsmodel.h"
#include "sendcoinsentry.h"
#include "guiutil.h"
#include "askpassphrasedialog.h"

#include "coincontrol.h"
#include "coincontroldialog.h"

#include <QMessageBox>
#include <QLocale>
#include <QTextDocument>
#include <QScrollBar>
#include <QClipboard>

#include <QNetworkRequest>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTableWidgetItem>
#include <QtGui>
#include <QDebug>

#include <algorithm>
#include <iterator>

#include <openssl/aes.h>
#include <QSslSocket>

#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <stdio.h>

int padding = RSA_PKCS1_PADDING;

SendCoinsDialog::SendCoinsDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::SendCoinsDialog),
    model(0)
{
    ui->setupUi(this);

#ifdef Q_OS_MAC // Icons on push buttons are very uncommon on Mac
    ui->addButton->setIcon(QIcon());
    ui->clearButton->setIcon(QIcon());
    ui->sendButton->setIcon(QIcon());
#endif

#if QT_VERSION >= 0x040700
    /* Do not move this to the XML file, Qt before 4.7 will choke on it */
    //ui->editTxComment->setPlaceholderText(tr("Enter the Destination Address (Note: ONLY USE IF ANONIMIZING THE TRANSACTION)"));
#endif

#if QT_VERSION >= 0x040700
    /* Do not move this to the XML file, Qt before 4.7 will choke on it */
    ui->lineEditCoinControlChange->setPlaceholderText(tr("Enter a NAV address (e.g. sjz75uKHzUQJnSdzvpiigEGxseKkDhQToX)"));
#endif

    addEntry();

    //connect(ui->addButton, SIGNAL(clicked()), this, SLOT(addEntry()));
    connect(ui->clearButton, SIGNAL(clicked()), this, SLOT(clear()));

    // Coin Control
    ui->lineEditCoinControlChange->setFont(GUIUtil::bitcoinAddressFont());
    connect(ui->pushButtonCoinControl, SIGNAL(clicked()), this, SLOT(coinControlButtonClicked()));
    connect(ui->checkBoxCoinControlChange, SIGNAL(stateChanged(int)), this, SLOT(coinControlChangeChecked(int)));
    connect(ui->lineEditCoinControlChange, SIGNAL(textEdited(const QString &)), this, SLOT(coinControlChangeEdited(const QString &)));

    // Coin Control: clipboard actions
    QAction *clipboardQuantityAction = new QAction(tr("Copy quantity"), this);
    QAction *clipboardAmountAction = new QAction(tr("Copy amount"), this);
    QAction *clipboardFeeAction = new QAction(tr("Copy fee"), this);
    QAction *clipboardAfterFeeAction = new QAction(tr("Copy after fee"), this);
    QAction *clipboardBytesAction = new QAction(tr("Copy bytes"), this);
    QAction *clipboardPriorityAction = new QAction(tr("Copy priority"), this);
    QAction *clipboardLowOutputAction = new QAction(tr("Copy low output"), this);
    QAction *clipboardChangeAction = new QAction(tr("Copy change"), this);
    connect(clipboardQuantityAction, SIGNAL(triggered()), this, SLOT(coinControlClipboardQuantity()));
    connect(clipboardAmountAction, SIGNAL(triggered()), this, SLOT(coinControlClipboardAmount()));
    connect(clipboardFeeAction, SIGNAL(triggered()), this, SLOT(coinControlClipboardFee()));
    connect(clipboardAfterFeeAction, SIGNAL(triggered()), this, SLOT(coinControlClipboardAfterFee()));
    connect(clipboardBytesAction, SIGNAL(triggered()), this, SLOT(coinControlClipboardBytes()));
    connect(clipboardPriorityAction, SIGNAL(triggered()), this, SLOT(coinControlClipboardPriority()));
    connect(clipboardLowOutputAction, SIGNAL(triggered()), this, SLOT(coinControlClipboardLowOutput()));
    connect(clipboardChangeAction, SIGNAL(triggered()), this, SLOT(coinControlClipboardChange()));
    ui->labelCoinControlQuantity->addAction(clipboardQuantityAction);
    ui->labelCoinControlAmount->addAction(clipboardAmountAction);
    ui->labelCoinControlFee->addAction(clipboardFeeAction);
    ui->labelCoinControlAfterFee->addAction(clipboardAfterFeeAction);
    ui->labelCoinControlBytes->addAction(clipboardBytesAction);
    ui->labelCoinControlPriority->addAction(clipboardPriorityAction);
    ui->labelCoinControlLowOutput->addAction(clipboardLowOutputAction);
    ui->labelCoinControlChange->addAction(clipboardChangeAction);

    fNewRecipientAllowed = true;
}

void SendCoinsDialog::setModel(WalletModel *model)
{
    this->model = model;

    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry)
        {
            entry->setModel(model);
        }
    }
    if(model && model->getOptionsModel())
    {
        setBalance(model->getBalance(), model->getStake(), model->getUnconfirmedBalance(), model->getImmatureBalance());
        connect(model, SIGNAL(balanceChanged(qint64, qint64, qint64, qint64)), this, SLOT(setBalance(qint64, qint64, qint64, qint64)));
        connect(model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));

        // Coin Control
        connect(model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(coinControlUpdateLabels()));
        connect(model->getOptionsModel(), SIGNAL(coinControlFeaturesChanged(bool)), this, SLOT(coinControlFeatureChanged(bool)));
        connect(model->getOptionsModel(), SIGNAL(transactionFeeChanged(qint64)), this, SLOT(coinControlUpdateLabels()));
        ui->frameCoinControl->setVisible(model->getOptionsModel()->getCoinControlFeatures());
        coinControlUpdateLabels();
    }
}

SendCoinsDialog::~SendCoinsDialog()
{
    delete ui;
}

bool SendCoinsDialog::chooseServer(QJsonArray anonServers, QString localHash)
{
    int max = anonServers.size();

    if(anonServers.size() == 0) {
        QMessageBox::warning(this, tr("Anonymous Transaction"),
        tr("Unable to locate a verified Anonymous Transaction Server, please try again later."),
        QMessageBox::Ok, QMessageBox::Ok);
        return false;
    }

    int randomNumber = qrand() % max;

    QJsonObject randomAnon = anonServers.at(randomNumber).toObject();
    bool success = this->testServer(randomAnon["server"].toString(), localHash);

    if(success == false) {
        anonServers.removeAt(randomNumber);
       return this->chooseServer(anonServers, localHash);
    }else{
        return true;
    }
}

bool SendCoinsDialog::testServer(QString serverAddress, QString localHash)
{

    QSslSocket *socket = new QSslSocket(this);
    socket->setPeerVerifyMode(socket->VerifyNone);

    socket->connectToHostEncrypted(serverAddress, 443);

    if(!socket->waitForEncrypted()){
        qDebug() << socket->errorString();
        return false;
    }

    QString reqString = QString("POST /api/check-node HTTP/1.1\r\n" \
                        "Host: %1\r\n" \
                        "Content-Type: application/x-www-form-urlencoded\r\n" \
                        "Connection: Close\r\n\r\n").arg(serverAddress);

    socket->write(reqString.toUtf8());

    while (socket->waitForReadyRead()){

        while(socket->canReadLine()){
            QString line = socket->readLine();
        }

        QString rawReply = socket->readAll();
        QJsonDocument jsonDoc =  QJsonDocument::fromJson(rawReply.toUtf8());
        QJsonObject jsonObject = jsonDoc.object();
        QString type = jsonObject["type"].toString();
        QJsonObject jsonData = jsonObject["data"].toObject();
        QString serverHash = jsonData["hash"].toString();

        if(type == "SUCCESS" && serverHash == localHash) {
            selectedServer = jsonData;
            selectedServerAddress = serverAddress;
            return true;
        } else {
            return false;
        }
    }
}

RSA * SendCoinsDialog::createRSA(unsigned char * key,int isPublic)
{
    RSA *rsa= NULL;
    BIO *keybio ;
    keybio = BIO_new_mem_buf(key, -1);
    if (keybio==NULL)
    {
        printf( "Failed to create key BIO");
        return 0;
    }
    if(isPublic)
    {
        rsa = PEM_read_bio_RSA_PUBKEY(keybio, &rsa,NULL, NULL);
    }
    else
    {
        rsa = PEM_read_bio_RSAPrivateKey(keybio, &rsa,NULL, NULL);
    }
    if(rsa == NULL)
    {
        printf( "Failed to create RSA");
    }

    return rsa;
}

void SendCoinsDialog::printLastError(char *msg)
{
    char * err = malloc(130);;
    ERR_load_crypto_strings();
    ERR_error_string(ERR_get_error(), err);
    qDebug() << QString("%1 ERROR: %2\n").arg(msg).arg(err);
    free(err);
}


int SendCoinsDialog::public_encrypt(unsigned char * data,int data_len,unsigned char * key, unsigned char *encrypted)
{
    RSA * rsa = this->createRSA(key,1);
    int result = RSA_public_encrypt(data_len,data,encrypted,rsa,padding);
    return result;
}

int SendCoinsDialog::private_decrypt(unsigned char * enc_data,int data_len,unsigned char * key, unsigned char *decrypted)
{
    RSA * rsa = createRSA(key,0);
    int  result = RSA_private_decrypt(data_len,enc_data,decrypted,rsa,padding);
    return result;
}

QString SendCoinsDialog::charToString(unsigned char *originalChar){

    QString temp;
    QString convertedString = "";
    int charLength = strlen(originalChar);

    for(int i = 0; i < charLength; i++) {
        temp = QChar(originalChar[i]).toAscii();
        convertedString.append(temp);
    }

    return convertedString;

}

QString SendCoinsDialog::encryptAddress(QString userAddress, QString serverPublicKey) {

    char publicKey[serverPublicKey.size()+1];
    memcpy( publicKey, serverPublicKey.toStdString().c_str() ,serverPublicKey.size());
    publicKey[serverPublicKey.size()] = 0;

    char plainText[userAddress.size()+1];
    memcpy( plainText, userAddress.toStdString().c_str() ,userAddress.size());
    plainText[userAddress.size()] = 0;

    unsigned char  encrypted[4098]={};

    int encrypted_length= this->public_encrypt(plainText,strlen(plainText),publicKey,encrypted);

    if(encrypted_length == -1)
    {
        qDebug() << QString("Public Encrypt failed");
        exit(0);
    } else {
        QString encryptedString = this->charToString(encrypted);
    }

    QByteArray convertedString = QByteArray(encrypted);

    QString encryptedString = convertedString.toBase64();

    return QString(encryptedString);

}

bool SendCoinsDialog::testEncryption(QString txComment){

    QSslSocket *socket = new QSslSocket(this);
    socket->setPeerVerifyMode(socket->VerifyNone);

    socket->connectToHostEncrypted(selectedServerAddress, 443);

    if(!socket->waitForEncrypted()){
        qDebug() << socket->errorString();
        return false;
    }

    QByteArray urlEncoded = QUrl::toPercentEncoding(txComment);

    QString urlEncodedQString = QString(urlEncoded);

    int contentLength = urlEncoded.length() + 10;

    QString reqString = QString("POST /api/check-encryption HTTP/1.1\r\n" \
                        "Host: %1\r\n" \
                        "Content-Type: application/x-www-form-urlencoded\r\n" \
                        "Content-Length: %2\r\n" \
                        "Connection: Close\r\n\r\n" \
                        "encrypted=%3\r\n").arg(selectedServerAddress).arg(contentLength).arg(urlEncodedQString);

    socket->write(reqString.toUtf8());

    while (socket->waitForReadyRead()){

        while(socket->canReadLine()){
            QString line = socket->readLine();
        }

        QString rawReply = socket->readAll();
        QJsonDocument jsonDoc =  QJsonDocument::fromJson(rawReply.toUtf8());
        QJsonObject jsonObject = jsonDoc.object();
        QString type = jsonObject["type"].toString();

        if(type == "SUCCESS") {
            return true;
        } else {
            return false;
        }
    }
}

void SendCoinsDialog::on_sendButton_clicked()
{

    if(ui->anonCheckBox->checkState() == 0){
        QString node = QString("");
        this->sendCoins(node);
    }else{

        QString anonFileContents;
        QString anonFilePath = QString("%1%2%3").arg(GetDefaultDataDir().string().c_str()).arg(QDir::separator()).arg("anon.dat");
        QFile anonFile(anonFilePath);
        anonFile.open(QIODevice::ReadOnly | QIODevice::Text);
        anonFileContents = anonFile.readAll();
        anonFile.close();

        QJsonDocument anonJsonDoc =  QJsonDocument::fromJson(anonFileContents.toUtf8());
        QJsonObject anonJsonObject = anonJsonDoc.object();
        QJsonArray anonServers = anonJsonObject["servers"].toArray();
        QString localHash = anonJsonObject["hash"].toString();
        int max = anonServers.size();

        if(max <= 0) {
            QMessageBox::warning(this, tr("Anonymous Transaction"),
            tr("Your server list appears to be empty, please wait for peer network sync."),
            QMessageBox::Ok, QMessageBox::Ok);
            return;
        }

        //get and verify the entered address

        QList<SendCoinsRecipient> recipients;
        bool valid = true;

        if(!model){
            return;
        }

        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(0)->widget());
        if(entry)
        {
            if(entry->validate())
            {
                recipients.append(entry->getValue());
            }
            else
            {
                valid = false;
            }
        }

        QString qAddress;
        foreach(const SendCoinsRecipient &rcp, recipients){
            qAddress = rcp.address;
        }

        //find an operational server

        bool serverReady = this->chooseServer(anonServers, localHash);

        if(!serverReady || selectedServer.empty()) {
            return;
        }

        //prepare values required to make transaction

        QString publicKey = selectedServer["public_key"].toString();
        QString serverAddress = selectedServer["address"].toString();
        minAmount = selectedServer["min_amount"].toString().toDouble();
        maxAmount = selectedServer["max_amount"].toString().toDouble();
        double txFee = selectedServer["transaction_fee"].toString().toDouble();

        QString txComment = this->encryptAddress(qAddress, publicKey);

        bool decryptionResult = this->testEncryption(txComment);

        if(!decryptionResult) {
            QMessageBox::warning(this, tr("Anonymous Transaction"),
            tr("Unable to verify encryption, please try again."),
            QMessageBox::Ok, QMessageBox::Ok);
            return false;
        }

        model->setAnonDetails(minAmount, maxAmount, txComment);

        QString messageString = QString("Are you sure you want to send these coins through the Navajo Anonymous Network? There will be a %1% transaction fee.").arg(txFee);

        QMessageBox::StandardButton reply;
        reply = QMessageBox::question(this, "Anonymous Transaction", messageString, QMessageBox::Yes|QMessageBox::No);

        if(reply == QMessageBox::Yes){
            this->sendCoins(serverAddress);
        }

    }//else


}//sendButton

void SendCoinsDialog::sendCoins(QString anonNode){

    QList<SendCoinsRecipient> recipients;
    bool valid = true;

    if(!model)
        return;

    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry)
        {
            if(entry->validate())
            {
                recipients.append(entry->getValue());
            }
            else
            {
                valid = false;
            }
        }
    }

    if(!valid || recipients.isEmpty())
    {
        return;
    }

    // Format confirmation message
    QStringList formatted;
    foreach(const SendCoinsRecipient &rcp, recipients)
    {
        formatted.append(tr("<b>%1</b> to %2 (%3)").arg(BitcoinUnits::formatWithUnit(BitcoinUnits::BTC, rcp.amount), Qt::escape(rcp.label), rcp.address));
    }

    fNewRecipientAllowed = false;

    QMessageBox::StandardButton retval = QMessageBox::question(this, tr("Confirm send coins"),
                          tr("Are you sure you want to send %1?").arg(formatted.join(tr(" and "))),
          QMessageBox::Yes|QMessageBox::Cancel,
          QMessageBox::Cancel);

    if(retval != QMessageBox::Yes)
    {
        fNewRecipientAllowed = true;
        return;
    }

    WalletModel::UnlockContext ctx(model->requestUnlock());
    if(!ctx.isValid())
    {
        // Unlock wallet was cancelled
        fNewRecipientAllowed = true;
        return;
    }

    WalletModel::SendCoinsReturn sendstatus;

    if (!model->getOptionsModel() || !model->getOptionsModel()->getCoinControlFeatures())
        sendstatus = model->sendCoins(anonNode, recipients);
    else
        sendstatus = model->sendCoins(anonNode, recipients, CoinControlDialog::coinControl);

    switch(sendstatus.status)
    {
    case WalletModel::InvalidAddress:
        QMessageBox::warning(this, tr("Send Coins"),
            tr("The recipient address is not valid, please recheck."),
            QMessageBox::Ok, QMessageBox::Ok);
        break;
    case WalletModel::InvalidAmount:
        QMessageBox::warning(this, tr("Send Coins"),
            tr("The amount to pay must be larger than 0."),
            QMessageBox::Ok, QMessageBox::Ok);
        break;
    case WalletModel::MinAmount:
        QMessageBox::warning(this, tr("Send Coins"),
            tr("The amount to pay must be larger than %1 NAV.").arg(QString::number(minAmount)),
            QMessageBox::Ok, QMessageBox::Ok);
        break;
    case WalletModel::MaxAmount:
        QMessageBox::warning(this, tr("Send Coins"),
            tr("The amount to pay must be smaller than %1 NAV.").arg(QLocale(QLocale::English).toString(maxAmount, 'f', 0)),
            QMessageBox::Ok, QMessageBox::Ok);
        break;
    case WalletModel::AmountExceedsBalance:
        QMessageBox::warning(this, tr("Send Coins"),
            tr("The amount exceeds your balance."),
            QMessageBox::Ok, QMessageBox::Ok);
        break;
    case WalletModel::AmountWithFeeExceedsBalance:
        QMessageBox::warning(this, tr("Send Coins"),
            tr("The total exceeds your balance when the %1 transaction fee is included.").
            arg(BitcoinUnits::formatWithUnit(BitcoinUnits::BTC, sendstatus.fee)),
            QMessageBox::Ok, QMessageBox::Ok);
        break;
    case WalletModel::DuplicateAddress:
        QMessageBox::warning(this, tr("Send Coins"),
            tr("Duplicate address found, can only send to each address once per send operation."),
            QMessageBox::Ok, QMessageBox::Ok);
        break;
    case WalletModel::TransactionCreationFailed:
        QMessageBox::warning(this, tr("Send Coins"),
            tr("Error: Transaction creation failed."),
            QMessageBox::Ok, QMessageBox::Ok);
        break;
    case WalletModel::TransactionCommitFailed:
        QMessageBox::warning(this, tr("Send Coins"),
            tr("Error: The transaction was rejected. This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here."),
            QMessageBox::Ok, QMessageBox::Ok);
        break;
    case WalletModel::Aborted: // User aborted, nothing to do
        break;
    case WalletModel::OK:
        accept();
        CoinControlDialog::coinControl->UnSelectAll();
        coinControlUpdateLabels();
        break;
    }
    fNewRecipientAllowed = true;
}

void SendCoinsDialog::clear()
{
    //ui->editTxComment->clear();

    // Remove entries until only one left
    while(ui->entries->count())
    {
        delete ui->entries->takeAt(0)->widget();
    }
    addEntry();

    updateRemoveEnabled();

    ui->sendButton->setDefault(true);
}

void SendCoinsDialog::reject()
{
    clear();
}

void SendCoinsDialog::accept()
{
    clear();
}

SendCoinsEntry *SendCoinsDialog::addEntry()
{
    SendCoinsEntry *entry = new SendCoinsEntry(this);
    entry->setModel(model);
    ui->entries->addWidget(entry);
    connect(entry, SIGNAL(removeEntry(SendCoinsEntry*)), this, SLOT(removeEntry(SendCoinsEntry*)));
    connect(entry, SIGNAL(payAmountChanged()), this, SLOT(coinControlUpdateLabels()));

    updateRemoveEnabled();

    // Focus the field, so that entry can start immediately
    entry->clear();
    entry->setFocus();
    ui->scrollAreaWidgetContents->resize(ui->scrollAreaWidgetContents->sizeHint());
    QCoreApplication::instance()->processEvents();
    QScrollBar* bar = ui->scrollArea->verticalScrollBar();
    if(bar)
        bar->setSliderPosition(bar->maximum());
    return entry;
}

void SendCoinsDialog::updateRemoveEnabled()
{
    // Remove buttons are enabled as soon as there is more than one send-entry
    bool enabled = (ui->entries->count() > 1);
    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry)
        {
            entry->setRemoveEnabled(enabled);
        }
    }
    setupTabChain(0);
    coinControlUpdateLabels();
}

void SendCoinsDialog::removeEntry(SendCoinsEntry* entry)
{
    delete entry;
    updateRemoveEnabled();
}

QWidget *SendCoinsDialog::setupTabChain(QWidget *prev)
{
    //QWidget::setTabOrder(prev, ui->editTxComment);

    //prev = ui->editTxComment;

    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry)
        {
            prev = entry->setupTabChain(prev);
        }
    }
    //QWidget::setTabOrder(prev, ui->addButton);
    //QWidget::setTabOrder(ui->addButton, ui->sendButton);
    //QWidget::setTabOrder(ui->sendButton);
    return ui->sendButton;
}

void SendCoinsDialog::pasteEntry(const SendCoinsRecipient &rv)
{
    if(!fNewRecipientAllowed)
        return;

    SendCoinsEntry *entry = 0;
    // Replace the first entry if it is still unused
    if(ui->entries->count() == 1)
    {
        SendCoinsEntry *first = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(0)->widget());
        if(first->isClear())
        {
            entry = first;
        }
    }
    if(!entry)
    {
        entry = addEntry();
    }

    entry->setValue(rv);
}

bool SendCoinsDialog::handleURI(const QString &uri)
{
    SendCoinsRecipient rv;
    // URI has to be valid
    if (GUIUtil::parseBitcoinURI(uri, &rv))
    {
        CBitcoinAddress address(rv.address.toStdString());
        if (!address.IsValid())
            return false;
        pasteEntry(rv);
        return true;
    }

    return false;
}

void SendCoinsDialog::setBalance(qint64 balance, qint64 stake, qint64 unconfirmedBalance, qint64 immatureBalance)
{
    Q_UNUSED(stake);
    Q_UNUSED(unconfirmedBalance);
    Q_UNUSED(immatureBalance);
    if(!model || !model->getOptionsModel())
        return;

    int unit = model->getOptionsModel()->getDisplayUnit();
    ui->labelBalance->setText(BitcoinUnits::formatWithUnit(unit, balance));
}

void SendCoinsDialog::updateDisplayUnit()
{
    if(model && model->getOptionsModel())
    {
        // Update labelBalance with the current balance and the current unit
        ui->labelBalance->setText(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), model->getBalance()));
    }
}

// Coin Control: copy label "Quantity" to clipboard
void SendCoinsDialog::coinControlClipboardQuantity()
{
    QApplication::clipboard()->setText(ui->labelCoinControlQuantity->text());
}

// Coin Control: copy label "Amount" to clipboard
void SendCoinsDialog::coinControlClipboardAmount()
{
    QApplication::clipboard()->setText(ui->labelCoinControlAmount->text().left(ui->labelCoinControlAmount->text().indexOf(" ")));
}

// Coin Control: copy label "Fee" to clipboard
void SendCoinsDialog::coinControlClipboardFee()
{
    QApplication::clipboard()->setText(ui->labelCoinControlFee->text().left(ui->labelCoinControlFee->text().indexOf(" ")));
}

// Coin Control: copy label "After fee" to clipboard
void SendCoinsDialog::coinControlClipboardAfterFee()
{
    QApplication::clipboard()->setText(ui->labelCoinControlAfterFee->text().left(ui->labelCoinControlAfterFee->text().indexOf(" ")));
}

// Coin Control: copy label "Bytes" to clipboard
void SendCoinsDialog::coinControlClipboardBytes()
{
    QApplication::clipboard()->setText(ui->labelCoinControlBytes->text());
}

// Coin Control: copy label "Priority" to clipboard
void SendCoinsDialog::coinControlClipboardPriority()
{
    QApplication::clipboard()->setText(ui->labelCoinControlPriority->text());
}

// Coin Control: copy label "Low output" to clipboard
void SendCoinsDialog::coinControlClipboardLowOutput()
{
    QApplication::clipboard()->setText(ui->labelCoinControlLowOutput->text());
}

// Coin Control: copy label "Change" to clipboard
void SendCoinsDialog::coinControlClipboardChange()
{
    QApplication::clipboard()->setText(ui->labelCoinControlChange->text().left(ui->labelCoinControlChange->text().indexOf(" ")));
}

// Coin Control: settings menu - coin control enabled/disabled by user
void SendCoinsDialog::coinControlFeatureChanged(bool checked)
{
    ui->frameCoinControl->setVisible(checked);

    if (!checked && model) // coin control features disabled
        CoinControlDialog::coinControl->SetNull();
}

// Coin Control: button inputs -> show actual coin control dialog
void SendCoinsDialog::coinControlButtonClicked()
{
    CoinControlDialog dlg;
    dlg.setModel(model);
    dlg.exec();
    coinControlUpdateLabels();
}

// Coin Control: checkbox custom change address
void SendCoinsDialog::coinControlChangeChecked(int state)
{
    if (model)
    {
        if (state == Qt::Checked)
            CoinControlDialog::coinControl->destChange = CBitcoinAddress(ui->lineEditCoinControlChange->text().toStdString()).Get();
        else
            CoinControlDialog::coinControl->destChange = CNoDestination();
    }

    ui->lineEditCoinControlChange->setEnabled((state == Qt::Checked));
    ui->labelCoinControlChangeLabel->setEnabled((state == Qt::Checked));
}

// Coin Control: custom change address changed
void SendCoinsDialog::coinControlChangeEdited(const QString & text)
{
    if (model)
    {
        CoinControlDialog::coinControl->destChange = CBitcoinAddress(text.toStdString()).Get();

        // label for the change address
        ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:black;}");
        if (text.isEmpty())
            ui->labelCoinControlChangeLabel->setText("");
        else if (!CBitcoinAddress(text.toStdString()).IsValid())
        {
            ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:red;}");
            ui->labelCoinControlChangeLabel->setText(tr("WARNING: Invalid NAV address"));
        }
        else
        {
            QString associatedLabel = model->getAddressTableModel()->labelForAddress(text);
            if (!associatedLabel.isEmpty())
                ui->labelCoinControlChangeLabel->setText(associatedLabel);
            else
            {
                CPubKey pubkey;
                CKeyID keyid;
                CBitcoinAddress(text.toStdString()).GetKeyID(keyid);
                if (model->getPubKey(keyid, pubkey))
                    ui->labelCoinControlChangeLabel->setText(tr("(no label)"));
                else
                {
                    ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:red;}");
                    ui->labelCoinControlChangeLabel->setText(tr("WARNING: unknown change address"));
                }
            }
        }
    }
}

// Coin Control: update labels
void SendCoinsDialog::coinControlUpdateLabels()
{
    if (!model || !model->getOptionsModel() || !model->getOptionsModel()->getCoinControlFeatures())
        return;

    // set pay amounts
    CoinControlDialog::payAmounts.clear();
    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry)
            CoinControlDialog::payAmounts.append(entry->getValue().amount);
    }

    if (CoinControlDialog::coinControl->HasSelected())
    {
        // actual coin control calculation
        CoinControlDialog::updateLabels(model, this);

        // show coin control stats
        ui->labelCoinControlAutomaticallySelected->hide();
        ui->widgetCoinControl->show();
    }
    else
    {
        // hide coin control stats
        ui->labelCoinControlAutomaticallySelected->show();
        ui->widgetCoinControl->hide();
        ui->labelCoinControlInsuffFunds->hide();
    }
}
