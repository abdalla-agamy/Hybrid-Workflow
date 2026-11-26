#include "DTHSBPNodeApp.h"
#include "SwarmMobility.h"
#include "SwarmScenarioManager.h"

#include "inet/common/Simsignals.h"
#include "inet/common/packet/Packet.h"
#include "inet/common/packet/chunk/BytesChunk.h"
#include "inet/linklayer/common/InterfaceTag_m.h"
#include "inet/linklayer/common/UserPriorityTag_m.h"
#include "inet/networklayer/common/L3AddressTag_m.h"
#include "inet/networklayer/common/NetworkInterface.h"
#include "inet/networklayer/contract/IInterfaceTable.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace dthsbp_v2 {

namespace {
constexpr uint16_t kFlagTailTruncated = 0x0001;
constexpr uint16_t kFlagDtPrecomputed = 0x0002;
constexpr uint16_t kFlagCollateral    = 0x0004;
constexpr uint16_t kFlagResyncReply   = 0x0008;
constexpr uint16_t kFlagSlPromotion   = 0x0010;
constexpr uint16_t kFlagEscalate      = 0x0020;
constexpr uint16_t kFlagJoinAdmission = 0x0040;
constexpr uint16_t kFlagJoinRequest   = 0x0080;

class RelayForwardMsg : public omnetpp::cMessage {
public:
    uint64_t dedupKey = 0;
    V2WireHeader hdr{};
    std::vector<uint8_t> rawBytes;
    RelayForwardMsg(const char* name, int kind) : omnetpp::cMessage(name, kind) {}
};

class AckDelayMsg : public omnetpp::cMessage {
public:
    int destSlotIdx = -1;
    uint32_t roundId = 0;
    uint64_t keyEpoch = 0;
    AckDelayMsg(const char* name, int kind) : omnetpp::cMessage(name, kind) {}
};

class RoundTimerMsg : public omnetpp::cMessage {
public:
    uint32_t roundId = 0;
    RoundTimerMsg(const char* name, int kind, uint32_t rid)
        : omnetpp::cMessage(name, kind), roundId(rid) {}
};

uint64_t makeDedupKey(int32_t origin, uint32_t seq, int16_t frag) {
    uint64_t o = static_cast<uint32_t>(origin) & 0xFFFFULL;
    uint64_t s = static_cast<uint64_t>(seq) & 0xFFFFFFFFULL;
    uint64_t f = static_cast<uint16_t>(frag) & 0xFFFFULL;
    return (o << 48) | (s << 16) | f;
}
} // namespace

Define_Module(DTHSBPNodeApp);

DTHSBPNodeApp::~DTHSBPNodeApp() {
    clearAllTimers();
}

void DTHSBPNodeApp::clearAllTimers() {
    cancelAndDelete(dtReportTimer_);
    dtReportTimer_ = nullptr;
    cancelAndDelete(chSummaryTimer_);
    chSummaryTimer_ = nullptr;
    cancelAndDelete(c2Timer_);
    c2Timer_ = nullptr;
    cancelAndDelete(watchdogTimer_);
    watchdogTimer_ = nullptr;

    for (auto& kv : pendingRelayTimers_) {
        cancelAndDelete(kv.second);
    }
    pendingRelayTimers_.clear();

    for (auto* m : transientTimers_) {
        cancelAndDelete(m);
    }
    transientTimers_.clear();
    pendingRounds_.clear();
}

void DTHSBPNodeApp::initialize(int stage) {
    ApplicationBase::initialize(stage);
    if (stage == inet::INITSTAGE_LOCAL) {
        localPort_ = par("localPort");
        relaySuppressionThreshold_ = par("relaySuppressionThreshold");
        ackDeadlineSec_ = par("ackDeadlineSec");
        resyncCooldownSec_ = par("resyncCooldownSec");
        slotIdx_ = getParentModule()->getIndex();
        manager_ = dynamic_cast<SwarmScenarioManager*>(getModuleByPath("^.^.scenarioManager"));
        mobility_ = dynamic_cast<SwarmMobility*>(getParentModule()->getSubmodule("mobility"));

        dtReportTimer_ = new inet::cMessage("DTReportTimer", TIMER_DT_REPORT);
        chSummaryTimer_ = new inet::cMessage("CHSummaryTimer", TIMER_CH_SUMMARY);
        c2Timer_ = new inet::cMessage("C2BcastTimer", TIMER_C2_BCAST);
        watchdogTimer_ = new inet::cMessage("WatchdogTimer", TIMER_WATCHDOG);
    }
}

void DTHSBPNodeApp::handleStartOperation(inet::LifecycleOperation* /*operation*/) {
    ensureWlanInterfaceId();
    socket_.setOutputGate(gate("socketOut"));
    socket_.bind(localPort_);
    socket_.setBroadcast(true);
    socket_.setCallback(this);
}

void DTHSBPNodeApp::handleStopOperation(inet::LifecycleOperation* /*operation*/) {
    clearAllTimers();
    socket_.close();
    delayActiveOperationFinish(par("stopOperationTimeout"));
}

void DTHSBPNodeApp::handleCrashOperation(inet::LifecycleOperation* /*operation*/) {
    clearAllTimers();
    socket_.destroy();
}

void DTHSBPNodeApp::ensureWlanInterfaceId() {
    if (wlanInterfaceId_ == -1) {
        if (auto* ift = dynamic_cast<inet::IInterfaceTable*>(getParentModule()->getSubmodule("interfaceTable"))) {
            if (auto* ie = ift->findInterfaceByName("wlan0")) {
                wlanInterfaceId_ = ie->getInterfaceId();
            }
        }
    }
}

void DTHSBPNodeApp::activateNode(
    int slotIdx,
    NodeRole role,
    int clusterId,
    int chSlotIdx,
    int slSlotIdx,
    uint64_t initialKeyEpoch
) {
    Enter_Method("activateNode");
    slotIdx_ = slotIdx;
    role_ = role;
    clusterId_ = clusterId;
    chSlotIdx_ = chSlotIdx;
    slSlotIdx_ = slSlotIdx;
    swarmKeyEpoch_ = initialKeyEpoch;
    clusterKeyEpoch_ = initialKeyEpoch;
    for (int c = 0; c < 5; ++c) {
        slClusterEpochs_[c] = 1;
    }
    simulatedLinkQ_ = 0.96;
    active_ = true;
    needsJoinAdmission_ = (initialKeyEpoch == 0);
    slFailoverTriggered_ = false;
    lastC2OrSlHeardSec_ = inet::simTime().dbl();
    desyncedSinceSec_ = (initialKeyEpoch == 0) ? inet::simTime().dbl() : -1.0;

    dtTracker_.clear();
    clusterMembers_.clear();
    excludedSlots_.clear();
    seenCache_.clear();
    rxFragAssemblies_.clear();
    relayOverhearCount_.clear();

    if (role_ == NodeRole::CLUSTER_HEAD && manager_) {
        const auto members = manager_->getInitialClusterMembers(clusterId_);
        for (int m : members) {
            if (m != slotIdx_) {
                clusterMembers_.insert(m);
            }
        }
    }

    cancelEvent(dtReportTimer_);
    cancelEvent(chSummaryTimer_);
    cancelEvent(c2Timer_);
    cancelEvent(watchdogTimer_);

    const double now = inet::simTime().dbl();
    if (role_ == NodeRole::FOLLOWER) {
        scheduleAt(now + uniform(0.02, manager_->getDtReportIntervalSec()), dtReportTimer_);
        if (initialKeyEpoch == 0) {
            sendDtReport();
        }
    } else if (role_ == NodeRole::CLUSTER_HEAD) {
        scheduleAt(now + uniform(0.5, manager_->getChSummaryIntervalSec()), chSummaryTimer_);
        scheduleAt(now + 0.50, watchdogTimer_);
    } else if (role_ == NodeRole::SWARM_LEADER) {
        scheduleAt(now + manager_->getC2BroadcastIntervalSec(), c2Timer_);
        scheduleAt(now + 0.50, watchdogTimer_);
    }
}

