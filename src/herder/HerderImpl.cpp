// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderImpl.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "herder/HerderPersistence.h"
#include "herder/HerderUtils.h"
#include "herder/LedgerCloseData.h"
#include "herder/QuorumIntersectionChecker.h"
#include "herder/TxSetFrame.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "lib/json/json.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/ErrorMessages.h"
#include "main/PersistentState.h"
#include "overlay/OverlayManager.h"
#include "scp/LocalNode.h"
#include "scp/Slot.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/StatusManager.h"
#include "util/Timer.h"

#include "medida/counter.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "util/Decoder.h"
#include "util/XDRStream.h"
#include "xdrpp/marshal.h"
#include <Tracy.hpp>

#include <ctime>
#include <fmt/format.h>

using namespace std;

namespace stellar
{

constexpr auto const TRANSACTION_QUEUE_SIZE = 4;
constexpr auto const TRANSACTION_QUEUE_BAN_SIZE = 10;
constexpr auto const TRANSACTION_QUEUE_MULTIPLIER = 4;
constexpr size_t const OPERATION_BROADCAST_MULTIPLIER = 2;

std::unique_ptr<Herder>
Herder::create(Application& app)
{
    return std::make_unique<HerderImpl>(app);
}

HerderImpl::SCPMetrics::SCPMetrics(Application& app)
    : mLostSync(app.getMetrics().NewMeter({"scp", "sync", "lost"}, "sync"))
    , mEnvelopeEmit(
          app.getMetrics().NewMeter({"scp", "envelope", "emit"}, "envelope"))
    , mEnvelopeReceive(
          app.getMetrics().NewMeter({"scp", "envelope", "receive"}, "envelope"))
    , mCumulativeStatements(app.getMetrics().NewCounter(
          {"scp", "memory", "cumulative-statements"}))
    , mEnvelopeValidSig(app.getMetrics().NewMeter(
          {"scp", "envelope", "validsig"}, "envelope"))
    , mEnvelopeInvalidSig(app.getMetrics().NewMeter(
          {"scp", "envelope", "invalidsig"}, "envelope"))
{
}

HerderImpl::HerderImpl(Application& app)
    : mTransactionQueue(app, TRANSACTION_QUEUE_SIZE, TRANSACTION_QUEUE_BAN_SIZE,
                        TRANSACTION_QUEUE_MULTIPLIER)
    , mPendingEnvelopes(app, *this)
    , mHerderSCPDriver(app, *this, mUpgrades, mPendingEnvelopes)
    , mLastSlotSaved(0)
    , mTrackingTimer(app)
    , mLastExternalize(app.getClock().now())
    , mTriggerTimer(app)
    , mRebroadcastTimer(app)
    , mApp(app)
    , mLedgerManager(app.getLedgerManager())
    , mSCPMetrics(app)
{
    auto ln = getSCP().getLocalNode();
    mPendingEnvelopes.addSCPQuorumSet(ln->getQuorumSetHash(),
                                      ln->getQuorumSet());
}

HerderImpl::~HerderImpl()
{
}

Herder::State
HerderImpl::getState() const
{
    return mHerderSCPDriver.getState();
}

SCP&
HerderImpl::getSCP()
{
    return mHerderSCPDriver.getSCP();
}

void
HerderImpl::syncMetrics()
{
    int64_t count = getSCP().getCumulativeStatemtCount();
    mSCPMetrics.mCumulativeStatements.set_count(count);
    TracyPlot("scp.memory.cumulative-statements", count);
}

std::string
HerderImpl::getStateHuman() const
{
    static const char* stateStrings[HERDER_NUM_STATE] = {
        "HERDER_SYNCING_STATE", "HERDER_TRACKING_STATE"};
    return std::string(stateStrings[getState()]);
}

void
HerderImpl::bootstrap()
{
    CLOG(INFO, "Herder") << "Force joining SCP with local state";
    assert(getSCP().isValidator());
    assert(mApp.getConfig().FORCE_SCP);

    mLedgerManager.moveToSynced();
    mHerderSCPDriver.bootstrap();

    ledgerClosed(true);
}

void
HerderImpl::shutdown()
{
    mTrackingTimer.cancel();
    mRebroadcastTimer.cancel();
    mTriggerTimer.cancel();
    if (mLastQuorumMapIntersectionState.mRecalculating)
    {
        // We want to interrupt any calculation-in-progress at shutdown to
        // avoid a long pause joining worker threads.
        CLOG(DEBUG, "Herder")
            << "Shutdown interrupting quorum transitive closure analysis.";
        mLastQuorumMapIntersectionState.mInterruptFlag = true;
    }
}

void
HerderImpl::processExternalized(uint64 slotIndex, StellarValue const& value)
{
    ZoneScoped;
    bool validated = getSCP().isSlotFullyValidated(slotIndex);

    if (Logging::logDebug("Herder"))
    {
        CLOG(DEBUG, "Herder") << fmt::format(
            "HerderSCPDriver::valueExternalized index: {} txSet: {}", slotIndex,
            hexAbbrev(value.txSetHash));
    }

    if (getSCP().isValidator() && !validated)
    {
        CLOG(WARNING, "Herder")
            << fmt::format("Ledger {} ({}) closed and could NOT be fully "
                           "validated by validator",
                           slotIndex, hexAbbrev(value.txSetHash));
    }

    TxSetFramePtr externalizedSet = mPendingEnvelopes.getTxSet(value.txSetHash);

    // save the SCP messages in the database
    if (mApp.getConfig().MODE_STORES_HISTORY)
    {
        mApp.getHerderPersistence().saveSCPHistory(
            static_cast<uint32>(slotIndex),
            getSCP().getExternalizingState(slotIndex),
            mPendingEnvelopes.getCurrentlyTrackedQuorum());
    }

    // reflect upgrades with the ones included in this SCP round
    {
        bool updated;
        auto newUpgrades = mUpgrades.removeUpgrades(value.upgrades.begin(),
                                                    value.upgrades.end(),
                                                    value.closeTime, updated);
        if (updated)
        {
            setUpgrades(newUpgrades);
        }
    }

    // tell the LedgerManager that this value got externalized
    // LedgerManager will perform the proper action based on its internal
    // state: apply, trigger catchup, etc
    LedgerCloseData ledgerData(static_cast<uint32_t>(slotIndex),
                               externalizedSet, value);
    mLedgerManager.valueExternalized(ledgerData);
}

void
HerderImpl::valueExternalized(uint64 slotIndex, StellarValue const& value)
{
    ZoneScoped;
    const int DUMP_SCP_TIMEOUT_SECONDS = 20;
    // SCPDriver always updates tracking for latest slots
    bool isLatestSlot = slotIndex == getCurrentLedgerSeq();

    if (isLatestSlot)
    {
        // called both here and at the end (this one is in case of an exception)
        trackingHeartBeat();

        // dump SCP information if this ledger took a long time
        auto gap = std::chrono::duration<double>(mApp.getClock().now() -
                                                 mLastExternalize)
                       .count();
        if (gap > DUMP_SCP_TIMEOUT_SECONDS)
        {
            auto slotInfo = getJsonQuorumInfo(getSCP().getLocalNodeID(), false,
                                              false, slotIndex);
            Json::FastWriter fw;
            CLOG(WARNING, "Herder")
                << fmt::format("Ledger took {} seconds, SCP information:{}",
                               gap, fw.write(slotInfo));
        }

        // trigger will be recreated when the ledger is closed
        // we do not want it to trigger while downloading the current set
        // and there is no point in taking a position after the round is over
        mTriggerTimer.cancel();

        processExternalized(slotIndex, value);

        // start timing next externalize from this point
        mLastExternalize = mApp.getClock().now();

        // perform cleanups
        TxSetFramePtr externalizedSet =
            mPendingEnvelopes.getTxSet(value.txSetHash);
        updateTransactionQueue(externalizedSet->mTransactions);

        // Evict slots that are outside of our ledger validity bracket
        auto minSlotToRemember = getMinLedgerSeqToRemember();
        if (minSlotToRemember > LedgerManager::GENESIS_LEDGER_SEQ)
        {
            getHerderSCPDriver().purgeSlots(minSlotToRemember);
            mPendingEnvelopes.eraseBelow(minSlotToRemember);
        }

        ledgerClosed(false);

        // Check to see if quorums have changed and we need to reanalyze.
        checkAndMaybeReanalyzeQuorumMap();

        // heart beat *after* doing all the work (ensures that we do not include
        // the overhead of externalization in the way we track SCP)
        trackingHeartBeat();
    }
    else
    {
        processExternalized(slotIndex, value);
    }
}

void
HerderImpl::rebroadcast()
{
    ZoneScoped;
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader().header;
    for (auto const& e : getSCP().getLatestMessagesSend(lcl.ledgerSeq + 1))
    {
        broadcast(e);
    }

    if (!mHerderSCPDriver.trackingSCP())
    {
        getMoreSCPState();
    }

    startRebroadcastTimer();
}

void
HerderImpl::broadcast(SCPEnvelope const& e)
{
    ZoneScoped;
    if (!mApp.getConfig().MANUAL_CLOSE)
    {
        StellarMessage m;
        m.type(SCP_MESSAGE);
        m.envelope() = e;

        CLOG(DEBUG, "Herder") << "broadcast "
                              << " s:" << e.statement.pledges.type()
                              << " i:" << e.statement.slotIndex;

        mSCPMetrics.mEnvelopeEmit.Mark();
        mApp.getOverlayManager().broadcastMessage(m, true);
    }
}

void
HerderImpl::startRebroadcastTimer()
{
    mRebroadcastTimer.expires_from_now(std::chrono::seconds(2));

    mRebroadcastTimer.async_wait(std::bind(&HerderImpl::rebroadcast, this),
                                 &VirtualTimer::onFailureNoop);
}

void
HerderImpl::emitEnvelope(SCPEnvelope const& envelope)
{
    ZoneScoped;
    uint64 slotIndex = envelope.statement.slotIndex;

    if (Logging::logDebug("Herder"))
        CLOG(DEBUG, "Herder")
            << "emitEnvelope"
            << " s:" << envelope.statement.pledges.type() << " i:" << slotIndex
            << " a:" << mApp.getStateHuman();

    persistSCPState(slotIndex);

    broadcast(envelope);

    // this resets the re-broadcast timer
    startRebroadcastTimer();
}

TransactionQueue::AddResult
HerderImpl::recvTransaction(TransactionFrameBasePtr tx)
{
    ZoneScoped;
    auto result = mTransactionQueue.tryAdd(tx);
    if (result == TransactionQueue::AddResult::ADD_STATUS_PENDING)
    {
        if (Logging::logTrace("Herder"))
            CLOG(TRACE, "Herder")
                << "recv transaction " << hexAbbrev(tx->getFullHash())
                << " for " << KeyUtils::toShortString(tx->getSourceID());
    }
    return result;
}

bool
HerderImpl::checkCloseTime(SCPEnvelope const& envelope, bool enforceRecent)
{
    ZoneScoped;
    using std::placeholders::_1;
    auto const& st = envelope.statement;

    uint64_t ctCutoff = 0;

    if (enforceRecent)
    {
        ctCutoff = VirtualClock::to_time_t(mApp.getClock().system_now());
        if (ctCutoff >= mApp.getConfig().MAXIMUM_LEDGER_CLOSETIME_DRIFT)
        {
            ctCutoff -= mApp.getConfig().MAXIMUM_LEDGER_CLOSETIME_DRIFT;
        }
    }

    auto envLedgerIndex = envelope.statement.slotIndex;
    auto& scpD = getHerderSCPDriver();

    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader().header;
    auto lastCloseIndex = lcl.ledgerSeq;
    auto lastCloseTime = lcl.scpValue.closeTime;

    // see if we can get a better estimate of lastCloseTime for validating this
    // statement using consensus data:
    // update lastCloseIndex/lastCloseTime to be the highest possible but still
    // be less than envLedgerIndex
    auto adjustLastCloseTime = [&](HerderSCPDriver::ConsensusData const* cd) {
        if (cd && envLedgerIndex >= cd->mConsensusIndex &&
            cd->mConsensusIndex > lastCloseIndex)
        {
            lastCloseIndex = static_cast<uint32>(cd->mConsensusIndex);
            lastCloseTime = cd->mConsensusValue.closeTime;
        }
    };
    adjustLastCloseTime(mHerderSCPDriver.trackingSCP());
    adjustLastCloseTime(mHerderSCPDriver.lastTrackingSCP());

    StellarValue sv;
    // performs the most conservative check:
    // returns true if one of the values is valid
    auto checkCTHelper = [&](std::vector<Value> const& values) {
        return std::any_of(values.begin(), values.end(), [&](Value const& e) {
            auto r = scpD.toStellarValue(e, sv);
            // sv must be after cutoff
            r = r && sv.closeTime >= ctCutoff;
            if (r)
            {
                // statement received after the fact, only keep externalized
                // value
                r = (lastCloseIndex == envLedgerIndex &&
                     lastCloseTime == sv.closeTime);
                // for older messages, just ensure that they occurred before
                r = r || (lastCloseIndex > envLedgerIndex &&
                          lastCloseTime > sv.closeTime);
                // for future message, perform the same validity check than
                // within SCP
                r = r || scpD.checkCloseTime(envLedgerIndex, lastCloseTime, sv);
            }
            return r;
        });
    };

    bool b;

    switch (st.pledges.type())
    {
    case SCP_ST_NOMINATE:
        b = checkCTHelper(st.pledges.nominate().accepted) ||
            checkCTHelper(st.pledges.nominate().votes);
        break;
    case SCP_ST_PREPARE:
    {
        auto& prep = st.pledges.prepare();
        b = checkCTHelper({prep.ballot.value});
        if (!b && prep.prepared)
        {
            b = checkCTHelper({prep.prepared->value});
        }
        if (!b && prep.preparedPrime)
        {
            b = checkCTHelper({prep.preparedPrime->value});
        }
    }
    break;
    case SCP_ST_CONFIRM:
        b = checkCTHelper({st.pledges.confirm().ballot.value});
        break;
    case SCP_ST_EXTERNALIZE:
        b = checkCTHelper({st.pledges.externalize().commit.value});
        break;
    default:
        abort();
    }

    if (!b && Logging::logTrace("Herder"))
    {
        CLOG(TRACE, "Herder")
            << "Invalid close time processing " << getSCP().envToStr(st);
    }
    return b;
}

uint32_t
HerderImpl::getMinLedgerSeqToRemember()
{
    auto maxSlotsToRemember = mApp.getConfig().MAX_SLOTS_TO_REMEMBER;
    auto currSlot = getCurrentLedgerSeq();
    if (currSlot > maxSlotsToRemember)
    {
        return (currSlot - maxSlotsToRemember + 1);
    }
    else
    {
        return LedgerManager::GENESIS_LEDGER_SEQ;
    }
}

Herder::EnvelopeStatus
HerderImpl::recvSCPEnvelope(SCPEnvelope const& envelope)
{
    ZoneScoped;
    if (mApp.getConfig().MANUAL_CLOSE)
    {
        return Herder::ENVELOPE_STATUS_DISCARDED;
    }

    if (!verifyEnvelope(envelope))
    {
        std::string txt("DISCARDED - bad envelope");
        ZoneText(txt.c_str(), txt.size());
        CLOG(TRACE, "Herder") << "Received bad envelope, discarding";
        return Herder::ENVELOPE_STATUS_DISCARDED;
    }

    mSCPMetrics.mEnvelopeReceive.Mark();

    uint32_t minLedgerSeq = getMinLedgerSeqToRemember();
    uint32_t maxLedgerSeq = std::numeric_limits<uint32>::max();

    if (!checkCloseTime(envelope, false))
    {
        // if the envelope contains an invalid close time, don't bother
        // processing it as we're not going to forward it anyways and it's
        // going to just sit in our SCP state not contributing anything useful.
        CLOG(TRACE, "Herder")
            << "skipping invalid close time (incompatible with current state)";
        std::string txt("DISCARDED - incompatible close time");
        ZoneText(txt.c_str(), txt.size());
        return Herder::ENVELOPE_STATUS_DISCARDED;
    }

    if (mHerderSCPDriver.trackingSCP())
    {
        // when tracking, we can filter messages based on the information we got
        // from consensus for the max ledger

        // note that this filtering will cause a node on startup
        // to potentially drop messages outside of the bracket
        // causing it to discard CONSENSUS_STUCK_TIMEOUT_SECONDS worth of
        // ledger closing
        maxLedgerSeq = mHerderSCPDriver.nextConsensusLedgerIndex() +
                       LEDGER_VALIDITY_BRACKET;
    }
    else if (!checkCloseTime(envelope,
                             minLedgerSeq <= LedgerManager::GENESIS_LEDGER_SEQ))
    {
        // if we've never been in sync, we can be more aggressive in how we
        // filter messages: we can ignore messages that are unlikely to be
        // the latest messages from the network
        CLOG(TRACE, "Herder") << "recvSCPEnvelope: skipping invalid close time "
                                 "(check MAXIMUM_LEDGER_CLOSETIME_DRIFT)";
        std::string txt("DISCARDED - invalid close time");
        ZoneText(txt.c_str(), txt.size());
        return Herder::ENVELOPE_STATUS_DISCARDED;
    }

    // If envelopes are out of our validity brackets, we just ignore them.
    if (envelope.statement.slotIndex > maxLedgerSeq ||
        envelope.statement.slotIndex < minLedgerSeq)
    {
        CLOG(TRACE, "Herder") << "Ignoring SCPEnvelope outside of range: "
                              << envelope.statement.slotIndex << "( "
                              << minLedgerSeq << "," << maxLedgerSeq << ")";
        std::string txt("DISCARDED - out of range");
        ZoneText(txt.c_str(), txt.size());
        return Herder::ENVELOPE_STATUS_DISCARDED;
    }

    if (envelope.statement.nodeID == getSCP().getLocalNode()->getNodeID())
    {
        CLOG(TRACE, "Herder") << "recvSCPEnvelope: skipping own message";
        std::string txt("SKIPPED_SELF");
        ZoneText(txt.c_str(), txt.size());
        return Herder::ENVELOPE_STATUS_SKIPPED_SELF;
    }

    auto status = mPendingEnvelopes.recvSCPEnvelope(envelope);
    if (status == Herder::ENVELOPE_STATUS_READY)
    {
        std::string txt("READY");
        ZoneText(txt.c_str(), txt.size());
        if (Logging::logDebug("Herder"))
        {
            CLOG(DEBUG, "Herder")
                << "recvSCPEnvelope (ready) from: "
                << mApp.getConfig().toShortString(envelope.statement.nodeID)
                << " s:" << envelope.statement.pledges.type()
                << " i:" << envelope.statement.slotIndex
                << " a:" << mApp.getStateHuman();
        }

        processSCPQueue();
    }
    else
    {
        if (status == Herder::ENVELOPE_STATUS_FETCHING)
        {
            std::string txt("FETCHING");
            ZoneText(txt.c_str(), txt.size());
        }
        else if (status == Herder::ENVELOPE_STATUS_PROCESSED)
        {
            std::string txt("PROCESSED");
            ZoneText(txt.c_str(), txt.size());
        }
        if (Logging::logTrace("Herder"))
        {
            CLOG(TRACE, "Herder")
                << "recvSCPEnvelope (" << status << ") from: "
                << mApp.getConfig().toShortString(envelope.statement.nodeID)
                << " s:" << envelope.statement.pledges.type()
                << " i:" << envelope.statement.slotIndex
                << " a:" << mApp.getStateHuman();
        }
    }
    return status;
}

Herder::EnvelopeStatus
HerderImpl::recvSCPEnvelope(SCPEnvelope const& envelope,
                            const SCPQuorumSet& qset, TxSetFrame txset)
{
    ZoneScoped;
    mPendingEnvelopes.addTxSet(txset.getContentsHash(),
                               envelope.statement.slotIndex,
                               std::make_shared<TxSetFrame>(txset));
    mPendingEnvelopes.addSCPQuorumSet(sha256(xdr::xdr_to_opaque(qset)), qset);
    return recvSCPEnvelope(envelope);
}

void
HerderImpl::sendSCPStateToPeer(uint32 ledgerSeq, Peer::pointer peer)
{
    ZoneScoped;
    bool log = true;
    getSCP().processSlotsAscendingFrom(ledgerSeq, [&](uint64 seq) {
        getSCP().processCurrentState(seq,
                                     [&](SCPEnvelope const& e) {
                                         StellarMessage m;
                                         m.type(SCP_MESSAGE);
                                         m.envelope() = e;
                                         peer->sendMessage(m, log);
                                         log = false;
                                         return true;
                                     },
                                     false);
        return true;
    });
}

void
HerderImpl::processSCPQueue()
{
    ZoneScoped;
    if (mHerderSCPDriver.trackingSCP())
    {
        std::string txt("tracking");
        ZoneText(txt.c_str(), txt.size());
        processSCPQueueUpToIndex(mHerderSCPDriver.nextConsensusLedgerIndex());
    }
    else
    {
        std::string txt("not tracking");
        ZoneText(txt.c_str(), txt.size());
        // we don't know which ledger we're in
        // try to consume the messages from the queue
        // starting from the smallest slot
        for (auto& slot : mPendingEnvelopes.readySlots())
        {
            processSCPQueueUpToIndex(slot);
            if (mHerderSCPDriver.trackingSCP())
            {
                // one of the slots externalized
                // we go back to regular flow
                break;
            }
        }
    }
}

void
HerderImpl::processSCPQueueUpToIndex(uint64 slotIndex)
{
    ZoneScoped;
    while (true)
    {
        SCPEnvelopeWrapperPtr envW = mPendingEnvelopes.pop(slotIndex);
        if (envW)
        {
            auto r = getSCP().receiveEnvelope(envW);
            if (r == SCP::EnvelopeState::VALID)
            {
                auto const& env = envW->getEnvelope();
                auto const& st = env.statement;
                if (st.pledges.type() == SCP_ST_EXTERNALIZE)
                {
                    mHerderSCPDriver.recordSCPExternalizeEvent(
                        st.slotIndex, st.nodeID, false);
                }
                mPendingEnvelopes.envelopeProcessed(env);
            }
        }
        else
        {
            return;
        }
    }
}

#ifdef BUILD_TESTS
PendingEnvelopes&
HerderImpl::getPendingEnvelopes()
{
    return mPendingEnvelopes;
}

TransactionQueue&
HerderImpl::getTransactionQueue()
{
    return mTransactionQueue;
}
#endif

void
HerderImpl::ledgerClosed(bool synchronous)
{
    // this method is triggered every time the most recent ledger is
    // externalized it performs some cleanup and also decides if it needs to
    // schedule triggering the next ledger

    ZoneScoped;
    mTriggerTimer.cancel();

    CLOG(TRACE, "Herder") << "HerderImpl::ledgerClosed";

    auto lastIndex = mHerderSCPDriver.lastConsensusLedgerIndex();
    uint64_t nextIndex = mHerderSCPDriver.nextConsensusLedgerIndex();

    mPendingEnvelopes.slotClosed(lastIndex);

    mApp.getOverlayManager().ledgerClosed(lastIndex);

    if (mLedgerManager.isSynced())
    {
        // if we're in sync, we setup mTriggerTimer
        // it may get cancelled if a more recent ledger externalizes

        auto seconds = mApp.getConfig().getExpectedLedgerCloseTime();

        // bootstrap with a pessimistic estimate of when
        // the ballot protocol started last
        auto lastBallotStart = mApp.getClock().now() - seconds;
        auto lastStart = mHerderSCPDriver.getPrepareStart(lastIndex);
        if (lastStart)
        {
            lastBallotStart = *lastStart;
        }
        // even if ballot protocol started before triggering, we just use that
        // time as reference point for triggering again (this may trigger right
        // away if externalizing took a long time)
        mTriggerTimer.expires_at(lastBallotStart + seconds);

        if (!mApp.getConfig().MANUAL_CLOSE)
            mTriggerTimer.async_wait(
                std::bind(&HerderImpl::triggerNextLedger, this,
                          static_cast<uint32_t>(nextIndex), true),
                &VirtualTimer::onFailureNoop);
    }
    else
    {
        CLOG(DEBUG, "Herder")
            << "Not presently synced, not triggering ledger-close.";
    }

    // process any statements up to the next slot
    // this may cause it to externalize
    auto processSCPQueueSomeMore = [this, nextIndex]() {
        if (mApp.isStopping())
        {
            return;
        }
        processSCPQueueUpToIndex(nextIndex);
    };

    if (synchronous)
    {
        processSCPQueueSomeMore();
    }
    else
    {
        mApp.postOnMainThread(processSCPQueueSomeMore,
                              "processSCPQueueSomeMore");
    }
}

bool
HerderImpl::recvSCPQuorumSet(Hash const& hash, const SCPQuorumSet& qset)
{
    ZoneScoped;
    return mPendingEnvelopes.recvSCPQuorumSet(hash, qset);
}

bool
HerderImpl::recvTxSet(Hash const& hash, const TxSetFrame& t)
{
    ZoneScoped;
    auto txset = std::make_shared<TxSetFrame>(t);
    return mPendingEnvelopes.recvTxSet(hash, txset);
}

void
HerderImpl::peerDoesntHave(MessageType type, uint256 const& itemID,
                           Peer::pointer peer)
{
    ZoneScoped;
    mPendingEnvelopes.peerDoesntHave(type, itemID, peer);
}

TxSetFramePtr
HerderImpl::getTxSet(Hash const& hash)
{
    return mPendingEnvelopes.getTxSet(hash);
}

SCPQuorumSetPtr
HerderImpl::getQSet(Hash const& qSetHash)
{
    return mHerderSCPDriver.getQSet(qSetHash);
}

uint32_t
HerderImpl::getCurrentLedgerSeq() const
{
    uint32_t res = mLedgerManager.getLastClosedLedgerNum();

    if (mHerderSCPDriver.trackingSCP() &&
        res < mHerderSCPDriver.trackingSCP()->mConsensusIndex)
    {
        res = static_cast<uint32_t>(
            mHerderSCPDriver.trackingSCP()->mConsensusIndex);
    }
    if (mHerderSCPDriver.lastTrackingSCP() &&
        res < mHerderSCPDriver.lastTrackingSCP()->mConsensusIndex)
    {
        res = static_cast<uint32_t>(
            mHerderSCPDriver.lastTrackingSCP()->mConsensusIndex);
    }
    return res;
}

SequenceNumber
HerderImpl::getMaxSeqInPendingTxs(AccountID const& acc)
{
    return mTransactionQueue.getAccountTransactionQueueInfo(acc).mMaxSeq;
}

void
HerderImpl::setInSyncAndTriggerNextLedger()
{
    // We either have not set trigger timer, or we're in the
    // middle of a consensus round. Either way, we do not want
    // to trigger ledger, as the node is already making progress
    if (mTriggerTimer.seq() > 0)
    {
        CLOG(DEBUG, "Herder") << "Skipping setInSyncAndTriggerNextLedger: "
                                 "trigger timer already set";
        return;
    }

    // Bring Herder and LM in sync in case they aren't
    if (mLedgerManager.getState() == LedgerManager::LM_BOOTING_STATE)
    {
        mLedgerManager.moveToSynced();
    }

    // Trigger next ledger, without requiring Herder to properly track SCP
    auto lcl = mLedgerManager.getLastClosedLedgerNum();
    triggerNextLedger(lcl + 1, false);
}

// called to take a position during the next round
// uses the state in LedgerManager to derive a starting position
void
HerderImpl::triggerNextLedger(uint32_t ledgerSeqToTrigger,
                              bool checkTrackingSCP)
{
    ZoneScoped;
    ZoneValue(static_cast<int64_t>(ledgerSeqToTrigger));

    auto isTrackingValid = mHerderSCPDriver.trackingSCP() || !checkTrackingSCP;

    if (!isTrackingValid || !mLedgerManager.isSynced())
    {
        CLOG(DEBUG, "Herder") << "triggerNextLedger: skipping (out of sync) : "
                              << mApp.getStateHuman();
        return;
    }

    // our first choice for this round's set is all the tx we have collected
    // during last few ledger closes
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    auto proposedSet = mTransactionQueue.toTxSet(lcl);

    auto upperBoundCloseTimeOffset =
        getUpperBoundCloseTimeOffset(mApp, lcl.header.scpValue.closeTime);

    auto removed = proposedSet->trimInvalid(mApp, upperBoundCloseTimeOffset);
    mTransactionQueue.ban(removed);

    proposedSet->surgePricingFilter(mApp);

    // we not only check that the value is valid for consensus (offset=0) but
    // also that we performed the proper cleanup above
    if (!proposedSet->checkValid(mApp, upperBoundCloseTimeOffset))
    {
        throw std::runtime_error("wanting to emit an invalid txSet");
    }

    auto txSetHash = proposedSet->getContentsHash();

    // use the slot index from ledger manager here as our vote is based off
    // the last closed ledger stored in ledger manager
    uint32_t slotIndex = lcl.header.ledgerSeq + 1;

    // Inform the item fetcher so queries from other peers about his txSet
    // can be answered. Note this can trigger SCP callbacks, externalize, etc
    // if we happen to build a txset that we were trying to download.
    mPendingEnvelopes.addTxSet(txSetHash, slotIndex, proposedSet);

    // no point in sending out a prepare:
    // externalize was triggered on a more recent ledger
    if (ledgerSeqToTrigger != slotIndex)
    {
        return;
    }

    // We pick as next close time the current time unless it's before the last
    // close time. We don't know how much time it will take to reach consensus
    // so this is the most appropriate value to use as closeTime.
    uint64_t nextCloseTime =
        VirtualClock::to_time_t(mApp.getClock().system_now());
    if (nextCloseTime <= lcl.header.scpValue.closeTime)
    {
        nextCloseTime = lcl.header.scpValue.closeTime + 1;
    }

    StellarValue newProposedValue(txSetHash, nextCloseTime, emptyUpgradeSteps,
                                  STELLAR_VALUE_BASIC);

    // see if we need to include some upgrades
    auto upgrades = mUpgrades.createUpgradesFor(lcl.header);
    for (auto const& upgrade : upgrades)
    {
        Value v(xdr::xdr_to_opaque(upgrade));
        if (v.size() >= UpgradeType::max_size())
        {
            CLOG(ERROR, "Herder")
                << "HerderImpl::triggerNextLedger"
                << " exceeded size for upgrade step (got " << v.size()
                << " ) for upgrade type " << std::to_string(upgrade.type());
            CLOG(ERROR, "Herder") << REPORT_INTERNAL_BUG;
        }
        else
        {
            newProposedValue.upgrades.emplace_back(v.begin(), v.end());
        }
    }

    getHerderSCPDriver().recordSCPEvent(slotIndex, true);

    // If we are not a validating node we stop here and don't start nomination
    if (!getSCP().isValidator())
    {
        CLOG(DEBUG, "Herder")
            << "Non-validating node, skipping nomination (SCP).";
        return;
    }

    if (lcl.header.ledgerVersion >= 11)
    {
        // version 11 and above require values to be signed during nomination
        signStellarValue(mApp.getConfig().NODE_SEED, newProposedValue);
    }
    mHerderSCPDriver.nominate(slotIndex, newProposedValue, proposedSet,
                              lcl.header.scpValue);
}

void
HerderImpl::setUpgrades(Upgrades::UpgradeParameters const& upgrades)
{
    mUpgrades.setParameters(upgrades, mApp.getConfig());
    persistUpgrades();

    auto desc = mUpgrades.toString();

    if (!desc.empty())
    {
        auto message = fmt::format("Armed with network upgrades: {}", desc);
        auto prev = mApp.getStatusManager().getStatusMessage(
            StatusCategory::REQUIRES_UPGRADES);
        if (prev != message)
        {
            CLOG(INFO, "Herder") << message;
            mApp.getStatusManager().setStatusMessage(
                StatusCategory::REQUIRES_UPGRADES, message);
        }
    }
    else
    {
        CLOG(INFO, "Herder") << "Network upgrades cleared";
        mApp.getStatusManager().removeStatusMessage(
            StatusCategory::REQUIRES_UPGRADES);
    }
}

std::string
HerderImpl::getUpgradesJson()
{
    return mUpgrades.getParameters().toJson();
}

bool
HerderImpl::resolveNodeID(std::string const& s, PublicKey& retKey)
{
    bool r = mApp.getConfig().resolveNodeID(s, retKey);
    if (!r)
    {
        if (s.size() > 1 && s[0] == '@')
        {
            std::string arg = s.substr(1);
            // go through SCP messages of the previous ledger
            // (to increase the chances of finding the node)
            uint32 seq = getCurrentLedgerSeq();
            if (seq > 2)
            {
                seq--;
            }
            getSCP().processCurrentState(
                seq,
                [&](SCPEnvelope const& e) {
                    std::string curK = KeyUtils::toStrKey(e.statement.nodeID);
                    if (curK.compare(0, arg.size(), arg) == 0)
                    {
                        retKey = e.statement.nodeID;
                        r = true;
                        return false;
                    }
                    return true;
                },
                true);
        }
    }
    return r;
}

Json::Value
HerderImpl::getJsonInfo(size_t limit, bool fullKeys)
{
    Json::Value ret;
    ret["you"] = mApp.getConfig().toStrKey(
        mApp.getConfig().NODE_SEED.getPublicKey(), fullKeys);

    ret["scp"] = getSCP().getJsonInfo(limit, fullKeys);
    ret["queue"] = mPendingEnvelopes.getJsonInfo(limit);
    return ret;
}

Json::Value
HerderImpl::getJsonTransitiveQuorumIntersectionInfo(bool fullKeys) const
{
    Json::Value ret;
    ret["intersection"] =
        mLastQuorumMapIntersectionState.enjoysQuorunIntersection();
    ret["node_count"] =
        static_cast<Json::UInt64>(mLastQuorumMapIntersectionState.mNumNodes);
    ret["last_check_ledger"] = static_cast<Json::UInt64>(
        mLastQuorumMapIntersectionState.mLastCheckLedger);
    if (mLastQuorumMapIntersectionState.enjoysQuorunIntersection())
    {
        Json::Value critical;
        for (auto const& group :
             mLastQuorumMapIntersectionState.mIntersectionCriticalNodes)
        {
            Json::Value jg;
            for (auto const& k : group)
            {
                auto s = mApp.getConfig().toStrKey(k, fullKeys);
                jg.append(s);
            }
            critical.append(jg);
        }
        ret["critical"] = critical;
    }
    else
    {
        ret["last_good_ledger"] = static_cast<Json::UInt64>(
            mLastQuorumMapIntersectionState.mLastGoodLedger);
        Json::Value split, a, b;
        auto const& pair = mLastQuorumMapIntersectionState.mPotentialSplit;
        for (auto const& k : pair.first)
        {
            auto s = mApp.getConfig().toStrKey(k, fullKeys);
            a.append(s);
        }
        for (auto const& k : pair.second)
        {
            auto s = mApp.getConfig().toStrKey(k, fullKeys);
            b.append(s);
        }
        split.append(a);
        split.append(b);
        ret["potential_split"] = split;
    }
    return ret;
}

Json::Value
HerderImpl::getJsonQuorumInfo(NodeID const& id, bool summary, bool fullKeys,
                              uint64 index)
{
    Json::Value ret;
    ret["node"] = mApp.getConfig().toStrKey(id, fullKeys);
    ret["qset"] = getSCP().getJsonQuorumInfo(id, summary, fullKeys, index);
    bool isSelf = id == mApp.getConfig().NODE_SEED.getPublicKey();
    if (isSelf && mLastQuorumMapIntersectionState.hasAnyResults())
    {
        ret["transitive"] = getJsonTransitiveQuorumIntersectionInfo(fullKeys);
    }
    return ret;
}

Json::Value
HerderImpl::getJsonTransitiveQuorumInfo(NodeID const& rootID, bool summary,
                                        bool fullKeys)
{
    Json::Value ret;
    bool isSelf = rootID == mApp.getConfig().NODE_SEED.getPublicKey();
    if (isSelf && mLastQuorumMapIntersectionState.hasAnyResults())
    {
        ret = getJsonTransitiveQuorumIntersectionInfo(fullKeys);
    }

    Json::Value& nodes = ret["nodes"];

    auto& q = mPendingEnvelopes.getCurrentlyTrackedQuorum();

    auto rootLatest = getSCP().getLatestMessage(rootID);
    std::map<Value, int> knownValues;

    // walk the quorum graph, starting at id
    std::unordered_set<NodeID> visited;
    std::vector<NodeID> next;
    next.push_back(rootID);
    visited.emplace(rootID);
    int distance = 0;
    int valGenID = 0;
    while (!next.empty())
    {
        std::vector<NodeID> frontier(std::move(next));
        next.clear();
        std::sort(frontier.begin(), frontier.end());
        for (auto const& id : frontier)
        {
            Json::Value cur;
            valGenID++;
            cur["node"] = mApp.getConfig().toStrKey(id, fullKeys);
            if (!summary)
            {
                cur["distance"] = distance;
            }
            auto it = q.find(id);
            std::string status;
            if (it != q.end())
            {
                auto qSet = it->second;
                if (qSet)
                {
                    if (!summary)
                    {
                        cur["qset"] =
                            getSCP().getLocalNode()->toJson(*qSet, fullKeys);
                    }
                    LocalNode::forAllNodes(*qSet, [&](NodeID const& n) {
                        auto b = visited.emplace(n);
                        if (b.second)
                        {
                            next.emplace_back(n);
                        }
                    });
                }
                auto latest = getSCP().getLatestMessage(id);
                if (latest)
                {
                    auto vals = Slot::getStatementValues(latest->statement);
                    // updates the `knownValues` map, and generate a unique ID
                    // for the value (heuristic to group votes)
                    int trackingValID = -1;
                    Value const* trackingValue = nullptr;
                    for (auto const& v : vals)
                    {
                        auto p =
                            knownValues.insert(std::make_pair(v, valGenID));
                        if (p.first->second > trackingValID)
                        {
                            trackingValID = p.first->second;
                            trackingValue = &v;
                        }
                    }

                    cur["heard"] =
                        static_cast<Json::UInt64>(latest->statement.slotIndex);
                    if (!summary)
                    {
                        cur["value"] = trackingValue
                                           ? mHerderSCPDriver.getValueString(
                                                 *trackingValue)
                                           : "";
                        cur["value_id"] = trackingValID;
                    }
                    // give a sense of how this node is doing compared to rootID
                    if (rootLatest)
                    {
                        if (latest->statement.slotIndex <
                            rootLatest->statement.slotIndex)
                        {
                            status = "behind";
                        }
                        else if (latest->statement.slotIndex >
                                 rootLatest->statement.slotIndex)
                        {
                            status = "ahead";
                        }
                        else
                        {
                            status = "tracking";
                        }
                    }
                }
                else
                {
                    status = "missing";
                }
            }
            else
            {
                status = "unknown";
            }
            cur["status"] = status;
            nodes.append(cur);
        }
        distance++;
    }
    return ret;
}

QuorumTracker::QuorumMap const&
HerderImpl::getCurrentlyTrackedQuorum() const
{
    return mPendingEnvelopes.getCurrentlyTrackedQuorum();
}

static Hash
getQmapHash(QuorumTracker::QuorumMap const& qmap)
{
    ZoneScoped;
    SHA256 hasher;
    std::map<NodeID, SCPQuorumSetPtr> ordered_map(qmap.begin(), qmap.end());
    for (auto const& pair : ordered_map)
    {
        hasher.add(xdr::xdr_to_opaque(pair.first));
        if (pair.second)
        {
            hasher.add(xdr::xdr_to_opaque(*pair.second));
        }
        else
        {
            hasher.add("\0");
        }
    }
    return hasher.finish();
}

void
HerderImpl::checkAndMaybeReanalyzeQuorumMap()
{
    if (!mApp.getConfig().QUORUM_INTERSECTION_CHECKER)
    {
        return;
    }
    ZoneScoped;
    QuorumTracker::QuorumMap const& qmap = getCurrentlyTrackedQuorum();
    Hash curr = getQmapHash(qmap);
    if (mLastQuorumMapIntersectionState.mLastCheckQuorumMapHash == curr)
    {
        // Everything's stable, nothing to do.
        return;
    }

    if (mLastQuorumMapIntersectionState.mRecalculating)
    {
        // Already recalculating. If we're recalculating for the hash we want,
        // we do nothing, just wait for it to finish. If we're recalculating for
        // a hash that has changed _again_ (since the calculation started), we
        // _interrupt_ the calculation-in-progress: we'll return to this
        // function on the next externalize and start a new calculation for the
        // new hash we want.
        if (mLastQuorumMapIntersectionState.mCheckingQuorumMapHash == curr)
        {
            CLOG(DEBUG, "Herder")
                << "Transitive closure of quorum has changed, "
                << "already analyzing new configuration.";
        }
        else
        {
            CLOG(DEBUG, "Herder")
                << "Transitive closure of quorum has changed, "
                << "interrupting existing analysis.";
            mLastQuorumMapIntersectionState.mInterruptFlag = true;
        }
    }
    else
    {
        CLOG(INFO, "Herder")
            << "Transitive closure of quorum has changed, re-analyzing.";
        // Not currently recalculating: start doing so.
        mLastQuorumMapIntersectionState.mRecalculating = true;
        mLastQuorumMapIntersectionState.mInterruptFlag = false;
        mLastQuorumMapIntersectionState.mCheckingQuorumMapHash = curr;
        auto& cfg = mApp.getConfig();
        auto qic = QuorumIntersectionChecker::create(
            qmap, cfg, mLastQuorumMapIntersectionState.mInterruptFlag);
        auto ledger = getCurrentLedgerSeq();
        auto nNodes = qmap.size();
        auto& hState = mLastQuorumMapIntersectionState;
        auto& app = mApp;
        auto worker = [curr, ledger, nNodes, qic, qmap, cfg, &app, &hState] {
            try
            {
                ZoneScoped;
                bool ok = qic->networkEnjoysQuorumIntersection();
                auto split = qic->getPotentialSplit();
                std::set<std::set<PublicKey>> critical;
                if (ok)
                {
                    // Only bother calculating the _critical_ groups if we're
                    // intersecting; if not intersecting we should finish ASAP
                    // and raise an alarm.
                    critical = QuorumIntersectionChecker::
                        getIntersectionCriticalGroups(qmap, cfg,
                                                      hState.mInterruptFlag);
                }
                app.postOnMainThread(
                    [ok, curr, ledger, nNodes, split, critical, &hState] {
                        hState.mRecalculating = false;
                        hState.mInterruptFlag = false;
                        hState.mNumNodes = nNodes;
                        hState.mLastCheckLedger = ledger;
                        hState.mLastCheckQuorumMapHash = curr;
                        hState.mCheckingQuorumMapHash = Hash{};
                        hState.mPotentialSplit = split;
                        hState.mIntersectionCriticalNodes = critical;
                        if (ok)
                        {
                            hState.mLastGoodLedger = ledger;
                        }
                    },
                    "QuorumIntersectionChecker finished");
            }
            catch (QuorumIntersectionChecker::InterruptedException&)
            {
                CLOG(DEBUG, "Herder")
                    << "Quorum transitive closure analysis interrupted.";
                app.postOnMainThread(
                    [&hState] {
                        hState.mRecalculating = false;
                        hState.mInterruptFlag = false;
                        hState.mCheckingQuorumMapHash = Hash{};
                    },
                    "QuorumIntersectionChecker interrupted");
            }
        };
        mApp.postOnBackgroundThread(worker, "QuorumIntersectionChecker");
    }
}

void
HerderImpl::persistSCPState(uint64 slot)
{
    ZoneScoped;
    if (slot < mLastSlotSaved)
    {
        return;
    }

    mLastSlotSaved = slot;

    // saves SCP messages and related data (transaction sets, quorum sets)
    xdr::xvector<SCPEnvelope> latestEnvs;
    std::map<Hash, TxSetFramePtr> txSets;
    std::map<Hash, SCPQuorumSetPtr> quorumSets;

    for (auto const& e : getSCP().getLatestMessagesSend(slot))
    {
        latestEnvs.emplace_back(e);

        // saves transaction sets referred by the statement
        for (auto const& h : getTxSetHashes(e))
        {
            auto txSet = mPendingEnvelopes.getTxSet(h);
            if (txSet)
            {
                txSets.insert(std::make_pair(h, txSet));
            }
        }
        Hash qsHash = Slot::getCompanionQuorumSetHashFromStatement(e.statement);
        SCPQuorumSetPtr qSet = mPendingEnvelopes.getQSet(qsHash);
        if (qSet)
        {
            quorumSets.insert(std::make_pair(qsHash, qSet));
        }
    }

    xdr::xvector<TransactionSet> latestTxSets;
    for (auto it : txSets)
    {
        latestTxSets.emplace_back();
        it.second->toXDR(latestTxSets.back());
    }

    xdr::xvector<SCPQuorumSet> latestQSets;
    for (auto it : quorumSets)
    {
        latestQSets.emplace_back(*it.second);
    }

    auto latestSCPData =
        xdr::xdr_to_opaque(latestEnvs, latestTxSets, latestQSets);
    std::string scpState;
    scpState = decoder::encode_b64(latestSCPData);

    mApp.getPersistentState().setSCPStateForSlot(slot, scpState);
}

void
HerderImpl::restoreSCPState()
{
    ZoneScoped;
    // setup a sufficient state that we can participate in consensus
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    if (!mApp.getConfig().FORCE_SCP &&
        lcl.header.ledgerSeq == LedgerManager::GENESIS_LEDGER_SEQ)
    {
        // if we're on genesis ledger, there is no point in claiming
        // that we're "in sync"
        return;
    }

    mHerderSCPDriver.restoreSCPState(lcl.header.ledgerSeq, lcl.header.scpValue);

    trackingHeartBeat();

    // load saved state from database
    auto latest64 = mApp.getPersistentState().getSCPStateAllSlots();
    for (auto const& state : latest64)
    {
        std::vector<uint8_t> buffer;
        decoder::decode_b64(state, buffer);

        xdr::xvector<SCPEnvelope> latestEnvs;
        xdr::xvector<TransactionSet> latestTxSets;
        xdr::xvector<SCPQuorumSet> latestQSets;

        try
        {
            xdr::xdr_from_opaque(buffer, latestEnvs, latestTxSets, latestQSets);

            for (auto const& txset : latestTxSets)
            {
                TxSetFramePtr cur =
                    make_shared<TxSetFrame>(mApp.getNetworkID(), txset);
                Hash h = cur->getContentsHash();
                mPendingEnvelopes.addTxSet(h, 0, cur);
            }
            for (auto const& qset : latestQSets)
            {
                Hash hash = sha256(xdr::xdr_to_opaque(qset));
                mPendingEnvelopes.addSCPQuorumSet(hash, qset);
            }
            for (auto const& e : latestEnvs)
            {
                auto envW = getHerderSCPDriver().wrapEnvelope(e);
                getSCP().setStateFromEnvelope(e.statement.slotIndex, envW);
                mLastSlotSaved =
                    std::max<uint64>(mLastSlotSaved, e.statement.slotIndex);
            }
        }
        catch (std::exception& e)
        {
            // we may have exceptions when upgrading the protocol
            // this should be the only time we get exceptions decoding old
            // messages.
            CLOG(INFO, "Herder") << "Error while restoring old scp messages, "
                                    "proceeding without them : "
                                 << e.what();
        }
        mPendingEnvelopes.rebuildQuorumTrackerState();
    }

    startRebroadcastTimer();
}

void
HerderImpl::persistUpgrades()
{
    ZoneScoped;
    auto s = mUpgrades.getParameters().toJson();
    mApp.getPersistentState().setState(PersistentState::kLedgerUpgrades, s);
}

void
HerderImpl::restoreUpgrades()
{
    ZoneScoped;
    std::string s =
        mApp.getPersistentState().getState(PersistentState::kLedgerUpgrades);
    if (!s.empty())
    {
        Upgrades::UpgradeParameters p;
        p.fromJson(s);
        try
        {
            // use common code to set status
            setUpgrades(p);
        }
        catch (std::exception e)
        {
            CLOG(INFO, "Herder") << "Error restoring upgrades '" << e.what()
                                 << "' with upgrades '" << s << "'";
        }
    }
}

void
HerderImpl::restoreState()
{
    restoreSCPState();
    restoreUpgrades();
}

void
HerderImpl::trackingHeartBeat()
{
    if (mApp.getConfig().MANUAL_CLOSE)
    {
        return;
    }

    assert(mHerderSCPDriver.trackingSCP());
    mTrackingTimer.expires_from_now(
        std::chrono::seconds(CONSENSUS_STUCK_TIMEOUT_SECONDS));
    mTrackingTimer.async_wait(std::bind(&HerderImpl::herderOutOfSync, this),
                              &VirtualTimer::onFailureNoop);
}

void
HerderImpl::updateTransactionQueue(
    std::vector<TransactionFrameBasePtr> const& applied)
{
    ZoneScoped;
    // remove all these tx from mTransactionQueue
    mTransactionQueue.removeApplied(applied);
    mTransactionQueue.shift();

    // Transactions in the queue need to be updated after the protocol 13
    // upgrade
    auto replacedTxs = mTransactionQueue.maybeVersionUpgraded();
    for (auto const& replacedTx : replacedTxs)
    {
        mApp.getOverlayManager().updateFloodRecord(
            replacedTx.mOld->toStellarMessage(),
            replacedTx.mNew->toStellarMessage());
    }

    // Generate a transaction set from a random hash and drop invalid
    auto lhhe = mLedgerManager.getLastClosedLedgerHeader();
    lhhe.hash = HashUtils::random();
    auto txSet = mTransactionQueue.toTxSet(lhhe);

    auto removed = txSet->trimInvalid(
        mApp,
        getUpperBoundCloseTimeOffset(mApp, lhhe.header.scpValue.closeTime));
    mTransactionQueue.ban(removed);

    // Rebroadcast transactions, sorted in apply-order to maximize chances of
    // propagation. Do not broadcast more operations than
    // OPERATION_BROADCAST_MULTIPLIER times the maximum number of operations
    // that can be included in the next ledger.
    size_t maxOps = mLedgerManager.getLastMaxTxSetSizeOps();
    size_t opsToFlood = OPERATION_BROADCAST_MULTIPLIER * maxOps;
    for (auto tx : txSet->sortForApply())
    {
        if (opsToFlood < tx->getNumOperations())
        {
            break;
        }
        opsToFlood -= tx->getNumOperations();

        auto msg = tx->toStellarMessage();
        mApp.getOverlayManager().broadcastMessage(msg);
    }
}

void
HerderImpl::herderOutOfSync()
{
    ZoneScoped;
    CLOG(WARNING, "Herder") << "Lost track of consensus";

    auto s = getJsonInfo(20).toStyledString();
    CLOG(WARNING, "Herder") << "Out of sync context: " << s;

    mSCPMetrics.mLostSync.Mark();
    mHerderSCPDriver.lostSync();

    getMoreSCPState();

    processSCPQueue();
}

void
HerderImpl::getMoreSCPState()
{
    ZoneScoped;
    int const NB_PEERS_TO_ASK = 2;
    auto low = mApp.getLedgerManager().getLastClosedLedgerNum() + 1;
    auto maxSlotsToRemember = mApp.getConfig().MAX_SLOTS_TO_REMEMBER;
    if (low > maxSlotsToRemember)
    {
        low -= maxSlotsToRemember;
    }
    else
    {
        low = 1;
    }

    // ask a few random peers their SCP messages
    auto r = mApp.getOverlayManager().getRandomAuthenticatedPeers();
    for (int i = 0; i < NB_PEERS_TO_ASK && i < r.size(); i++)
    {
        r[i]->sendGetScpState(low);
    }
}

bool
HerderImpl::verifyEnvelope(SCPEnvelope const& envelope)
{
    ZoneScoped;
    auto b = PubKeyUtils::verifySig(
        envelope.statement.nodeID, envelope.signature,
        xdr::xdr_to_opaque(mApp.getNetworkID(), ENVELOPE_TYPE_SCP,
                           envelope.statement));
    if (b)
    {
        mSCPMetrics.mEnvelopeValidSig.Mark();
    }
    else
    {
        mSCPMetrics.mEnvelopeInvalidSig.Mark();
    }

    return b;
}
void
HerderImpl::signEnvelope(SecretKey const& s, SCPEnvelope& envelope)
{
    ZoneScoped;
    envelope.signature = s.sign(xdr::xdr_to_opaque(
        mApp.getNetworkID(), ENVELOPE_TYPE_SCP, envelope.statement));
}
bool
HerderImpl::verifyStellarValueSignature(StellarValue const& sv)
{
    ZoneScoped;
    auto b = PubKeyUtils::verifySig(
        sv.ext.lcValueSignature().nodeID, sv.ext.lcValueSignature().signature,
        xdr::xdr_to_opaque(mApp.getNetworkID(), ENVELOPE_TYPE_SCPVALUE,
                           sv.txSetHash, sv.closeTime));
    return b;
}

void
HerderImpl::signStellarValue(SecretKey const& s, StellarValue& sv)
{
    ZoneScoped;
    sv.ext.v(STELLAR_VALUE_SIGNED);
    sv.ext.lcValueSignature().nodeID = s.getPublicKey();
    sv.ext.lcValueSignature().signature =
        s.sign(xdr::xdr_to_opaque(mApp.getNetworkID(), ENVELOPE_TYPE_SCPVALUE,
                                  sv.txSetHash, sv.closeTime));
}
}
