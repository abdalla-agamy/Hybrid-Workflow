#ifndef DT_HSBP_PAYLOAD_H
#define DT_HSBP_PAYLOAD_H

#include <cstdint>

namespace dthsbp {

/**
 * Compact binary header prepended to BytesChunk payloads sent via UDP/802.11.
 * Size is 28 bytes (< minimum 48 B LKH trace packet size).
 */
struct WireRekeyHeader {
    int32_t txId = 0;
    int32_t eventIdx = 0;
    int32_t srcUavId = 0;
    int32_t senderHostIdx = -1;
    int32_t intendedDstUavId = -1; // -1 denotes UDP broadcast
    int32_t fragIdx = 0;
    int32_t numFrags = 1;
};

static_assert(sizeof(WireRekeyHeader) == 28, "WireRekeyHeader must remain 28 bytes");

} // namespace dthsbp

#endif // DT_HSBP_PAYLOAD_H