void DTHSBPNodeApp::deactivateNode() {
    Enter_Method("deactivateNode");
    active_ = false;
    cancelEvent(dtReportTimer_);
    cancelEvent(chSummaryTimer_);
    cancelEvent(c2Timer_);
    cancelEvent(watchdogTimer_);
    for (auto& kv : pendingRelayTimers_) {
        cancelAndDelete(kv.second);
    }
    pendingRelayTimers_.clear();
    for (auto* m : transientTimers_) {
        cancelAndDelete(m);
    }
    transientTimers_.clear();
    pendingRounds_.clear();
}

void DTHSBPNodeApp::setSimulatedLinkQuality(double linkQ) {
    Enter_Method("setSimulatedLinkQuality");
    simulatedLinkQ_ = std::clamp(linkQ, 0.05, 1.0);
}

void DTHSBPNodeApp::handleMessageWhenUp(inet::cMessage* msg) {
    if (msg->isSelfMessage()) {
        if (!active_) {
            if (msg != dtReportTimer_ && msg != chSummaryTimer_ && msg != c2Timer_ && msg != watchdogTimer_) {
                transientTimers_.erase(
                    std::remove(transientTimers_.begin(), transientTimers_.end(), msg),
                    transientTimers_.end()
                );
                if (auto* rf = dynamic_cast<RelayForwardMsg*>(msg)) {
                    pendingRelayTimers_.erase(rf->dedupKey);
                }
                delete msg;
            }
            return;
        }

        switch (msg->getKind()) {
            case TIMER_DT_REPORT:
                sendDtReport();
                scheduleAt(inet::simTime() + manager_->getDtReportIntervalSec(), dtReportTimer_);
                return;
            case TIMER_CH_SUMMARY:
                sendChSummary();
                scheduleAt(inet::simTime() + manager_->getChSummaryIntervalSec(), chSummaryTimer_);
                return;
            case TIMER_C2_BCAST:
                sendC2Broadcast();
                scheduleAt(inet::simTime() + manager_->getC2BroadcastIntervalSec(), c2Timer_);
                return;
            case TIMER_WATCHDOG:
                runWatchdog();
                scheduleAt(inet::simTime() + 0.50, watchdogTimer_);
                return;
            case TIMER_RELAY_FWD: {
                auto* rf = omnetpp::check_and_cast<RelayForwardMsg*>(msg);
                pendingRelayTimers_.erase(rf->dedupKey);
                const int seenCount = relayOverhearCount_[rf->dedupKey];
                const int suppressThresh = (manager_->getProtoKind() == ProtoKind::FLAT_SBP)
                    ? 1
                    : relaySuppressionThreshold_;
                if (seenCount < suppressThresh) {
                    rf->hdr.prevHopNodeId = static_cast<int16_t>(slotIdx_);
                    if (rf->rawBytes.size() >= sizeof(V2WireHeader)) {
                        std::memcpy(rf->rawBytes.data(), &rf->hdr, sizeof(V2WireHeader));
                        const MsgKind mk = static_cast<MsgKind>(rf->hdr.msgKind);
                        sendRawPacket(
                            "RelayFwd",
                            mk,
                            rf->rawBytes.data(),
                            static_cast<int>(rf->rawBytes.size()),
                            inet::L3Address(inet::Ipv4Address::ALLONES_ADDRESS)
                        );
                    }
                } else {
                    manager_->recordRelaySuppressed();
                }
                delete rf;
                return;
            }
            case TIMER_ACK_TX: {
                auto* ack = omnetpp::check_and_cast<AckDelayMsg*>(msg);
                transientTimers_.erase(std::remove(transientTimers_.begin(), transientTimers_.end(), ack), transientTimers_.end());
                V2WireHeader hdr{};
                hdr.msgKind = static_cast<uint8_t>(MsgKind::REKEY_UNICAST_ACK);
                hdr.protoKind = static_cast<uint8_t>(manager_->getProtoKind());
                hdr.hopStage = 2;
                hdr.ttl = 0;
                hdr.seqId = nextSeq();
                hdr.roundId = ack->roundId;
                hdr.keyEpoch = ack->keyEpoch;
                hdr.originNodeId = static_cast<int16_t>(slotIdx_);
                hdr.prevHopNodeId = static_cast<int16_t>(slotIdx_);
                hdr.targetClusterId = static_cast<int16_t>(clusterId_);
                hdr.targetNodeId = static_cast<int16_t>(ack->destSlotIdx);
                hdr.fragIdx = 0;
                hdr.numFrags = 1;
                hdr.payloadBytes = sizeof(V2WireHeader);
                hdr.txTimestampUs = inet::simTime().inUnit(inet::SIMTIME_US);
                const auto destAddr = manager_->getSlotAddress(ack->destSlotIdx);
                if (!destAddr.isUnspecified()) {
                    sendRawPacket("RekeyAck", MsgKind::REKEY_UNICAST_ACK, &hdr, sizeof(hdr), destAddr);
                }
                delete ack;
                return;
            }
            case TIMER_CRYPTO_DONE: {
                auto* rt = omnetpp::check_and_cast<RoundTimerMsg*>(msg);
                const uint32_t rid = rt->roundId;
                transientTimers_.erase(std::remove(transientTimers_.begin(), transientTimers_.end(), rt), transientTimers_.end());
                delete rt;
                launchRekeyAfterCryptoDelay(rid);
                return;
            }
            case TIMER_REKEY_DEADLINE: {
                auto* rt = omnetpp::check_and_cast<RoundTimerMsg*>(msg);
                const uint32_t rid = rt->roundId;
                transientTimers_.erase(std::remove(transientTimers_.begin(), transientTimers_.end(), rt), transientTimers_.end());
                if (auto it = pendingRounds_.find(rid); it != pendingRounds_.end()) {
                    it->second.deadlineMsg = nullptr;
                }
                delete rt;
                finalizeRekeyRound(rid);
                return;
            }
            default:
                break;
        }
    }

    socket_.processMessage(msg);
}

