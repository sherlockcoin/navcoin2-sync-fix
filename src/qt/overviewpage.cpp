#include "overviewpage.h"
#include "ui_overviewpage.h"

#include "clientmodel.h"
#include "anonsend.h"
#include "anonsendconfig.h"
#include "walletmodel.h"
#include "bitcoinunits.h"
#include "optionsmodel.h"
#include "transactiontablemodel.h"
#include "transactionfilterproxy.h"
#include "guiutil.h"
#include "guiconstants.h"

#include <QAbstractItemDelegate>
#include <QPainter>
#include <QTimer>
#include <QDebug>
#include <QScrollArea>
#include <QScroller>

#define DECORATION_SIZE 64
#define NUM_ITEMS 6

class TxViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    TxViewDelegate(): QAbstractItemDelegate(), unit(BitcoinUnits::BTC)
    {

    }

    inline void paint(QPainter *painter, const QStyleOptionViewItem &option,
                      const QModelIndex &index ) const
    {
        painter->save();

        QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        QRect mainRect = option.rect;
        QRect decorationRect(mainRect.topLeft(), QSize(DECORATION_SIZE, DECORATION_SIZE));
        int xspace = DECORATION_SIZE + 8;
        int ypad = 6;
        int halfheight = (mainRect.height() - 2*ypad)/2;
        QRect amountRect(mainRect.left() + xspace, mainRect.top()+ypad, mainRect.width() - xspace, halfheight);
        QRect addressRect(mainRect.left() + xspace, mainRect.top()+ypad+halfheight, mainRect.width() - xspace, halfheight);
        icon.paint(painter, decorationRect);

        QDateTime date = index.data(TransactionTableModel::DateRole).toDateTime();
        QString address = index.data(Qt::DisplayRole).toString();
        qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        bool confirmed = index.data(TransactionTableModel::ConfirmedRole).toBool();
        QVariant value = index.data(Qt::ForegroundRole);
        QColor foreground = option.palette.color(QPalette::Text);
        if(qVariantCanConvert<QColor>(value))
        {
            foreground = qvariant_cast<QColor>(value);
        }

        painter->setPen(fUseBlackTheme ? QColor(255, 255, 255) : foreground);
        painter->drawText(addressRect, Qt::AlignLeft|Qt::AlignVCenter, address);

        if(amount < 0)
        {
            foreground = COLOR_NEGATIVE;
        }
        else if(!confirmed)
        {
            foreground = COLOR_UNCONFIRMED;
        }
        else
        {
            foreground = option.palette.color(QPalette::Text);
        }
        painter->setPen(fUseBlackTheme ? QColor(255, 255, 255) : foreground);
        QString amountText = BitcoinUnits::formatWithUnit(unit, amount, true);
        if(!confirmed)
        {
            amountText = QString("[") + amountText + QString("]");
        }
        painter->drawText(amountRect, Qt::AlignRight|Qt::AlignVCenter, amountText);

        painter->setPen(fUseBlackTheme ? QColor(96, 101, 110) : option.palette.color(QPalette::Text));
        painter->drawText(amountRect, Qt::AlignLeft|Qt::AlignVCenter, GUIUtil::dateTimeStr(date));

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        return QSize(DECORATION_SIZE, DECORATION_SIZE);
    }

    int unit;

};
#include "overviewpage.moc"

