#include "inode.h"
#include "activeinode.h"
#include "anonsend.h"
//#include "primitives/transaction.h"
#include "main.h"
#include "util.h"
#include "addrman.h"
#include <boost/lexical_cast.hpp>


int CINode::minProtoVersion = MIN_MN_PROTO_VERSION;

CCriticalSection cs_inodes;

/** The list of active inodes */
std::vector<CINode> vecInodes;
/** Object for who's going to get paid on which blocks */
CInodePayments inodePayments;
// keep track of inode votes I've seen
map<uint256, CInodePaymentWinner> mapSeenInodeVotes;
// keep track of the scanning errors I've seen
map<uint256, int> mapSeenInodeScanningErrors;
// who's asked for the inode list and the last time
std::map<CNetAddr, int64_t> askedForInodeList;
// which inodes we've asked for
std::map<COutPoint, int64_t> askedForInodeListEntry;
// cache block hashes as we calculate them
std::map<int64_t, uint256> mapCacheBlockHashes;

// manage the inode connections
void ProcessInodeConnections(){
    LOCK(cs_vNodes);

    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        //if it's our inode, let it be
        if(anonSendPool.submittedToInode == pnode->addr) continue;

        if(pnode->fAnonSendMaster){
            LogPrintf("Closing inode connection %s \n", pnode->addr.ToString().c_str());
            pnode->CloseSocketDisconnect();
        }
    }
}