void DTHSBPNodeApp::sendRawPacket(
    const char* name,
    MsgKind kind,
    const void* data,
    int lenBytes,
    const inet::L3Address& destAddr
) {
    if (!active_ || lenBytes <= 0 || destAddr.isUnspecified()) {
        return;
    }
    ensureWlanInterfaceId();
    auto* pkt = new inet::Packet(name);
    const auto* ptr = static_cast<const uint8_t*>(data);
    std::vector<uint8_t> buf(ptr, ptr + lenBytes);
    auto payload = inet::makeShared<inet::BytesChunk>(buf);
    pkt->insertAtBack(payload);

    pkt->addTagIfAbsent<inet::UserPriorityReq>()->setUserPriority(userPriorityForMsg(kind));
    if (wlanInterfaceId_ != -1) {
        pkt->addTagIfAbsent<inet::InterfaceReq>()->setInterfaceId(wlanInterfaceId_);
    }

    emit(inet::packetSentSignal, pkt);
    const bool isAck = (kind == MsgKind::REKEY_UNICAST_ACK);
    const bool isResync = (kind == MsgKind::KEY_RESYNC_PULL);
    manager_->recordPacketTx(kind, lenBytes, isAck, isResync);
    socket_.sendTo(pkt, destAddr, localPort_);
}

void DTHSBPNodeApp::sendDtReport() {
    if (!active_ || !mobility_ || !manager_) {
        return;
    }
    const auto pos = mobility_->getPositionNow();
    const auto vel = mobility_->getVelocityNow();
    const double energyPct = mobility_->getEnergyPct();

    HeartbeatDtPacket pkt{};
    pkt.hdr.msgKind = static_cast<uint8_t>(MsgKind::HEARTBEAT_DT_REPORT);
    pkt.hdr.protoKind = static_cast<uint8_t>(manager_->getProtoKind());
    pkt.hdr.hopStage = 4;
    pkt.hdr.ttl = 0;
    pkt.hdr.seqId = nextSeq();
    pkt.hdr.keyEpoch = clusterKeyEpoch_;
    pkt.hdr.originNodeId = static_cast<int16_t>(slotIdx_);
    pkt.hdr.prevHopNodeId = static_cast<int16_t>(slotIdx_);
    pkt.hdr.targetClusterId = static_cast<int16_t>(clusterId_);
    pkt.hdr.targetNodeId = static_cast<int16_t>(chSlotIdx_);
    pkt.hdr.fragIdx = 0;
    pkt.hdr.numFrags = 1;
    pkt.hdr.payloadBytes = sizeof(HeartbeatDtPacket);
    pkt.hdr.flags = needsJoinAdmission_ ? kFlagJoinRequest : 0;
    pkt.hdr.txTimestampUs = inet::simTime().inUnit(inet::SIMTIME_US);
    pkt.hdr.authEpochTag = static_cast<uint32_t>(swarmKeyEpoch_);

    pkt.posX_mm = static_cast<int32_t>(std::llround(pos.x * 1000.0));
    pkt.posY_mm = static_cast<int32_t>(std::llround(pos.y * 1000.0));
    pkt.velX_cms = static_cast<int16_t>(std::llround(vel.x * 100.0));
    pkt.velY_cms = static_cast<int16_t>(std::llround(vel.y * 100.0));
    pkt.energy_bp = static_cast<uint16_t>(std::clamp(std::llround(energyPct * 100.0), 0LL, 10000LL));
    pkt.linkQ_bp = static_cast<uint16_t>(std::clamp(std::llround(simulatedLinkQ_ * 10000.0), 0LL, 10000LL));

    const auto destAddr = manager_->getSlotAddress(chSlotIdx_);
    sendRawPacket("DtReport", MsgKind::HEARTBEAT_DT_REPORT, &pkt, sizeof(pkt), destAddr);
}

void DTHSBPNodeApp::sendChSummary() {
    if (!active_ || role_ != NodeRole::CLUSTER_HEAD || slSlotIdx_ == slotIdx_) {
        return;
    }
    ChSummaryPacket pkt{};
    pkt.hdr.msgKind = static_cast<uint8_t>(MsgKind::CH_SUMMARY_TO_SL);
    pkt.hdr.protoKind = static_cast<uint8_t>(manager_->getProtoKind());
    pkt.hdr.hopStage = 5;
    pkt.hdr.ttl = 0;
    pkt.hdr.seqId = nextSeq();
    pkt.hdr.keyEpoch = clusterKeyEpoch_;
    pkt.hdr.originNodeId = static_cast<int16_t>(slotIdx_);
    pkt.hdr.prevHopNodeId = static_cast<int16_t>(slotIdx_);
    pkt.hdr.targetClusterId = static_cast<int16_t>(clusterId_);
    pkt.hdr.targetNodeId = static_cast<int16_t>(slSlotIdx_);
    pkt.hdr.fragIdx = 0;
    pkt.hdr.numFrags = 1;
    pkt.hdr.payloadBytes = sizeof(ChSummaryPacket);
    pkt.hdr.txTimestampUs = inet::simTime().inUnit(inet::SIMTIME_US);
    pkt.hdr.authEpochTag = static_cast<uint32_t>(swarmKeyEpoch_);

    double maxDt = 0.0;
    double sumEnergy = 0.0;
    int synced = 0;
    int nMembers = 0;
    for (int id : clusterMembers_) {
        if (const auto* st = dtTracker_.getState(id)) {
            maxDt = std::max(maxDt, st->dtScore);
            sumEnergy += st->energyPct;
            if (st->reportedEpoch == clusterKeyEpoch_) {
                ++synced;
            }
            ++nMembers;
        }
    }
    pkt.clusterId = static_cast<uint16_t>(std::max(0, clusterId_));
    pkt.memberCount = static_cast<uint16_t>(clusterMembers_.size());
    pkt.healthyCount = static_cast<uint16_t>(nMembers);
    pkt.syncedCount = static_cast<uint16_t>(synced);
    pkt.maxDtScore = static_cast<float>(maxDt);
    pkt.meanEnergyPct = (nMembers > 0) ? static_cast<float>(sumEnergy / nMembers) : 100.0f;

    const auto destAddr = manager_->getSlotAddress(slSlotIdx_);
    sendRawPacket("ChSummary", MsgKind::CH_SUMMARY_TO_SL, &pkt, sizeof(pkt), destAddr);
}

