#pragma once
// cast_message.h
// Hand-rolled protobuf encoder for CastMessage (cast_channel.proto)
// Avoids the full nanopb toolchain for this fixed, well-known schema.
//
// Wire-type reference (proto2):
//   0 = Varint, 1 = 64-bit, 2 = Length-delimited, 5 = 32-bit
//   Field tag = (field_number << 3) | wire_type

#include <stdint.h>
#include <string.h>
#include <Arduino.h>

// ── CastMessage field numbers ───────────────────────────────────────────────
// field 1  protocol_version  varint  (always 0 = CASTV2_1_0)
// field 2  source_id         string
// field 3  destination_id    string
// field 4  namespace         string
// field 5  payload_type      varint  (0 = STRING, 1 = BINARY)
// field 6  payload_utf8      string
// field 7  payload_binary    bytes

namespace CastProto {

// Write a varint to buf, return bytes written
static inline size_t writeVarint(uint8_t* buf, uint64_t value) {
    size_t i = 0;
    while (value > 0x7F) {
        buf[i++] = (uint8_t)((value & 0x7F) | 0x80);
        value >>= 7;
    }
    buf[i++] = (uint8_t)(value & 0x7F);
    return i;
}

// Write a length-delimited string field
static inline size_t writeString(uint8_t* buf, uint8_t fieldNum, const char* str) {
    size_t len = strlen(str);
    size_t i = 0;
    // tag: (fieldNum << 3) | 2
    i += writeVarint(buf + i, ((uint64_t)fieldNum << 3) | 2);
    i += writeVarint(buf + i, (uint64_t)len);
    memcpy(buf + i, str, len);
    i += len;
    return i;
}

// Write a varint field
static inline size_t writeVarintField(uint8_t* buf, uint8_t fieldNum, uint64_t value) {
    size_t i = 0;
    i += writeVarint(buf + i, ((uint64_t)fieldNum << 3) | 0);
    i += writeVarint(buf + i, value);
    return i;
}

// Encode a full CastMessage into outBuf (caller must ensure enough space ~1024+payload)
// Returns total byte count of the encoded message (NOT including the 4-byte length prefix)
static size_t encode(
    uint8_t*    outBuf,
    size_t      outBufSize,
    const char* sourceId,
    const char* destinationId,
    const char* namespaceName,
    bool        isBinary,           // false → payload_utf8 (field 6)
    const char* payloadUtf8,        // used when !isBinary
    const uint8_t* payloadBinary,   // used when isBinary
    size_t      payloadBinaryLen
) {
    size_t i = 0;

    // field 1: protocol_version = 0
    i += writeVarintField(outBuf + i, 1, 0);

    // field 2: source_id
    i += writeString(outBuf + i, 2, sourceId);

    // field 3: destination_id
    i += writeString(outBuf + i, 3, destinationId);

    // field 4: namespace
    i += writeString(outBuf + i, 4, namespaceName);

    // field 5: payload_type  (0 = STRING, 1 = BINARY)
    i += writeVarintField(outBuf + i, 5, isBinary ? 1 : 0);

    if (!isBinary && payloadUtf8) {
        // field 6: payload_utf8
        i += writeString(outBuf + i, 6, payloadUtf8);
    } else if (isBinary && payloadBinary && payloadBinaryLen > 0) {
        // field 7: payload_binary
        i += writeVarint(outBuf + i, (7 << 3) | 2);
        i += writeVarint(outBuf + i, (uint64_t)payloadBinaryLen);
        memcpy(outBuf + i, payloadBinary, payloadBinaryLen);
        i += payloadBinaryLen;
    }

    return i;
}

} // namespace CastProto