OverviewPage::OverviewPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::OverviewPage),
    clientModel(0),
    walletModel(0),
    currentBalance(-1),
    currentStake(0),
    currentUnconfirmedBalance(-1),
    currentImmatureBalance(-1),
    txdelegate(new TxViewDelegate()),
    filter(0)
{
    ui->setupUi(this);

    ui->frameAnonsend->setVisible(false);  // Hide anonsend features

    QScroller::grabGesture(ui->scrollArea, QScroller::LeftMouseButtonGesture);
    ui->scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    ui->columnTwoWidget->setContentsMargins(0,0,0,0);
    ui->columnTwoWidget->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::MinimumExpanding);
    ui->columnTwoWidget->setMinimumWidth(300);

    // Recent transactions
    ui->listTransactions->setItemDelegate(txdelegate);
    ui->listTransactions->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listTransactions->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);
    ui->listTransactions->setMinimumWidth(350);

    connect(ui->listTransactions, SIGNAL(clicked(QModelIndex)), this, SLOT(handleTransactionClicked(QModelIndex)));

    // init "out of sync" warning labels
    ui->labelWalletStatus->setText("(" + tr("out of sync") + ")");
    ui->labelTransactionsStatus->setText("(" + tr("out of sync") + ")");

    showingAnonSendMessage = 0;
    anonsendActionCheck = 0;
    lastNewBlock = 0;

    if(fLiteMode){
        ui->frameAnonsend->setVisible(false);
    } else {
	qDebug() << "Anon Send Status Timer";
        timer = new QTimer(this);
        connect(timer, SIGNAL(timeout()), this, SLOT(anonSendStatus()));
	if(!GetBoolArg("-reindexaddr", false))
            timer->start(60000);
    }

    if(fINode || fLiteMode){
        ui->toggleAnonsend->setText("(" + tr("Disabled") + ")");
        ui->toggleAnonsend->setEnabled(false);
    }else if(!fEnableAnonsend){
        ui->toggleAnonsend->setText(tr("Start Anonsend"));
    } else {
        ui->toggleAnonsend->setText(tr("Stop Anonsend"));
    }

    // start with displaying the "out of sync" warnings
    showOutOfSyncWarning(true);

    if (fUseBlackTheme)
    {
        const char* whiteLabelQSS = "QLabel { color: rgb(255,255,255); }";
        ui->labelBalance->setStyleSheet(whiteLabelQSS);
        ui->labelStake->setStyleSheet(whiteLabelQSS);
        ui->labelUnconfirmed->setStyleSheet(whiteLabelQSS);
        ui->labelImmature->setStyleSheet(whiteLabelQSS);
        ui->labelTotal->setStyleSheet(whiteLabelQSS);
    }
}

void OverviewPage::handleTransactionClicked(const QModelIndex &index)
{
    if(filter)
        emit transactionClicked(filter->mapToSource(index));
}

OverviewPage::~OverviewPage()
{
    delete ui;
}

void OverviewPage::setBalance(qint64 balance, qint64 stake, qint64 unconfirmedBalance, qint64 immatureBalance, qint64 anonymizedBalance)
{
    int unit = walletModel->getOptionsModel()->getDisplayUnit();
    currentBalance = balance;
    currentStake = stake;
    currentUnconfirmedBalance = unconfirmedBalance;
    currentImmatureBalance = immatureBalance;
    currentAnonymizedBalance = anonymizedBalance;
    ui->labelBalance->setText(BitcoinUnits::formatWithUnit(unit, balance));
    ui->labelStake->setText(BitcoinUnits::formatWithUnit(unit, stake));
    ui->labelUnconfirmed->setText(BitcoinUnits::formatWithUnit(unit, unconfirmedBalance));
    ui->labelImmature->setText(BitcoinUnits::formatWithUnit(unit, immatureBalance));
    ui->labelTotal->setText(BitcoinUnits::formatWithUnit(unit, balance + stake + unconfirmedBalance + immatureBalance));
    ui->labelAnonymized->setText(BitcoinUnits::formatWithUnit(unit, anonymizedBalance));

    // only show immature (newly mined) balance if it's non-zero, so as not to complicate things
    // for the non-mining users
    bool showImmature = immatureBalance != 0;
    ui->labelImmature->setVisible(showImmature);
    ui->labelImmatureText->setVisible(showImmature);

    if(cachedTxLocks != nCompleteTXLocks){
        cachedTxLocks = nCompleteTXLocks;
        ui->listTransactions->update();
    }
}

void OverviewPage::setClientModel(ClientModel *model)
{
    this->clientModel = model;
    if(model)
    {
        // Show warning if this is a prerelease version
        connect(model, SIGNAL(alertsChanged(QString)), this, SLOT(updateAlerts(QString)));
        updateAlerts(model->getStatusBarWarnings());
    }
}