void DTHSBPNodeApp::sendChEscalationOrEpochAck(
    int destSlSlot,
    int targetClusterId,
    int subjectNodeId,
    uint64_t epochVal,
    bool isEscalation,
    bool isJoinAdmission
) {
    if (!active_ || destSlSlot < 0 || destSlSlot == slotIdx_) {
        return;
    }
    V2WireHeader hdr{};
    hdr.msgKind = static_cast<uint8_t>(MsgKind::REKEY_UNICAST_ACK);
    hdr.protoKind = static_cast<uint8_t>(manager_->getProtoKind());
    hdr.hopStage = 2;
    hdr.ttl = 0;
    hdr.seqId = nextSeq();
    hdr.roundId = 0;
    hdr.keyEpoch = epochVal;
    hdr.originNodeId = static_cast<int16_t>(slotIdx_);
    hdr.prevHopNodeId = static_cast<int16_t>(slotIdx_);
    hdr.targetClusterId = static_cast<int16_t>(targetClusterId);
    hdr.targetNodeId = static_cast<int16_t>(destSlSlot);
    hdr.fragIdx = 0;
    hdr.numFrags = 1;
    hdr.payloadBytes = sizeof(V2WireHeader);
    hdr.flags = (isEscalation ? kFlagEscalate : 0) | (isJoinAdmission ? kFlagJoinAdmission : 0);
    hdr.txTimestampUs = inet::simTime().inUnit(inet::SIMTIME_US);
    hdr.authEpochTag = static_cast<uint32_t>(subjectNodeId >= 0 ? subjectNodeId : 0xFFFFU);

    const auto destAddr = manager_->getSlotAddress(destSlSlot);
    sendRawPacket(
        isEscalation ? "ChEscalateToSl" : "ChEpochAckToSl",
        MsgKind::REKEY_UNICAST_ACK,
        &hdr,
        sizeof(hdr),
        destAddr
    );
}

void DTHSBPNodeApp::sendC2Broadcast() {
    if (!active_ || role_ != NodeRole::SWARM_LEADER) {
        return;
    }
    const double now = inet::simTime().dbl();
    const uint32_t seq = nextSeq();

    C2BroadcastPacket pkt{};
    pkt.hdr.msgKind = static_cast<uint8_t>(MsgKind::MISSION_C2_BCAST);
    pkt.hdr.protoKind = static_cast<uint8_t>(manager_->getProtoKind());
    pkt.hdr.hopStage = 3;
    pkt.hdr.ttl = (manager_->getProtoKind() == ProtoKind::FLAT_SBP) ? 2 : 1;
    pkt.hdr.seqId = seq;
    pkt.hdr.roundId = seq;
    pkt.hdr.keyEpoch = swarmKeyEpoch_;
    pkt.hdr.originNodeId = static_cast<int16_t>(slotIdx_);
    pkt.hdr.prevHopNodeId = static_cast<int16_t>(slotIdx_);
    pkt.hdr.targetClusterId = -1;
    pkt.hdr.targetNodeId = -1;
    pkt.hdr.fragIdx = 0;
    pkt.hdr.numFrags = 1;
    pkt.hdr.payloadBytes = sizeof(C2BroadcastPacket);
    pkt.hdr.txTimestampUs = inet::simTime().inUnit(inet::SIMTIME_US);
    pkt.hdr.authEpochTag = static_cast<uint32_t>(swarmKeyEpoch_);
    pkt.requiredKeyEpoch = swarmKeyEpoch_;
    pkt.c2Seq = seq;
    for (int c = 0; c < 5; ++c) {
        pkt.clusterEpochs[c] = slClusterEpochs_[c];
    }

    seenCache_.insertIfNew(slotIdx_, seq, 0);
    manager_->recordC2Sent(swarmKeyEpoch_, now);
    sendRawPacket("MissionC2", MsgKind::MISSION_C2_BCAST, &pkt, sizeof(pkt),
                  inet::L3Address(inet::Ipv4Address::ALLONES_ADDRESS));
}

void DTHSBPNodeApp::runWatchdog() {
    if (!active_) {
        return;
    }
    const double now = inet::simTime().dbl();

    // 1. Detect silent node crashes via missed 1 Hz Heartbeat+DT report timeout
    if (role_ == NodeRole::CLUSTER_HEAD || role_ == NodeRole::SWARM_LEADER) {
        auto stale = dtTracker_.staleNodes(now, manager_->getMissedHeartbeatTimeoutSec());
        if (!stale.empty()) {
            for (int id : stale) {
                dtTracker_.removeNode(id);
                clusterMembers_.erase(id);
                excludedSlots_.insert(id);
            }
            const ProtoKind proto = manager_->getProtoKind();
            if ((proto == ProtoKind::FLAT_SBP || proto == ProtoKind::LKH || proto == ProtoKind::TGDH)
                && role_ == NodeRole::CLUSTER_HEAD && slotIdx_ != slSlotIdx_) {
                sendChEscalationOrEpochAck(slSlotIdx_, clusterId_, stale.front(), clusterKeyEpoch_, true, false);
            } else {
                triggerRekeyForTarget(clusterId_, stale, false, stale.size() > 1, false, false);
            }
        }
    }

    // 2. Backup SL failover detection at CH 0 when no C2 heard for >= 1.6s after warmup
    if (role_ == NodeRole::CLUSTER_HEAD && clusterId_ == 0 && !slFailoverTriggered_
        && now > 5.0 && (now - lastC2OrSlHeardSec_) >= 1.6) {
        slFailoverTriggered_ = true;
        role_ = NodeRole::SWARM_LEADER;
        slSlotIdx_ = slotIdx_;
        manager_->recordSlFailoverCompleted();
        triggerRekeyForTarget(-1, {0}, false, false, true, false);
        scheduleAt(now + 0.05, c2Timer_);
    }
}