void ProcessMessageInode(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{

    if (strCommand == "dsee") { //AnonSend Election Entry
        if(fLiteMode) return; //disable all anonsend/inode related functionality

        bool fIsInitialDownload = IsInitialBlockDownload();
        if(fIsInitialDownload) return;

        CTxIn vin;
        CService addr;
        CPubKey pubkey;
        CPubKey pubkey2;
        vector<unsigned char> vchSig;
        int64_t sigTime;
        int count;
        int current;
        int64_t lastUpdated;
        int protocolVersion;
        std::string strMessage;

        // 70047 and greater
        vRecv >> vin >> addr >> vchSig >> sigTime >> pubkey >> pubkey2 >> count >> current >> lastUpdated >> protocolVersion;

        // make sure signature isn't in the future (past is OK)
        if (sigTime > GetAdjustedTime() + 60 * 60) {
            LogPrintf("dsee - Signature rejected, too far into the future %s\n", vin.ToString().c_str());
            return;
        }

        bool isLocal = addr.IsRFC1918() || addr.IsLocal();
        //if(Params().MineBlocksOnDemand()) isLocal = false;

        std::string vchPubKey(pubkey.begin(), pubkey.end());
        std::string vchPubKey2(pubkey2.begin(), pubkey2.end());

        strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) + vchPubKey + vchPubKey2 + boost::lexical_cast<std::string>(protocolVersion);

        if(protocolVersion < MIN_MN_PROTO_VERSION) {
            LogPrintf("dsee - ignoring outdated inode %s protocol version %d\n", vin.ToString().c_str(), protocolVersion);
            return;
        }

        CScript pubkeyScript;
        pubkeyScript =GetScriptForDestination(pubkey.GetID());

        if(pubkeyScript.size() != 25) {
            LogPrintf("dsee - pubkey the wrong size\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        CScript pubkeyScript2;
        pubkeyScript2 =GetScriptForDestination(pubkey2.GetID());

        if(pubkeyScript2.size() != 25) {
            LogPrintf("dsee - pubkey2 the wrong size\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        std::string errorMessage = "";
        if(!anonSendSigner.VerifyMessage(pubkey, vchSig, strMessage, errorMessage)){
            LogPrintf("dsee - Got bad inode address signature\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        

        //search existing inode list, this is where we update existing inodes with new dsee broadcasts
	LOCK(cs_inodes);
        BOOST_FOREACH(CINode& mn, vecInodes) {
            if(mn.vin.prevout == vin.prevout) {
                // count == -1 when it's a new entry
                //   e.g. We don't want the entry relayed/time updated when we're syncing the list
                // mn.pubkey = pubkey, IsVinAssociatedWithPubkey is validated once below,
                //   after that they just need to match
                if(count == -1 && mn.pubkey == pubkey && !mn.UpdatedWithin(INODE_MIN_DSEE_SECONDS)){
                    mn.UpdateLastSeen();

                    if(mn.now < sigTime){ //take the newest entry
                        LogPrintf("dsee - Got updated entry for %s\n", addr.ToString().c_str());
                        mn.pubkey2 = pubkey2;
                        mn.now = sigTime;
                        mn.sig = vchSig;
                        mn.protocolVersion = protocolVersion;
                        mn.addr = addr;

                        RelayAnonSendElectionEntry(vin, addr, vchSig, sigTime, pubkey, pubkey2, count, current, lastUpdated, protocolVersion);
                    }
                }

                return;
            }
        }

        // make sure the vout that was signed is related to the transaction that spawned the inode
        //  - this is expensive, so it's only done once per inode
        if(!anonSendSigner.IsVinAssociatedWithPubkey(vin, pubkey)) {
            LogPrintf("dsee - Got mismatched pubkey and vin\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        if(fDebug) LogPrintf("dsee - Got NEW inode entry %s\n", addr.ToString().c_str());

        // make sure it's still unspent
        //  - this is checked later by .check() in many places and by ThreadCheckAnonSendPool()

        CValidationState state;
        CTransaction tx = CTransaction();
        CTxOut vout = CTxOut(99999*COIN, anonSendPool.collateralPubKey);
        tx.vin.push_back(vin);
        tx.vout.push_back(vout);
        //if(AcceptableInputs(mempool, state, tx)){
	bool* pfMissingInputs = false;
	if(AcceptableInputs(mempool, tx, false, pfMissingInputs)){
            if(fDebug) LogPrintf("dsee - Accepted inode entry %i %i\n", count, current);

            if(GetInputAge(vin) < INODE_MIN_CONFIRMATIONS){
                LogPrintf("dsee - Input must have least %d confirmations\n", INODE_MIN_CONFIRMATIONS);
                Misbehaving(pfrom->GetId(), 20);
                return;
            }

            // use this as a peer
            addrman.Add(CAddress(addr), pfrom->addr, 2*60*60);

            // add our inode
            CINode mn(addr, vin, pubkey, vchSig, sigTime, pubkey2, protocolVersion);
            mn.UpdateLastSeen(lastUpdated);
            vecInodes.push_back(mn);

            // if it matches our inodeprivkey, then we've been remotely activated
            if(pubkey2 == activeInode.pubKeyInode && protocolVersion == PROTOCOL_VERSION){
                activeInode.EnableHotColdINode(vin, addr);
            }

            if(count == -1 && !isLocal)
                RelayAnonSendElectionEntry(vin, addr, vchSig, sigTime, pubkey, pubkey2, count, current, lastUpdated, protocolVersion);

        } else {
            LogPrintf("dsee - Rejected inode entry %s\n", addr.ToString().c_str());

            int nDoS = 0;
            if (state.IsInvalid(nDoS))
            {
                LogPrintf("dsee - %s from %s %s was not accepted into the memory pool\n", tx.GetHash().ToString().c_str(),
                    pfrom->addr.ToString().c_str(), pfrom->cleanSubVer.c_str());
                if (nDoS > 0)
                    Misbehaving(pfrom->GetId(), nDoS);
            }
        }
    }

    else if (strCommand == "dseep") { //AnonSend Election Entry Ping
        if(fLiteMode) return; //disable all anonsend/inode related functionality
        bool fIsInitialDownload = IsInitialBlockDownload();
        if(fIsInitialDownload) return;

        CTxIn vin;
        vector<unsigned char> vchSig;
        int64_t sigTime;
        bool stop;
        vRecv >> vin >> vchSig >> sigTime >> stop;

        //LogPrintf("dseep - Received: vin: %s sigTime: %lld stop: %s\n", vin.ToString().c_str(), sigTime, stop ? "true" : "false");

        if (sigTime > GetAdjustedTime() + 60 * 60) {
            LogPrintf("dseep - Signature rejected, too far into the future %s\n", vin.ToString().c_str());
            return;
        }

        if (sigTime <= GetAdjustedTime() - 60 * 60) {
            LogPrintf("dseep - Signature rejected, too far into the past %s - %d %d \n", vin.ToString().c_str(), sigTime, GetAdjustedTime());
            return;
        }

        // see if we have this inode
	LOCK(cs_inodes);
        BOOST_FOREACH(CINode& mn, vecInodes) {
            if(mn.vin.prevout == vin.prevout) {
            	// LogPrintf("dseep - Found corresponding mn for vin: %s\n", vin.ToString().c_str());
            	// take this only if it's newer
                if(mn.lastDseep < sigTime){
                    std::string strMessage = mn.addr.ToString() + boost::lexical_cast<std::string>(sigTime) + boost::lexical_cast<std::string>(stop);

                    std::string errorMessage = "";
                    if(!anonSendSigner.VerifyMessage(mn.pubkey2, vchSig, strMessage, errorMessage)){
                        LogPrintf("dseep - Got bad inode address signature %s \n", vin.ToString().c_str());
                        //Misbehaving(pfrom->GetId(), 100);
                        return;
                    }

                    mn.lastDseep = sigTime;

                    if(!mn.UpdatedWithin(INODE_MIN_DSEEP_SECONDS)){
                        mn.UpdateLastSeen();
                        if(stop) {
                            mn.Disable();
                            mn.Check();
                        }
                        RelayAnonSendElectionEntryPing(vin, vchSig, sigTime, stop);
                    }
                }
                return;
            }
        }

        if(fDebug) LogPrintf("dseep - Couldn't find inode entry %s\n", vin.ToString().c_str());

        std::map<COutPoint, int64_t>::iterator i = askedForInodeListEntry.find(vin.prevout);
        if (i != askedForInodeListEntry.end()){
            int64_t t = (*i).second;
            if (GetTime() < t) {
                // we've asked recently
                return;
            }
        }

        // ask for the dsee info once from the node that sent dseep

        LogPrintf("dseep - Asking source node for missing entry %s\n", vin.ToString().c_str());
        pfrom->PushMessage("dseg", vin);
        int64_t askAgain = GetTime()+(60*60*24);
        askedForInodeListEntry[vin.prevout] = askAgain;

    } else if (strCommand == "dseg") { //Get inode list or specific entry
        if(fLiteMode) return; //disable all anonsend/inode related functionality
        CTxIn vin;
        vRecv >> vin;

        if(vin == CTxIn()) { //only should ask for this once
            //local network
            //Note tor peers show up as local proxied addrs //if(!pfrom->addr.IsRFC1918())//&& !Params().MineBlocksOnDemand())
            //{
                std::map<CNetAddr, int64_t>::iterator i = askedForInodeList.find(pfrom->addr);
                if (i != askedForInodeList.end())
                {
                    int64_t t = (*i).second;
                    if (GetTime() < t) {
                        //Misbehaving(pfrom->GetId(), 34);
                        //LogPrintf("dseg - peer already asked me for the list\n");
                        //return;
                    }
                }

                int64_t askAgain = GetTime()+(60*60*3);
                askedForInodeList[pfrom->addr] = askAgain;
            //}
        } //else, asking for a specific node which is ok

	LOCK(cs_inodes);
        int count = vecInodes.size();
        int i = 0;

        BOOST_FOREACH(CINode mn, vecInodes) {

            if(mn.addr.IsRFC1918()) continue; //local network

            if(vin == CTxIn()){
                mn.Check();
                if(mn.IsEnabled()) {
                    if(fDebug) LogPrintf("dseg - Sending inode entry - %s \n", mn.addr.ToString().c_str());
                    pfrom->PushMessage("dsee", mn.vin, mn.addr, mn.sig, mn.now, mn.pubkey, mn.pubkey2, count, i, mn.lastTimeSeen, mn.protocolVersion);
                }
            } else if (vin == mn.vin) {
                if(fDebug) LogPrintf("dseg - Sending inode entry - %s \n", mn.addr.ToString().c_str());
                pfrom->PushMessage("dsee", mn.vin, mn.addr, mn.sig, mn.now, mn.pubkey, mn.pubkey2, count, i, mn.lastTimeSeen, mn.protocolVersion);
                LogPrintf("dseg - Sent 1 inode entries to %s\n", pfrom->addr.ToString().c_str());
                return;
            }
            i++;
        }

        LogPrintf("dseg - Sent %d inode entries to %s\n", count, pfrom->addr.ToString().c_str());
    }

    else if (strCommand == "mnget") { //Inode Payments Request Sync
        if(fLiteMode) return; //disable all anonsend/inode related functionality

        /*if(pfrom->HasFulfilledRequest("mnget")) {
            LogPrintf("mnget - peer already asked me for the list\n");
            Misbehaving(pfrom->GetId(), 20);
            return;
        }*/

        pfrom->FulfilledRequest("mnget");
        inodePayments.Sync(pfrom);
        LogPrintf("mnget - Sent inode winners to %s\n", pfrom->addr.ToString().c_str());
    }
    else if (strCommand == "mnw") { //Inode Payments Declare Winner
        //this is required in litemode
        CInodePaymentWinner winner;
        int a = 0;
        vRecv >> winner >> a;

        if(pindexBest == NULL) return;

        uint256 hash = winner.GetHash();
        if(mapSeenInodeVotes.count(hash)) {
            if(fDebug) LogPrintf("mnw - seen vote %s Height %d bestHeight %d\n", hash.ToString().c_str(), winner.nBlockHeight, pindexBest->nHeight);
            return;
        }

        if(winner.nBlockHeight < pindexBest->nHeight - 10 || winner.nBlockHeight > pindexBest->nHeight+20){
            LogPrintf("mnw - winner out of range %s Height %d bestHeight %d\n", winner.vin.ToString().c_str(), winner.nBlockHeight, pindexBest->nHeight);
            return;
        }

        if(winner.vin.nSequence != std::numeric_limits<unsigned int>::max()){
            LogPrintf("mnw - invalid nSequence\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        LogPrintf("mnw - winning vote  %s Height %d bestHeight %d\n", winner.vin.ToString().c_str(), winner.nBlockHeight, pindexBest->nHeight);

        if(!inodePayments.CheckSignature(winner)){
            LogPrintf("mnw - invalid signature\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        mapSeenInodeVotes.insert(make_pair(hash, winner));

        if(inodePayments.AddWinningInode(winner)){
            inodePayments.Relay(winner);
        }
    }
}

struct CompareValueOnly
{
    bool operator()(const pair<int64_t, CTxIn>& t1,
                    const pair<int64_t, CTxIn>& t2) const
    {
        return t1.first < t2.first;
    }
};

struct CompareValueOnly2
{
    bool operator()(const pair<int64_t, int>& t1,
                    const pair<int64_t, int>& t2) const
    {
        return t1.first < t2.first;
    }
};

int CountInodesAboveProtocol(int protocolVersion)
{
    int i = 0;
    LOCK(cs_inodes);
    BOOST_FOREACH(CINode& mn, vecInodes) {
        if(mn.protocolVersion < protocolVersion) continue;
        i++;
    }

    return i;

}


int GetInodeByVin(CTxIn& vin)
{
    int i = 0;
    LOCK(cs_inodes);
    BOOST_FOREACH(CINode& mn, vecInodes) {
        if (mn.vin == vin) return i;
        i++;
    }

    return -1;
}

int GetCurrentINode(int mod, int64_t nBlockHeight, int minProtocol)
{
    int i = 0;
    unsigned int score = 0;
    int winner = -1;
    LOCK(cs_inodes);
    // scan for winner
    BOOST_FOREACH(CINode mn, vecInodes) {
        mn.Check();
        if(mn.protocolVersion < minProtocol) continue;
        if(!mn.IsEnabled()) {
            i++;
            continue;
        }

        // calculate the score for each inode
        uint256 n = mn.CalculateScore(mod, nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        // determine the winner
        if(n2 > score){
            score = n2;
            winner = i;
        }
        i++;
    }

    return winner;
}

int GetInodeByRank(int findRank, int64_t nBlockHeight, int minProtocol)
{
    LOCK(cs_inodes);
    int i = 0;

    std::vector<pair<unsigned int, int> > vecInodeScores;

    i = 0;
    BOOST_FOREACH(CINode mn, vecInodes) {
        mn.Check();
        if(mn.protocolVersion < minProtocol) continue;
        if(!mn.IsEnabled()) {
            i++;
            continue;
        }

        uint256 n = mn.CalculateScore(1, nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        vecInodeScores.push_back(make_pair(n2, i));
        i++;
    }

    sort(vecInodeScores.rbegin(), vecInodeScores.rend(), CompareValueOnly2());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(unsigned int, int)& s, vecInodeScores){
        rank++;
        if(rank == findRank) return s.second;
    }

    return -1;
}

int GetInodeRank(CTxIn& vin, int64_t nBlockHeight, int minProtocol)
{
    LOCK(cs_inodes);
    std::vector<pair<unsigned int, CTxIn> > vecInodeScores;

    BOOST_FOREACH(CINode& mn, vecInodes) {
        mn.Check();

        if(mn.protocolVersion < minProtocol) continue;
        if(!mn.IsEnabled()) {
            continue;
        }

        uint256 n = mn.CalculateScore(1, nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        vecInodeScores.push_back(make_pair(n2, mn.vin));
    }

    sort(vecInodeScores.rbegin(), vecInodeScores.rend(), CompareValueOnly());

    unsigned int rank = 0;
    BOOST_FOREACH (PAIRTYPE(unsigned int, CTxIn)& s, vecInodeScores){
        rank++;
        if(s.second == vin) {
            return rank;
        }
    }

    return -1;
}

//Get the last hash that matches the modulus given. Processed in reverse order
bool GetBlockHash(uint256& hash, int nBlockHeight)
{
    if (pindexBest == NULL) return false;

    if(nBlockHeight == 0)
        nBlockHeight = pindexBest->nHeight;

    if(mapCacheBlockHashes.count(nBlockHeight)){
        hash = mapCacheBlockHashes[nBlockHeight];
        return true;
    }

    const CBlockIndex *BlockLastSolved = pindexBest;
    const CBlockIndex *BlockReading = pindexBest;

    if (BlockLastSolved == NULL || BlockLastSolved->nHeight == 0 || pindexBest->nHeight+1 < nBlockHeight) return false;

    int nBlocksAgo = 0;
    if(nBlockHeight > 0) nBlocksAgo = (pindexBest->nHeight+1)-nBlockHeight;
    assert(nBlocksAgo >= 0);

    int n = 0;
    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if(n >= nBlocksAgo){
            hash = BlockReading->GetBlockHash();
            mapCacheBlockHashes[nBlockHeight] = hash;
            return true;
        }
        n++;

        if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
        BlockReading = BlockReading->pprev;
    }

    return false;
}

//
// Deterministically calculate a given "score" for a inode depending on how close it's hash is to
// the proof of work for that block. The further away they are the better, the furthest will win the election
// and get paid this block
//
uint256 CINode::CalculateScore(int mod, int64_t nBlockHeight)
{
    if(pindexBest == NULL) return 0;

    uint256 hash = 0;
    uint256 aux = vin.prevout.hash + vin.prevout.n;

    if(!GetBlockHash(hash, nBlockHeight)) return 0;

    uint256 hash2 = Hash(BEGIN(hash), END(hash));
    uint256 hash3 = Hash(BEGIN(hash), END(aux));

    uint256 r = (hash3 > hash2 ? hash3 - hash2 : hash2 - hash3);

    return r;
}

void CINode::Check()
{
    //once spent, stop doing the checks
    if(enabled==3) return;


    if(!UpdatedWithin(INODE_REMOVAL_SECONDS)){
        enabled = 4;
        return;
    }

    if(!UpdatedWithin(INODE_EXPIRATION_SECONDS)){
        enabled = 2;
        return;
    }

    if(!unitTest){
        CValidationState state;
        CTransaction tx = CTransaction();
        CTxOut vout = CTxOut(99999*COIN, anonSendPool.collateralPubKey);
        tx.vin.push_back(vin);
        tx.vout.push_back(vout);

        //if(!AcceptableInputs(mempool, state, tx)){
        bool* pfMissingInputs = false;
	if(!AcceptableInputs(mempool, tx, false, pfMissingInputs)){
            enabled = 3;
            return;
        }
    }

    enabled = 1; // OK
}

bool CInodePayments::CheckSignature(CInodePaymentWinner& winner)
{
    //note: need to investigate why this is failing
    std::string strMessage = winner.vin.ToString().c_str() + boost::lexical_cast<std::string>(winner.nBlockHeight) + winner.payee.ToString();
    std::string strPubKey = strMainPubKey ;
    CPubKey pubkey(ParseHex(strPubKey));

    std::string errorMessage = "";
    if(!anonSendSigner.VerifyMessage(pubkey, winner.vchSig, strMessage, errorMessage)){
        return false;
    }

    return true;
}

bool CInodePayments::Sign(CInodePaymentWinner& winner)
{
    std::string strMessage = winner.vin.ToString().c_str() + boost::lexical_cast<std::string>(winner.nBlockHeight) + winner.payee.ToString();

    CKey key2;
    CPubKey pubkey2;
    std::string errorMessage = "";

    if(!anonSendSigner.SetKey(strMasterPrivKey, errorMessage, key2, pubkey2))
    {
        LogPrintf("CNodePayments::Sign - ERROR: Invalid inodeprivkey: '%s'\n", errorMessage.c_str());
        return false;
    }

    if(!anonSendSigner.SignMessage(strMessage, errorMessage, winner.vchSig, key2)) {
        LogPrintf("CInodePayments::Sign - Sign message failed");
        return false;
    }

    if(!anonSendSigner.VerifyMessage(pubkey2, winner.vchSig, strMessage, errorMessage)) {
        LogPrintf("CInodePayments::Sign - Verify message failed");
        return false;
    }

    return true;
}

uint64_t CInodePayments::CalculateScore(uint256 blockHash, CTxIn& vin)
{
    uint256 n1 = blockHash;
    uint256 n2 = Hash(BEGIN(n1), END(n1));
    uint256 n3 = Hash(BEGIN(vin.prevout.hash), END(vin.prevout.hash));
    uint256 n4 = n3 > n2 ? (n3 - n2) : (n2 - n3);

    //printf(" -- CInodePayments CalculateScore() n2 = %d \n", n2.Get64());
    //printf(" -- CInodePayments CalculateScore() n3 = %d \n", n3.Get64());
    //printf(" -- CInodePayments CalculateScore() n4 = %d \n", n4.Get64());

    return n4.Get64();
}

bool CInodePayments::GetBlockPayee(int nBlockHeight, CScript& payee)
{
    BOOST_FOREACH(CInodePaymentWinner& winner, vWinning){
        if(winner.nBlockHeight == nBlockHeight) {
            payee = winner.payee;
            return true;
        }
    }

    return false;
}

bool CInodePayments::GetWinningInode(int nBlockHeight, CTxIn& vinOut)
{
    BOOST_FOREACH(CInodePaymentWinner& winner, vWinning){
        if(winner.nBlockHeight == nBlockHeight) {
            vinOut = winner.vin;
            return true;
        }
    }

    return false;
}

bool CInodePayments::AddWinningInode(CInodePaymentWinner& winnerIn)
{
    uint256 blockHash = 0;
    if(!GetBlockHash(blockHash, winnerIn.nBlockHeight-576)) {
        return false;
    }

    winnerIn.score = CalculateScore(blockHash, winnerIn.vin);

    bool foundBlock = false;
    BOOST_FOREACH(CInodePaymentWinner& winner, vWinning){
        if(winner.nBlockHeight == winnerIn.nBlockHeight) {
            foundBlock = true;
            if(winner.score < winnerIn.score){
                winner.score = winnerIn.score;
                winner.vin = winnerIn.vin;
                winner.payee = winnerIn.payee;
                winner.vchSig = winnerIn.vchSig;

                return true;
            }
        }
    }

    // if it's not in the vector
    if(!foundBlock){
        vWinning.push_back(winnerIn);
        mapSeenInodeVotes.insert(make_pair(winnerIn.GetHash(), winnerIn));

        return true;
    }

    return false;
}

void CInodePayments::CleanPaymentList()
{
    LOCK(cs_inodes);
    if(pindexBest == NULL) return;

    int nLimit = std::max(((int)vecInodes.size())*2, 1000);

    vector<CInodePaymentWinner>::iterator it;
    for(it=vWinning.begin();it<vWinning.end();it++){
        if(pindexBest->nHeight - (*it).nBlockHeight > nLimit){
            if(fDebug) LogPrintf("CInodePayments::CleanPaymentList - Removing old inode payment - block %d\n", (*it).nBlockHeight);
            vWinning.erase(it);
            break;
        }
    }
}

bool CInodePayments::ProcessBlock(int nBlockHeight)
{
    LOCK(cs_inodes);
    if(!enabled) return false;
    CInodePaymentWinner winner;

    std::vector<CTxIn> vecLastPayments;
    int c = 0;
    BOOST_REVERSE_FOREACH(CInodePaymentWinner& winner, vWinning){
        vecLastPayments.push_back(winner.vin);
        //if we have one full payment cycle, break
        if(++c > (int)vecInodes.size()) break;
    }

    std::random_shuffle ( vecInodes.begin(), vecInodes.end() );
    BOOST_FOREACH(CINode& mn, vecInodes) {
        bool found = false;
        BOOST_FOREACH(CTxIn& vin, vecLastPayments)
            if(mn.vin == vin) found = true;

        if(found) continue;

        mn.Check();
        if(!mn.IsEnabled()) {
            continue;
        }

        winner.score = 0;
        winner.nBlockHeight = nBlockHeight;
        winner.vin = mn.vin;
        winner.payee =GetScriptForDestination(mn.pubkey.GetID());

        break;
    }

    //if we can't find someone to get paid, pick randomly
    if(winner.nBlockHeight == 0 && vecInodes.size() > 0) {
        winner.score = 0;
        winner.nBlockHeight = nBlockHeight;
        winner.vin = vecInodes[0].vin;
        winner.payee =GetScriptForDestination(vecInodes[0].pubkey.GetID());
    }

    if(Sign(winner)){
        if(AddWinningInode(winner)){
            Relay(winner);
            return true;
        }
    }

    return false;
}

void CInodePayments::Relay(CInodePaymentWinner& winner)
{
    CInv inv(MSG_INODE_WINNER, winner.GetHash());

    vector<CInv> vInv;
    vInv.push_back(inv);
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes){
        pnode->PushMessage("inv", vInv);
    }
}

void CInodePayments::Sync(CNode* node)
{
    int a = 0;
    BOOST_FOREACH(CInodePaymentWinner& winner, vWinning)
        if(winner.nBlockHeight >= pindexBest->nHeight-10 && winner.nBlockHeight <= pindexBest->nHeight + 20)
            node->PushMessage("mnw", winner, a);
}


bool CInodePayments::SetPrivKey(std::string strPrivKey)
{
    CInodePaymentWinner winner;

    // Test signing successful, proceed
    strMasterPrivKey = strPrivKey;

    Sign(winner);

    if(CheckSignature(winner)){
        LogPrintf("CInodePayments::SetPrivKey - Successfully initialized as inode payments master\n");
        enabled = true;
        return true;
    } else {
        return false;
    }
}