void OverviewPage::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
    if(model && model->getOptionsModel())
    {
        // Set up transaction list
        filter = new TransactionFilterProxy();
        filter->setSourceModel(model->getTransactionTableModel());
        filter->setLimit(NUM_ITEMS);
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->setShowInactive(false);
        filter->sort(TransactionTableModel::Status, Qt::DescendingOrder);

        ui->listTransactions->setModel(filter);
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);

        // Keep up to date with wallet
        setBalance(model->getBalance(), model->getStake(), model->getUnconfirmedBalance(), model->getImmatureBalance(), model->getAnonymizedBalance());
        connect(model, SIGNAL(balanceChanged(qint64, qint64, qint64, qint64, qint64)), this, SLOT(setBalance(qint64, qint64, qint64, qint64, qint64)));

        connect(model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));

        connect(ui->anonsendAuto, SIGNAL(clicked()), this, SLOT(anonsendAuto()));
        connect(ui->anonsendReset, SIGNAL(clicked()), this, SLOT(anonsendReset()));
        connect(ui->toggleAnonsend, SIGNAL(clicked()), this, SLOT(toggleAnonsend()));
    }

    // update the display unit, to not use the default ("BTC")
    updateDisplayUnit();
}

void OverviewPage::updateDisplayUnit()
{
    if(walletModel && walletModel->getOptionsModel())
    {
        if(currentBalance != -1)
            setBalance(currentBalance, walletModel->getStake(), currentUnconfirmedBalance, currentImmatureBalance, currentAnonymizedBalance);

        // Update txdelegate->unit with the current unit
        txdelegate->unit = walletModel->getOptionsModel()->getDisplayUnit();

        ui->listTransactions->update();
    }
}

void OverviewPage::updateAlerts(const QString &warnings)
{
    this->ui->labelAlerts->setVisible(!warnings.isEmpty());
    this->ui->labelAlerts->setText(warnings);
}

void OverviewPage::showOutOfSyncWarning(bool fShow)
{
    ui->labelWalletStatus->setVisible(fShow);
    ui->labelTransactionsStatus->setVisible(fShow);
}



void OverviewPage::updateAnonsendProgress()
{
    qDebug() << "updateAnonsendProgress()";
    if(IsInitialBlockDownload()) return;
    
    qDebug() << "updateAnonsendProgress() getbalance";
    int64_t nBalance = pwalletMain->GetBalance();
    if(nBalance == 0)
    {
        ui->anonsendProgress->setValue(0);
        QString s(tr("No inputs detected"));
        ui->anonsendProgress->setToolTip(s);
        return;
    }

    //get denominated unconfirmed inputs
    if(pwalletMain->GetDenominatedBalance(true, true) > 0)
    {
        QString s(tr("Found unconfirmed denominated outputs, will wait till they confirm to recalculate."));
        ui->anonsendProgress->setToolTip(s);
        return;
    }

    //Get the anon threshold
    int64_t nMaxToAnonymize = nAnonymizeNavCoinAmount*COIN;

    // If it's more than the wallet amount, limit to that.
    if(nMaxToAnonymize > nBalance) nMaxToAnonymize = nBalance;

    if(nMaxToAnonymize == 0) return;

    // calculate parts of the progress, each of them shouldn't be higher than 1:
    // mixing progress of denominated balance
    int64_t denominatedBalance = pwalletMain->GetDenominatedBalance();
    float denomPart = 0;
    if(denominatedBalance > 0)
    {
        denomPart = (float)pwalletMain->GetNormalizedAnonymizedBalance() / pwalletMain->GetDenominatedBalance();
        denomPart = denomPart > 1 ? 1 : denomPart;
    }

    // % of fully anonymized balance
    float anonPart = 0;
    if(nMaxToAnonymize > 0)
    {
        anonPart = (float)pwalletMain->GetAnonymizedBalance() / nMaxToAnonymize;
        // if anonPart is > 1 then we are done, make denomPart equal 1 too
        anonPart = anonPart > 1 ? (denomPart = 1, 1) : anonPart;
    }

    // apply some weights to them (sum should be <=100) and calculate the whole progress
    int progress = 80 * denomPart + 20 * anonPart;
    if(progress > 100) progress = 100;

    ui->anonsendProgress->setValue(progress);

    std::ostringstream convert;
    convert << "Progress: " << progress << "%, inputs have an average of " << pwalletMain->GetAverageAnonymizedRounds() << " of " << nAnonsendRounds << " rounds";
    QString s(convert.str().c_str());
    ui->anonsendProgress->setToolTip(s);
}