void DTHSBPNodeApp::triggerRekeyForTarget(
    int clusterScopeId,
    const std::vector<int>& excludedIds,
    bool isDtProactive,
    bool isCollateralBatch,
    bool isSlPromotion,
    bool isJoinAdmission
) {
    if (!active_ || excludedIds.empty()) {
        return;
    }
    const ProtoKind proto = manager_->getProtoKind();
    const int nSwarm = std::max(2, manager_->getActiveSwarmCountEstimate());
    const int nClust = std::max(2, nSwarm / std::max(1, manager_->getNumClusters()));

    int suffixSurvivors = 0;
    bool isTail = false;
    if (proto == ProtoKind::DTHSBP) {
        std::vector<ChainMember> members;
        members.reserve(clusterMembers_.size() + excludedIds.size());
        for (int m : clusterMembers_) {
            const auto* st = dtTracker_.getState(m);
            members.push_back({m, st ? st->energyPct : 90.0, st ? st->dtScore : 0.3});
        }
        for (int e : excludedIds) {
            const auto* st = dtTracker_.getState(e);
            members.push_back({e, st ? st->energyPct : 40.0, st ? std::max(3.0, st->dtScore) : 3.5});
        }
        auto sorted = LifespanChainOrder::sortDescending(members);
        isTail = LifespanChainOrder::isContiguousTail(sorted, excludedIds, suffixSurvivors);
    }

    const int payloadBytes = computeRekeyPayloadBytes(
        proto, nSwarm, nClust, static_cast<int>(excludedIds.size()), suffixSurvivors, isTail
    );
    const double cryptoDelaySec = computeCryptoLeaderDelaySec(
        proto, nSwarm, nClust, suffixSurvivors, isDtProactive
    );

    const uint32_t rid = ++localRoundCounter_;
    PendingRekeyRound round;
    round.roundId = rid;
    round.isDtProactive = isDtProactive;
    round.isCollateralBatch = isCollateralBatch;
    round.isSlPromotion = isSlPromotion;
    round.isJoinAdmission = isJoinAdmission;
    round.targetClusterId = (proto == ProtoKind::FLAT_SBP || isSlPromotion) ? -1 : clusterScopeId;
    round.payloadBytes = payloadBytes;

    if (round.targetClusterId == -1) {
        swarmKeyEpoch_ += 1;
        clusterKeyEpoch_ = std::max(clusterKeyEpoch_, swarmKeyEpoch_);
        round.targetEpoch = swarmKeyEpoch_;
        round.expectedResponders = std::max(1, nSwarm - (isJoinAdmission ? 0 : static_cast<int>(excludedIds.size())));
    } else if (role_ == NodeRole::SWARM_LEADER && round.targetClusterId != clusterId_) {
        // Centralized LKH/TGDH SL initiating distribution for another cluster
        slClusterEpochs_[round.targetClusterId % 5] += 1;
        round.targetEpoch = slClusterEpochs_[round.targetClusterId % 5];
        round.expectedResponders = std::max(1, nClust);
    } else {
        clusterKeyEpoch_ += 1;
        if (round.targetClusterId >= 0 && round.targetClusterId < 5) {
            slClusterEpochs_[round.targetClusterId] = static_cast<uint32_t>(
                std::max<uint64_t>(slClusterEpochs_[round.targetClusterId], clusterKeyEpoch_)
            );
        }
        round.targetEpoch = clusterKeyEpoch_;
        round.expectedResponders = std::max(1, static_cast<int>(clusterMembers_.size()));
    }

    round.startTimeSec = inet::simTime().dbl();
    round.cryptoDelayMs = cryptoDelaySec * 1000.0;
    if (!isJoinAdmission) {
        round.excludedSlots = excludedIds;
    }
    pendingRounds_[rid] = round;

    if (cryptoDelaySec <= 1e-6) {
        launchRekeyAfterCryptoDelay(rid);
    } else {
        auto* tm = new RoundTimerMsg("CryptoDelayDone", TIMER_CRYPTO_DONE, rid);
        transientTimers_.push_back(tm);
        scheduleAt(inet::simTime() + cryptoDelaySec, tm);
    }
}

void DTHSBPNodeApp::launchRekeyAfterCryptoDelay(uint32_t roundId) {
    auto it = pendingRounds_.find(roundId);
    if (it == pendingRounds_.end() || !active_) {
        return;
    }
    auto& rnd = it->second;
    const double now = inet::simTime().dbl();
    for (int ex : rnd.excludedSlots) {
        manager_->recordNodeExcludedByProtocol(ex, now);
    }

    uint16_t flags = 0;
    if (rnd.isDtProactive) flags |= kFlagDtPrecomputed | kFlagTailTruncated;
    if (rnd.isCollateralBatch) flags |= kFlagCollateral;
    if (rnd.isSlPromotion) flags |= kFlagSlPromotion;
    if (rnd.isJoinAdmission) flags |= kFlagJoinAdmission;

    const int targetExcluded = rnd.excludedSlots.empty() ? -1 : rnd.excludedSlots.front();
    const bool isSlRemoteCluster = (role_ == NodeRole::SWARM_LEADER && rnd.targetClusterId >= 0 && rnd.targetClusterId != clusterId_);
    const uint8_t hopStage = isSlRemoteCluster ? 0 : 1;
    const uint8_t ttl = (rnd.targetClusterId == -1) ? 2 : 0;

    dispatchRekeyBroadcast(
        rnd.roundId,
        rnd.targetEpoch,
        rnd.targetClusterId,
        targetExcluded,
        rnd.payloadBytes,
        flags,
        hopStage,
        ttl
    );

    // Notify SL in-band whenever CH updates its local cluster key epoch
    if (role_ == NodeRole::CLUSTER_HEAD && slotIdx_ != slSlotIdx_ && rnd.targetClusterId >= 0) {
        sendChEscalationOrEpochAck(slSlotIdx_, rnd.targetClusterId, -1, rnd.targetEpoch, false, rnd.isJoinAdmission);
    }

    auto* dl = new RoundTimerMsg("RekeyDeadline", TIMER_REKEY_DEADLINE, roundId);
    rnd.deadlineMsg = dl;
    transientTimers_.push_back(dl);
    scheduleAt(inet::simTime() + ackDeadlineSec_, dl);
}

void DTHSBPNodeApp::dispatchRekeyBroadcast(
    uint32_t roundId,
    uint64_t targetEpoch,
    int targetClusterId,
    int targetExcludedId,
    int payloadBytes,
    uint16_t flags,
    uint8_t hopStage,
    uint8_t ttl
) {
    const int mtu = std::max(256, manager_->getMtuBytes());
    const int hdrSize = static_cast<int>(sizeof(V2WireHeader));
    const int maxBody = std::max(64, mtu - hdrSize);
    const int totalBody = std::max(1, payloadBytes);
    const int numFrags = std::max(1, (totalBody + maxBody - 1) / maxBody);

    int remaining = totalBody;
    for (int f = 0; f < numFrags; ++f) {
        const int chunkBody = std::min(remaining, maxBody);
        remaining -= chunkBody;
        const int pktBytes = hdrSize + chunkBody;

        std::vector<uint8_t> raw(static_cast<size_t>(pktBytes), 0);
        V2WireHeader hdr{};
        hdr.msgKind = static_cast<uint8_t>(MsgKind::REKEY_BCAST_FRAG);
        hdr.protoKind = static_cast<uint8_t>(manager_->getProtoKind());
        hdr.hopStage = hopStage;
        hdr.ttl = ttl;
        hdr.seqId = nextSeq();
        hdr.roundId = roundId;
        hdr.keyEpoch = targetEpoch;
        hdr.originNodeId = static_cast<int16_t>(slotIdx_);
        hdr.prevHopNodeId = static_cast<int16_t>(slotIdx_);
        hdr.targetClusterId = static_cast<int16_t>(targetClusterId);
        hdr.targetNodeId = static_cast<int16_t>(targetExcludedId);
        hdr.fragIdx = static_cast<int16_t>(f);
        hdr.numFrags = static_cast<int16_t>(numFrags);
        hdr.payloadBytes = static_cast<uint16_t>(std::min(65535, totalBody));
        hdr.flags = flags;
        hdr.txTimestampUs = inet::simTime().inUnit(inet::SIMTIME_US);
        hdr.authEpochTag = static_cast<uint32_t>(swarmKeyEpoch_);

        std::memcpy(raw.data(), &hdr, sizeof(V2WireHeader));
        seenCache_.insertIfNew(slotIdx_, hdr.seqId, hdr.fragIdx);

        std::ostringstream nm;
        nm << "RekeyR" << roundId << "_F" << f;
        sendRawPacket(
            nm.str().c_str(),
            MsgKind::REKEY_BCAST_FRAG,
            raw.data(),
            pktBytes,
            inet::L3Address(inet::Ipv4Address::ALLONES_ADDRESS)
        );
    }
}

