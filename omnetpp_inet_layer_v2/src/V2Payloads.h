#ifndef DT_HSBP_V2_PAYLOADS_H
#define DT_HSBP_V2_PAYLOADS_H

#include <cstdint>
#include <cstring>

namespace dthsbp_v2 {

enum class MsgKind : uint8_t {
    HEARTBEAT_DT_REPORT = 1, // 64 B, UP=0 (AC_BE)
    CH_SUMMARY_TO_SL    = 2, // 64 B, UP=0 (AC_BE)
    MISSION_C2_BCAST    = 3, // 128 B, UP=4 (AC_VI)
    REKEY_BCAST_FRAG    = 4, // 48 B header + frag bytes, UP=6 (AC_VO)
    REKEY_UNICAST_ACK   = 5, // 48 B, UP=6 (AC_VO)
    KEY_RESYNC_PULL     = 6  // 56 B, UP=6 (AC_VO)
};

enum class ProtoKind : uint8_t {
    FLAT_SBP = 0,
    HSBP     = 1,
    DTHSBP   = 2,
    LKH      = 3,
    TGDH     = 4
};

#pragma pack(push, 1)
struct V2WireHeader {
    uint8_t msgKind;        // MsgKind
    uint8_t protoKind;      // ProtoKind
    uint8_t hopStage;       // 0=SL->CH, 1=CH->cluster/swarm-flood, 2=ACK, 3=C2, 4=HB/DT, 5=Summary, 6=Resync
    uint8_t ttl;            // Hop limit for decentralized flooding
    uint32_t seqId;         // Monotonic origin sequence number (dedup key)
    uint32_t roundId;       // Rekey / C2 round identifier
    uint64_t keyEpoch;      // Target or sender group key epoch
    int16_t originNodeId;   // Original sender UAV index
    int16_t prevHopNodeId;  // Last forwarder index
    int16_t targetClusterId;// -1 for swarm-wide, >=0 for cluster-scoped
    int16_t targetNodeId;   // -1 for broadcast, >=0 for excluded/specific destination
    int16_t fragIdx;        // Fragment index [0..numFrags-1]
    int16_t numFrags;       // Total fragments in this transmission
    uint16_t payloadBytes;  // Total payload bytes of logical message
    uint16_t flags;         // bit0=tailTruncated, bit1=dtPrecomputed, bit2=collateral, bit3=resyncReply
    int64_t txTimestampUs;  // Original send timestamp in microseconds
    uint32_t authEpochTag;  // Padding/auth tag aligning V2WireHeader to 48 bytes
};

struct HeartbeatDtPacket {
    V2WireHeader hdr;
    int32_t posX_mm;        // millimeters
    int32_t posY_mm;        // millimeters
    int16_t velX_cms;       // centimeters/sec
    int16_t velY_cms;       // centimeters/sec
    uint16_t energy_bp;     // basis points (0..10000 => 0%..100%)
    uint16_t linkQ_bp;      // basis points (0..10000 => 0.0..1.0)
};

struct ChSummaryPacket {
    V2WireHeader hdr;
    uint16_t clusterId;
    uint16_t memberCount;
    uint16_t healthyCount;
    uint16_t syncedCount;
    float maxDtScore;
    float meanEnergyPct;
};

struct C2BroadcastPacket {
    V2WireHeader hdr;
    uint64_t requiredKeyEpoch;
    uint64_t c2Seq;
    uint32_t clusterEpochs[5];
    uint8_t encryptedPayload[44];
};

struct KeyResyncPacket {
    V2WireHeader hdr;
    uint64_t currentEpoch;
};
#pragma pack(pop)

static_assert(sizeof(V2WireHeader) == 48, "V2WireHeader must remain 48 bytes");
static_assert(sizeof(HeartbeatDtPacket) == 64, "HeartbeatDtPacket must remain 64 bytes");
static_assert(sizeof(ChSummaryPacket) == 64, "ChSummaryPacket must remain 64 bytes");
static_assert(sizeof(C2BroadcastPacket) == 128, "C2BroadcastPacket must remain 128 bytes");
static_assert(sizeof(KeyResyncPacket) == 56, "KeyResyncPacket must remain 56 bytes");

inline int userPriorityForMsg(MsgKind kind) {
    switch (kind) {
        case MsgKind::REKEY_BCAST_FRAG:
        case MsgKind::REKEY_UNICAST_ACK:
        case MsgKind::KEY_RESYNC_PULL:
            return 6; // AC_VO
        case MsgKind::MISSION_C2_BCAST:
            return 4; // AC_VI
        case MsgKind::HEARTBEAT_DT_REPORT:
        case MsgKind::CH_SUMMARY_TO_SL:
        default:
            return 0; // AC_BE
    }
}

} // namespace dthsbp_v2

#endif // DT_HSBP_V2_PAYLOADS_H
