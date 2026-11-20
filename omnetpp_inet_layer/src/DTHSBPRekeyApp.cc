#include "DTHSBPRekeyApp.h"
#include "TraceEventPlayer.h"

#include "inet/common/ModuleAccess.h"
#include "inet/common/Simsignals.h"
#include "inet/common/TimeTag_m.h"
#include "inet/common/packet/Packet.h"
#include "inet/common/packet/chunk/BytesChunk.h"
#include "inet/linklayer/common/InterfaceTag_m.h"
#include "inet/networklayer/common/L3AddressResolver.h"
#include "inet/networklayer/contract/IInterfaceTable.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <vector>

namespace dthsbp {

Define_Module(DTHSBPRekeyApp);

void DTHSBPRekeyApp::initialize(int stage) {
    ApplicationBase::initialize(stage);
    if (stage == inet::INITSTAGE_LOCAL) {
        localPort_ = par("localPort");
        hostIndex_ = getParentModule()->getIndex();
        assignedUavId_ = -1;
        uavActive_ = false;
        numSent_ = 0;
        numReceived_ = 0;
        player_ = dynamic_cast<TraceEventPlayer*>(getModuleByPath("^.^.tracePlayer"));
    } else if (stage == inet::INITSTAGE_APPLICATION_LAYER) {
        localAddress();
    }
}

void DTHSBPRekeyApp::handleStartOperation(inet::LifecycleOperation* /*operation*/) {
    socket_.setOutputGate(gate("socketOut"));
    socket_.bind(localPort_);
    socket_.setBroadcast(true);
    socket_.setCallback(this);
    localAddress();
}

void DTHSBPRekeyApp::handleStopOperation(inet::LifecycleOperation* /*operation*/) {
    socket_.close();
    delayActiveOperationFinish(par("stopOperationTimeout"));
}

void DTHSBPRekeyApp::handleCrashOperation(inet::LifecycleOperation* /*operation*/) {
    socket_.destroy();
}

void DTHSBPRekeyApp::setAssignedUavId(int uavId, bool active) {
    assignedUavId_ = uavId;
    uavActive_ = active;
}

inet::L3Address DTHSBPRekeyApp::localAddress() {
    if (wlanInterfaceId_ == -1) {
        if (auto* ift = dynamic_cast<inet::IInterfaceTable*>(getParentModule()->getSubmodule("interfaceTable"))) {
            if (auto* ie = ift->findInterfaceByName("wlan0")) {
                wlanInterfaceId_ = ie->getInterfaceId();
            }
        }
    }
    if (cachedLocalAddr_.isUnspecified()) {
        inet::L3AddressResolver().tryResolve(getParentModule()->getFullPath().c_str(), cachedLocalAddr_, inet::L3AddressResolver::ADDR_IPv4);
    }
    return cachedLocalAddr_;
}

void DTHSBPRekeyApp::sendRekeyFragments(
    const inet::L3Address& destAddr,
    int destPort,
    WireRekeyHeader hdrTemplate,
    int payloadBytes,
    int mtuBytes
) {
    Enter_Method("sendRekeyFragments");
    if (payloadBytes <= 0 || mtuBytes <= 0) {
        return;
    }

    const int numFrags = std::max(1, (payloadBytes + mtuBytes - 1) / mtuBytes);
    hdrTemplate.senderHostIdx = hostIndex_;
    hdrTemplate.numFrags = numFrags;
    if (wlanInterfaceId_ == -1) {
        localAddress();
    }
    const bool isBcast = (hdrTemplate.intendedDstUavId == -1);

    int remaining = payloadBytes;
    for (int fIdx = 0; fIdx < numFrags; ++fIdx) {
        const int fragBytes = std::min(remaining, mtuBytes);
        remaining -= fragBytes;
        hdrTemplate.fragIdx = fIdx;

        std::ostringstream pktName;
        pktName << "RekeyEv" << hdrTemplate.eventIdx << "_Tx" << hdrTemplate.txId << "_F" << fIdx;
        auto* packet = new inet::Packet(pktName.str().c_str());

        const size_t chunkLen = std::max<size_t>(sizeof(WireRekeyHeader), static_cast<size_t>(fragBytes));
        std::vector<uint8_t> raw(chunkLen, 0);
        std::memcpy(raw.data(), &hdrTemplate, sizeof(WireRekeyHeader));

        auto chunk = inet::makeShared<inet::BytesChunk>(raw);
        chunk->addTag<inet::CreationTimeTag>()->setCreationTime(omnetpp::simTime());
        packet->insertAtBack(chunk);
        if (isBcast && wlanInterfaceId_ != -1) {
            packet->addTagIfAbsent<inet::InterfaceReq>()->setInterfaceId(wlanInterfaceId_);
        }

        emit(inet::packetSentSignal, packet);
        socket_.sendTo(packet, destAddr, destPort);
        ++numSent_;
    }
}

void DTHSBPRekeyApp::handleMessageWhenUp(omnetpp::cMessage* msg) {
    if (socket_.belongsToSocket(msg)) {
        socket_.processMessage(msg);
    } else {
        delete msg;
    }
}

void DTHSBPRekeyApp::socketDataArrived(inet::UdpSocket* /*socket*/, inet::Packet* packet) {
    emit(inet::packetReceivedSignal, packet);
    ++numReceived_;

    if (packet->getByteLength() >= static_cast<inet::b::value_type>(sizeof(WireRekeyHeader))) {
        auto hdrChunk = packet->peekDataAt<inet::BytesChunk>(
            inet::B(0),
            inet::B(static_cast<int>(sizeof(WireRekeyHeader)))
        );
        WireRekeyHeader hdr;
        hdrChunk->copyToBuffer(reinterpret_cast<uint8_t*>(&hdr), sizeof(WireRekeyHeader));

        // Filter broadcast self-echoes and notifications to inactive slots
        if (uavActive_ && hdr.senderHostIdx != hostIndex_ && player_ != nullptr) {
            player_->onPacketArrived(
                hostIndex_,
                assignedUavId_,
                hdr,
                static_cast<int>(packet->getByteLength())
            );
        }
    }
    delete packet;
}

void DTHSBPRekeyApp::socketErrorArrived(inet::UdpSocket* /*socket*/, inet::Indication* indication) {
    delete indication;
}

void DTHSBPRekeyApp::socketClosed(inet::UdpSocket* /*socket*/) {
    if (operationalState == State::STOPPING_OPERATION) {
        startActiveOperationExtraTimeOrFinish(par("stopOperationExtraTime"));
    }
}

void DTHSBPRekeyApp::finish() {
    recordScalar("fragmentsSent", static_cast<double>(numSent_));
    recordScalar("fragmentsReceived", static_cast<double>(numReceived_));
    ApplicationBase::finish();
}

} // namespace dthsbp