void DTHSBPNodeApp::finalizeRekeyRound(uint32_t roundId) {
    auto it = pendingRounds_.find(roundId);
    if (it == pendingRounds_.end() || it->second.finalized) {
        return;
    }
    auto& rnd = it->second;
    rnd.finalized = true;
    if (rnd.deadlineMsg) {
        cancelEvent(rnd.deadlineMsg);
        transientTimers_.erase(
            std::remove(transientTimers_.begin(), transientTimers_.end(), rnd.deadlineMsg),
            transientTimers_.end()
        );
        delete rnd.deadlineMsg;
        rnd.deadlineMsg = nullptr;
    }

    const double completionMs = (inet::simTime().dbl() - rnd.startTimeSec) * 1000.0;
    const double pdr = std::clamp(
        static_cast<double>(rnd.respondersReceived.size()) / static_cast<double>(std::max(1, rnd.expectedResponders)),
        0.0,
        1.0
    );
    manager_->recordRekeyRound(
        rnd.isDtProactive,
        rnd.excludedSlots.empty() ? -1 : rnd.excludedSlots.front(),
        static_cast<int>(rnd.excludedSlots.size()),
        rnd.isCollateralBatch,
        rnd.cryptoDelayMs,
        completionMs,
        pdr
    );
    pendingRounds_.erase(it);
}

void DTHSBPNodeApp::socketDataArrived(inet::UdpSocket* /*socket*/, inet::Packet* packet) {
    if (!active_) {
        delete packet;
        return;
    }
    emit(inet::packetReceivedSignal, packet);

    const auto chunk = packet->peekDataAsBytes();
    const auto& raw = chunk->getBytes();
    if (raw.size() < sizeof(V2WireHeader)) {
        delete packet;
        return;
    }

    V2WireHeader hdr{};
    std::memcpy(&hdr, raw.data(), sizeof(V2WireHeader));
    const MsgKind mk = static_cast<MsgKind>(hdr.msgKind);

    switch (mk) {
        case MsgKind::HEARTBEAT_DT_REPORT:
            if (raw.size() >= sizeof(HeartbeatDtPacket)) {
                HeartbeatDtPacket pkt{};
                std::memcpy(&pkt, raw.data(), sizeof(HeartbeatDtPacket));
                handleHeartbeatDtReport(pkt);
            }
            break;
        case MsgKind::CH_SUMMARY_TO_SL:
            if (raw.size() >= sizeof(ChSummaryPacket)) {
                ChSummaryPacket pkt{};
                std::memcpy(&pkt, raw.data(), sizeof(ChSummaryPacket));
                handleChSummaryToSl(pkt);
            }
            break;
        case MsgKind::MISSION_C2_BCAST:
            if (raw.size() >= sizeof(C2BroadcastPacket)) {
                C2BroadcastPacket pkt{};
                std::memcpy(&pkt, raw.data(), sizeof(C2BroadcastPacket));
                handleMissionC2(pkt, raw);
            }
            break;
        case MsgKind::REKEY_BCAST_FRAG:
            handleRekeyBroadcast(hdr, raw);
            break;
        case MsgKind::REKEY_UNICAST_ACK:
            handleRekeyAck(hdr);
            break;
        case MsgKind::KEY_RESYNC_PULL:
            if (raw.size() >= sizeof(KeyResyncPacket)) {
                KeyResyncPacket pkt{};
                std::memcpy(&pkt, raw.data(), sizeof(KeyResyncPacket));
                handleKeyResyncPull(pkt);
            }
            break;
        default:
            break;
    }

    delete packet;
}

void DTHSBPNodeApp::handleHeartbeatDtReport(const HeartbeatDtPacket& pkt) {
    if (role_ != NodeRole::CLUSTER_HEAD && role_ != NodeRole::SWARM_LEADER) {
        return;
    }
    const int sender = pkt.hdr.originNodeId;
    if (sender < 0 || sender == slotIdx_) {
        return;
    }
    const double now = inet::simTime().dbl();
    const double posX = static_cast<double>(pkt.posX_mm) / 1000.0;
    const double posY = static_cast<double>(pkt.posY_mm) / 1000.0;
    const double velX = static_cast<double>(pkt.velX_cms) / 100.0;
    const double velY = static_cast<double>(pkt.velY_cms) / 100.0;
    const double energyPct = static_cast<double>(pkt.energy_bp) / 100.0;
    const double linkQ = static_cast<double>(pkt.linkQ_bp) / 10000.0;

    const ProtoKind proto = manager_->getProtoKind();
    const bool isJoinReq = (pkt.hdr.flags & kFlagJoinRequest) != 0;
    if (!isJoinReq && excludedSlots_.count(sender) > 0) {
        return; // Ignore lagging reports from already-excluded departing/disturbed UAVs
    }
    if (isJoinReq) {
        excludedSlots_.erase(sender);
    }

    const bool isNewMember = (clusterMembers_.count(sender) == 0);
    const double score = dtTracker_.ingestReport(sender, now, posX, posY, velX, velY, energyPct, linkQ, pkt.hdr.keyEpoch);

    if (isNewMember) {
        clusterMembers_.insert(sender);
        if (isJoinReq) {
            if ((proto == ProtoKind::FLAT_SBP || proto == ProtoKind::LKH || proto == ProtoKind::TGDH)
                && role_ == NodeRole::CLUSTER_HEAD && slotIdx_ != slSlotIdx_) {
                sendChEscalationOrEpochAck(slSlotIdx_, clusterId_, sender, clusterKeyEpoch_, true, true);
            } else {
                triggerRekeyForTarget(clusterId_, {sender}, false, false, false, true);
            }
            return;
        }
    }

    if (proto == ProtoKind::DTHSBP && score > manager_->getThetaLeave()) {
        std::vector<int> batch = {sender};
        dtTracker_.removeNode(sender);
        clusterMembers_.erase(sender);
        excludedSlots_.insert(sender);

        const double lookaheadThresh = 0.55 * manager_->getThetaLeave();
        std::vector<int> extra;
        for (int m : clusterMembers_) {
            if (const auto* st = dtTracker_.getState(m)) {
                if (st->dtScore >= lookaheadThresh) {
                    extra.push_back(m);
                }
            }
        }
        for (int e : extra) {
            batch.push_back(e);
            dtTracker_.removeNode(e);
            clusterMembers_.erase(e);
            excludedSlots_.insert(e);
        }
        triggerRekeyForTarget(clusterId_, batch, true, batch.size() > 1, false, false);
    }
}