void OverviewPage::anonSendStatus()
{
    int nBestHeight = pindexBest->nHeight;

    if(nBestHeight != anonSendPool.cachedNumBlocks)
    {
        //we we're processing lots of blocks, we'll just leave
        if(GetTime() - lastNewBlock < 10) return;
        lastNewBlock = GetTime();

        updateAnonsendProgress();

        QString strSettings(" " + tr("Rounds"));
        strSettings.prepend(QString::number(nAnonsendRounds)).prepend(" / ");
        strSettings.prepend(BitcoinUnits::formatWithUnit(
            walletModel->getOptionsModel()->getDisplayUnit(),
            nAnonymizeNavCoinAmount * COIN)
        );

        ui->labelAmountRounds->setText(strSettings);
    }

    if(!fEnableAnonsend) {
        if(nBestHeight != anonSendPool.cachedNumBlocks)
        {
            anonSendPool.cachedNumBlocks = nBestHeight;

            ui->anonsendEnabled->setText(tr("Disabled"));
            ui->anonsendStatus->setText("");
            ui->toggleAnonsend->setText(tr("Start Anonsend"));
        }

        return;
    }

    // check anonsend status and unlock if needed
    if(nBestHeight != anonSendPool.cachedNumBlocks)
    {
        // Balance and number of transactions might have changed
        anonSendPool.cachedNumBlocks = nBestHeight;

        /* *******************************************************/

        ui->anonsendEnabled->setText(tr("Enabled"));
    }

    int state = anonSendPool.GetState();
    int entries = anonSendPool.GetEntriesCount();
    int accepted = anonSendPool.GetLastEntryAccepted();

    /* ** @TODO this string creation really needs some clean ups ---vertoe ** */
    std::ostringstream convert;

    if(state == POOL_STATUS_ACCEPTING_ENTRIES) {
        if(entries == 0) {
            if(anonSendPool.strAutoDenomResult.size() == 0){
                convert << tr("Anonsend is idle.").toStdString();
            } else {
                convert << anonSendPool.strAutoDenomResult;
            }
            showingAnonSendMessage = 0;
        } else if (accepted == 1) {
            convert << tr("Anonsend request complete: Your transaction was accepted into the pool!").toStdString();
            if(showingAnonSendMessage % 10 > 8) {
                anonSendPool.lastEntryAccepted = 0;
                showingAnonSendMessage = 0;
            }
        } else {
            if(showingAnonSendMessage % 70 <= 40) convert << tr("Submitted following entries to inode:").toStdString() << " " << entries << "/" << anonSendPool.GetMaxPoolTransactions();
            else if(showingAnonSendMessage % 70 <= 50) convert << tr("Submitted to inode, Waiting for more entries").toStdString() << " (" << entries << "/" << anonSendPool.GetMaxPoolTransactions() << " ) .";
            else if(showingAnonSendMessage % 70 <= 60) convert << tr("Submitted to inode, Waiting for more entries").toStdString() << " (" << entries << "/" << anonSendPool.GetMaxPoolTransactions() << " ) ..";
            else if(showingAnonSendMessage % 70 <= 70) convert << tr("Submitted to inode, Waiting for more entries").toStdString() << " (" << entries << "/" << anonSendPool.GetMaxPoolTransactions() << " ) ...";
        }
    } else if(state == POOL_STATUS_SIGNING) {
        if(showingAnonSendMessage % 70 <= 10) convert << tr("Found enough users, signing ...").toStdString();
        else if(showingAnonSendMessage % 70 <= 20) convert << tr("Found enough users, signing ( waiting. )").toStdString();
        else if(showingAnonSendMessage % 70 <= 30) convert << tr("Found enough users, signing ( waiting.. )").toStdString();
        else if(showingAnonSendMessage % 70 <= 40) convert << tr("Found enough users, signing ( waiting... )").toStdString();
    } else if(state == POOL_STATUS_TRANSMISSION) {
        convert << tr("Transmitting final transaction.").toStdString();
    } else if (state == POOL_STATUS_IDLE) {
        convert << tr("Anonsend is idle.").toStdString();
    } else if (state == POOL_STATUS_FINALIZE_TRANSACTION) {
        convert << tr("Finalizing transaction.").toStdString();
    } else if(state == POOL_STATUS_ERROR) {
        convert << tr("Anonsend request incomplete:").toStdString() << " " << anonSendPool.lastMessage << ". " << tr("Will retry...").toStdString();
    } else if(state == POOL_STATUS_SUCCESS) {
        convert << tr("Anonsend request complete:").toStdString() << " " << anonSendPool.lastMessage;
    } else if(state == POOL_STATUS_QUEUE) {
        if(showingAnonSendMessage % 70 <= 50) convert << tr("Submitted to inode, waiting in queue .").toStdString();
        else if(showingAnonSendMessage % 70 <= 60) convert << tr("Submitted to inode, waiting in queue ..").toStdString();
        else if(showingAnonSendMessage % 70 <= 70) convert << tr("Submitted to inode, waiting in queue ...").toStdString();
    } else {
        convert << tr("Unknown state:").toStdString() << " id = " << state;
    }

    if(state == POOL_STATUS_ERROR || state == POOL_STATUS_SUCCESS) anonSendPool.Check();

    QString s(convert.str().c_str());
    s = tr("Last Anonsend message:\n") + s;

    if(s != ui->anonsendStatus->text())
        LogPrintf("Last Anonsend message: %s\n", convert.str().c_str());

    ui->anonsendStatus->setText(s);

    if(anonSendPool.sessionDenom == 0){
        ui->labelSubmittedDenom->setText(tr("N/A"));
    } else {
        std::string out;
        anonSendPool.GetDenominationsToString(anonSendPool.sessionDenom, out);
        QString s2(out.c_str());
        ui->labelSubmittedDenom->setText(s2);
    }

    showingAnonSendMessage++;
    anonsendActionCheck++;

    // Get AnonSend Denomination Status
}