void DTHSBPNodeApp::handleChSummaryToSl(const ChSummaryPacket& pkt) {
    if (role_ == NodeRole::SWARM_LEADER) {
        activeClusters_.insert(pkt.clusterId);
    }
}

void DTHSBPNodeApp::handleMissionC2(const C2BroadcastPacket& pkt, const std::vector<uint8_t>& rawBytes) {
    if (pkt.hdr.originNodeId == slotIdx_) {
        return;
    }
    lastC2OrSlHeardSec_ = inet::simTime().dbl();
    if (pkt.hdr.originNodeId != slSlotIdx_) {
        slSlotIdx_ = pkt.hdr.originNodeId;
    }

    const uint64_t dk = makeDedupKey(pkt.hdr.originNodeId, pkt.hdr.seqId, pkt.hdr.fragIdx);
    ++relayOverhearCount_[dk];

    if (!seenCache_.insertIfNew(pkt.hdr.originNodeId, pkt.hdr.seqId, pkt.hdr.fragIdx)) {
        return;
    }

    if (pkt.hdr.ttl > 0 && (manager_->getProtoKind() == ProtoKind::FLAT_SBP || role_ == NodeRole::CLUSTER_HEAD)) {
        scheduleRelayForward(pkt.hdr, rawBytes);
    }

    const ProtoKind proto = manager_->getProtoKind();
    const double latMs = std::max(0.0, static_cast<double>(inet::simTime().inUnit(inet::SIMTIME_US) - pkt.hdr.txTimestampUs) / 1000.0);

    bool decryptedOk = false;
    if (proto == ProtoKind::FLAT_SBP) {
        decryptedOk = (swarmKeyEpoch_ >= pkt.requiredKeyEpoch);
    } else {
        const uint32_t reqClusterEpoch = (clusterId_ >= 0 && clusterId_ < 5) ? pkt.clusterEpochs[clusterId_] : 1U;
        decryptedOk = (swarmKeyEpoch_ >= pkt.requiredKeyEpoch) && (clusterKeyEpoch_ >= reqClusterEpoch);
    }
    manager_->recordC2Delivery(decryptedOk, latMs);

    if (!decryptedOk) {
        const double now = inet::simTime().dbl();
        if (desyncedSinceSec_ < 0.0) {
            desyncedSinceSec_ = now;
        }
        if ((now - lastResyncPullSec_) >= resyncCooldownSec_) {
            lastResyncPullSec_ = now;
            const int targetPullSlot = (proto == ProtoKind::FLAT_SBP || role_ == NodeRole::CLUSTER_HEAD)
                ? slSlotIdx_
                : chSlotIdx_;
            if (targetPullSlot != slotIdx_) {
                KeyResyncPacket pull{};
                pull.hdr.msgKind = static_cast<uint8_t>(MsgKind::KEY_RESYNC_PULL);
                pull.hdr.protoKind = static_cast<uint8_t>(proto);
                pull.hdr.hopStage = 6;
                pull.hdr.ttl = 0;
                pull.hdr.seqId = nextSeq();
                pull.hdr.keyEpoch = swarmKeyEpoch_;
                pull.hdr.originNodeId = static_cast<int16_t>(slotIdx_);
                pull.hdr.prevHopNodeId = static_cast<int16_t>(slotIdx_);
                pull.hdr.targetClusterId = static_cast<int16_t>(clusterId_);
                pull.hdr.targetNodeId = static_cast<int16_t>(targetPullSlot);
                pull.hdr.fragIdx = 0;
                pull.hdr.numFrags = 1;
                pull.hdr.payloadBytes = sizeof(KeyResyncPacket);
                pull.hdr.txTimestampUs = inet::simTime().inUnit(inet::SIMTIME_US);
                pull.currentEpoch = clusterKeyEpoch_;
                sendRawPacket("KeyResyncPull", MsgKind::KEY_RESYNC_PULL, &pull, sizeof(pull),
                              manager_->getSlotAddress(targetPullSlot));
            }
        }
    }
}

void DTHSBPNodeApp::handleRekeyBroadcast(const V2WireHeader& hdr, const std::vector<uint8_t>& rawBytes) {
    if (hdr.originNodeId == slotIdx_) {
        return;
    }
    if ((hdr.flags & kFlagSlPromotion) != 0) {
        lastC2OrSlHeardSec_ = inet::simTime().dbl();
        slSlotIdx_ = hdr.originNodeId;
        slFailoverTriggered_ = true;
    }

    const uint64_t dk = makeDedupKey(hdr.originNodeId, hdr.seqId, hdr.fragIdx);
    ++relayOverhearCount_[dk];

    if (!seenCache_.insertIfNew(hdr.originNodeId, hdr.seqId, hdr.fragIdx)) {
        return;
    }

    if (hdr.ttl > 0 && hdr.targetClusterId == -1) {
        scheduleRelayForward(hdr, rawBytes);
    }

    if (hdr.targetClusterId != -1 && hdr.targetClusterId != clusterId_) {
        return;
    }

    auto& asmState = rxFragAssemblies_[hdr.roundId];
    asmState.numFrags = std::max<int>(1, hdr.numFrags);
    asmState.fragsSeen.insert(hdr.fragIdx);

    if (static_cast<int>(asmState.fragsSeen.size()) < asmState.numFrags) {
        return;
    }
    rxFragAssemblies_.erase(hdr.roundId);

    // Apply key update unless excluded (or join admission includes everyone)
    const bool isExcluded = (hdr.targetNodeId == slotIdx_) && ((hdr.flags & kFlagJoinAdmission) == 0);
    if (!isExcluded) {
        if (hdr.authEpochTag > swarmKeyEpoch_) {
            swarmKeyEpoch_ = hdr.authEpochTag;
        }
        if (hdr.targetClusterId == -1) {
            swarmKeyEpoch_ = std::max(swarmKeyEpoch_, hdr.keyEpoch);
            clusterKeyEpoch_ = std::max(clusterKeyEpoch_, hdr.keyEpoch);
        } else {
            clusterKeyEpoch_ = std::max(clusterKeyEpoch_, hdr.keyEpoch);
        }
        if (clusterKeyEpoch_ > 0) {
            needsJoinAdmission_ = false;
        }
        if (desyncedSinceSec_ >= 0.0) {
            manager_->recordDesyncDurationMs((inet::simTime().dbl() - desyncedSinceSec_) * 1000.0);
            desyncedSinceSec_ = -1.0;
        }
    }

    // LKH / TGDH SL->CH (hopStage == 0) forwards downstream to local cluster (hopStage == 1)
    if (hdr.hopStage == 0 && role_ == NodeRole::CLUSTER_HEAD && hdr.targetClusterId == clusterId_) {
        scheduleAckReply(hdr.originNodeId, hdr.roundId, clusterKeyEpoch_);
        dispatchRekeyBroadcast(
            hdr.roundId,
            clusterKeyEpoch_,
            clusterId_,
            hdr.targetNodeId,
            static_cast<int>(rawBytes.size() - sizeof(V2WireHeader)),
            hdr.flags,
            1,
            0
        );
        return;
    }

    scheduleAckReply(hdr.originNodeId, hdr.roundId, (hdr.targetClusterId == -1) ? swarmKeyEpoch_ : clusterKeyEpoch_);
}

void DTHSBPNodeApp::handleRekeyAck(const V2WireHeader& hdr) {
    if (hdr.targetNodeId != slotIdx_) {
        return;
    }

    // CH -> SL escalation for centralized protocols (FlatSBP / LKH / TGDH)
    if ((hdr.flags & kFlagEscalate) != 0) {
        if (role_ == NodeRole::SWARM_LEADER) {
            const int subj = (hdr.authEpochTag == 0xFFFFU) ? -1 : static_cast<int>(hdr.authEpochTag);
            const bool isJoin = ((hdr.flags & kFlagJoinAdmission) != 0);
            if (subj >= 0) {
                triggerRekeyForTarget(hdr.targetClusterId, {subj}, false, false, false, isJoin);
            }
        }
        return;
    }

    // Update SL's per-cluster epoch vector from in-band CH epoch ACKs
    if (role_ == NodeRole::SWARM_LEADER && hdr.targetClusterId >= 0 && hdr.targetClusterId < 5) {
        slClusterEpochs_[hdr.targetClusterId] = static_cast<uint32_t>(
            std::max<uint64_t>(slClusterEpochs_[hdr.targetClusterId], hdr.keyEpoch)
        );
    }

    if (hdr.roundId == 0) {
        return;
    }
    auto it = pendingRounds_.find(hdr.roundId);
    if (it == pendingRounds_.end() || it->second.finalized) {
        return;
    }
    it->second.respondersReceived.insert(hdr.originNodeId);
    const int quorum = std::max(1, static_cast<int>(std::ceil(0.80 * static_cast<double>(it->second.expectedResponders))));
    if (static_cast<int>(it->second.respondersReceived.size()) >= quorum) {
        finalizeRekeyRound(hdr.roundId);
    }
}

void DTHSBPNodeApp::handleKeyResyncPull(const KeyResyncPacket& pkt) {
    if ((pkt.hdr.flags & kFlagResyncReply) != 0) {
        if (pkt.hdr.targetNodeId == slotIdx_) {
            swarmKeyEpoch_ = std::max(swarmKeyEpoch_, pkt.hdr.keyEpoch);
            clusterKeyEpoch_ = std::max(clusterKeyEpoch_, pkt.currentEpoch);
            if (clusterKeyEpoch_ > 0) {
                needsJoinAdmission_ = false;
            }
            if (desyncedSinceSec_ >= 0.0) {
                manager_->recordDesyncDurationMs((inet::simTime().dbl() - desyncedSinceSec_) * 1000.0);
                desyncedSinceSec_ = -1.0;
            }
        }
        return;
    }
    if (role_ == NodeRole::CLUSTER_HEAD || role_ == NodeRole::SWARM_LEADER) {
        KeyResyncPacket rep{};
        rep.hdr.msgKind = static_cast<uint8_t>(MsgKind::KEY_RESYNC_PULL);
        rep.hdr.protoKind = static_cast<uint8_t>(manager_->getProtoKind());
        rep.hdr.hopStage = 6;
        rep.hdr.ttl = 0;
        rep.hdr.seqId = nextSeq();
        rep.hdr.keyEpoch = swarmKeyEpoch_;
        rep.hdr.originNodeId = static_cast<int16_t>(slotIdx_);
        rep.hdr.prevHopNodeId = static_cast<int16_t>(slotIdx_);
        rep.hdr.targetClusterId = static_cast<int16_t>(clusterId_);
        rep.hdr.targetNodeId = pkt.hdr.originNodeId;
        rep.hdr.fragIdx = 0;
        rep.hdr.numFrags = 1;
        rep.hdr.payloadBytes = sizeof(KeyResyncPacket);
        rep.hdr.flags = kFlagResyncReply;
        rep.hdr.txTimestampUs = inet::simTime().inUnit(inet::SIMTIME_US);
        uint64_t targetClustEp = clusterKeyEpoch_;
        if (role_ == NodeRole::SWARM_LEADER && pkt.hdr.targetClusterId >= 0 && pkt.hdr.targetClusterId < 5) {
            targetClustEp = slClusterEpochs_[pkt.hdr.targetClusterId];
        }
        rep.currentEpoch = targetClustEp;
        sendRawPacket("KeyResyncReply", MsgKind::KEY_RESYNC_PULL, &rep, sizeof(rep),
                      manager_->getSlotAddress(pkt.hdr.originNodeId));
    }
}

void DTHSBPNodeApp::scheduleAckReply(int destSlotIdx, uint32_t roundId, uint64_t epoch) {
    if (!active_ || destSlotIdx < 0 || destSlotIdx == slotIdx_) {
        return;
    }
    const double maxJitterSec = (manager_->getProtoKind() == ProtoKind::FLAT_SBP) ? 0.014 : 0.006;
    const double delaySec = uniform(0.0004, maxJitterSec);
    auto* msg = new AckDelayMsg("AckReplyDelay", TIMER_ACK_TX);
    msg->destSlotIdx = destSlotIdx;
    msg->roundId = roundId;
    msg->keyEpoch = epoch;
    transientTimers_.push_back(msg);
    scheduleAt(inet::simTime() + delaySec, msg);
}

void DTHSBPNodeApp::scheduleRelayForward(const V2WireHeader& hdr, const std::vector<uint8_t>& rawBytes) {
    if (!active_ || hdr.ttl == 0) {
        return;
    }
    const uint64_t dk = makeDedupKey(hdr.originNodeId, hdr.seqId, hdr.fragIdx);
    if (pendingRelayTimers_.find(dk) != pendingRelayTimers_.end()) {
        return;
    }
    const double jitterMaxSec = std::max(0.0005, manager_->getRelayJitterMaxMs() / 1000.0);
    auto* rf = new RelayForwardMsg("RelayFwdTimer", TIMER_RELAY_FWD);
    rf->dedupKey = dk;
    rf->hdr = hdr;
    rf->hdr.ttl = static_cast<uint8_t>(hdr.ttl - 1);
    rf->rawBytes = rawBytes;
    pendingRelayTimers_[dk] = rf;
    scheduleAt(inet::simTime() + uniform(0.0001, jitterMaxSec), rf);
}

void DTHSBPNodeApp::socketErrorArrived(inet::UdpSocket* /*socket*/, inet::Indication* indication) {
    delete indication;
}

void DTHSBPNodeApp::socketClosed(inet::UdpSocket* /*socket*/) {
}

} // namespace dthsbp_v2