void OverviewPage::anonsendAuto(){
    anonSendPool.DoAutomaticDenominating();
}

void OverviewPage::anonsendReset(){
    anonSendPool.Reset();

    QMessageBox::warning(this, tr("Anonsend"),
        tr("Anonsend was successfully reset."),
        QMessageBox::Ok, QMessageBox::Ok);
}

void OverviewPage::toggleAnonsend(){
    if(!fEnableAnonsend){
        int64_t balance = pwalletMain->GetBalance();
        float minAmount = 1.49 * COIN;
        if(balance < minAmount){
            QString strMinAmount(
                BitcoinUnits::formatWithUnit(
                    walletModel->getOptionsModel()->getDisplayUnit(),
                    minAmount));
            QMessageBox::warning(this, tr("Anonsend"),
                tr("Anonsend requires at least %1 to use.").arg(strMinAmount),
                QMessageBox::Ok, QMessageBox::Ok);
            return;
        }

        // if wallet is locked, ask for a passphrase
        if (walletModel->getEncryptionStatus() == WalletModel::Locked)
        {
            WalletModel::UnlockContext ctx(walletModel->requestUnlock());
            if(!ctx.isValid())
            {
                //unlock was cancelled
                anonSendPool.cachedNumBlocks = 0;
                QMessageBox::warning(this, tr("Anonsend"),
                    tr("Wallet is locked and user declined to unlock. Disabling Anonsend."),
                    QMessageBox::Ok, QMessageBox::Ok);
                if (fDebug) LogPrintf("Wallet is locked and user declined to unlock. Disabling Anonsend.\n");
                return;
            }
        }

    }

    anonSendPool.cachedNumBlocks = 0;
    fEnableAnonsend = !fEnableAnonsend;

    if(!fEnableAnonsend){
        ui->toggleAnonsend->setText(tr("Start Anonsend"));
    } else {
        ui->toggleAnonsend->setText(tr("Stop Anonsend"));

        /* show anonsend configuration if client has defaults set */

        if(nAnonymizeNavCoinAmount == 0){
            AnonsendConfig dlg(this);
            dlg.setModel(walletModel);
            dlg.exec();
        }

        anonSendPool.DoAutomaticDenominating();
    }
}
