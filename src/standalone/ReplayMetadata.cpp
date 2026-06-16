// Minimal Rocket League .replay header parser.
//
// We only need the property dict at the top of the file:
//   - NumFrames (IntProperty)        — total frames; defines end of replay
//   - RecordFPS (FloatProperty)      — recording rate; frameTime = 1/RecordFPS
//   - Goals     (ArrayProperty)      — each element has a `frame` IntProperty
//   - MatchGuid (Name/StrProperty)   — sanity-check the replay is the right one
//
// Stops as soon as the header dict's "None" terminator is read; the rest of
// the .replay file (network stream, key frames, debug strings) is skipped.
//
// Format references: jjbott/RocketLeagueReplayParser, nickbabcock/boxcars.
// String encoding: int32 length-prefixed; positive = ASCII (Win-1252), each
// char a byte; negative = UTF-16 LE, |length| chars at 2 bytes each. The
// length INCLUDES the trailing null character.
#include "SyncComms/ReplayMetadata.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>

namespace SyncComms {

namespace {

class Reader {
public:
    explicit Reader(std::istream& in) : m_in(in) {}

    bool Read(void* dst, size_t n) {
        if (!m_in.read(static_cast<char*>(dst), static_cast<std::streamsize>(n))) {
            m_failed = true;
            return false;
        }
        return true;
    }
    bool U32(uint32_t& v) { return Read(&v, 4); }
    bool I32(int32_t& v)  { return Read(&v, 4); }
    bool U64(uint64_t& v) { return Read(&v, 8); }
    bool U8(uint8_t& v)   { return Read(&v, 1); }
    bool F32(float& v)    { return Read(&v, 4); }
    bool Skip(uint64_t n) {
        m_in.seekg(static_cast<std::streamoff>(n), std::ios::cur);
        if (!m_in) { m_failed = true; return false; }
        return true;
    }
    bool Failed() const { return m_failed; }

    bool String(std::string& out) {
        int32_t len;
        if (!I32(len)) return false;
        out.clear();
        if (len == 0) return true;
        if (len > 0) {
            // ASCII, len includes trailing null
            std::vector<char> buf(static_cast<size_t>(len));
            if (!Read(buf.data(), buf.size())) return false;
            size_t actual = buf.size();
            while (actual > 0 && buf[actual - 1] == '\0') --actual;
            out.assign(buf.data(), actual);
            return true;
        }
        // UTF-16 LE, |len| chars including null. Convert to UTF-8 (BMP only).
        size_t chars = static_cast<size_t>(-len);
        std::vector<uint8_t> buf(chars * 2);
        if (!Read(buf.data(), buf.size())) return false;
        if (chars > 0) chars--;  // drop null terminator pair
        for (size_t i = 0; i < chars; ++i) {
            uint16_t code = static_cast<uint16_t>(buf[i*2]) |
                            (static_cast<uint16_t>(buf[i*2 + 1]) << 8);
            if (code < 0x80) {
                out.push_back(static_cast<char>(code));
            } else if (code < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
        }
        return true;
    }

private:
    std::istream& m_in;
    bool m_failed = false;
};

// One header property's deserialized value. We keep all the fields generic
// rather than try to be type-strict — callers look up by name and check the
// fields they care about.
struct PropValue {
    std::string type;
    int64_t     intValue   = 0;
    double      floatValue = 0.0;
    bool        boolValue  = false;
    std::string strValue;
    std::vector<std::map<std::string, PropValue>> arrayValue;  // for ArrayProperty
};

// Forward decl — Property and Dict reference each other (Dict for arrays).
bool ParsePropertyValue(Reader& r, const std::string& type, uint64_t size, PropValue& out);

bool ParseDict(Reader& r, std::map<std::string, PropValue>& out) {
    // Hard cap on properties per dict — both for safety against corrupted
    // headers and to keep the parser bounded if "None" is missing somehow.
    for (int i = 0; i < 256; ++i) {
        std::string name;
        if (!r.String(name)) return false;
        if (name == "None") return true;

        std::string type;
        uint64_t size = 0;
        if (!r.String(type)) return false;
        if (!r.U64(size))    return false;

        PropValue val;
        if (!ParsePropertyValue(r, type, size, val)) return false;
        out.emplace(std::move(name), std::move(val));
    }
    return false;
}

bool ParsePropertyValue(Reader& r, const std::string& type, uint64_t size, PropValue& out) {
    out.type = type;
    if (type == "IntProperty") {
        int32_t v;
        if (!r.I32(v)) return false;
        out.intValue = v;
        return true;
    }
    if (type == "FloatProperty") {
        // Almost always 4 bytes; if size==8 (rare), read double.
        if (size == 8) {
            double v;
            if (!r.Read(&v, 8)) return false;
            out.floatValue = v;
        } else {
            float v;
            if (!r.F32(v)) return false;
            out.floatValue = v;
        }
        return true;
    }
    if (type == "BoolProperty") {
        uint8_t v;
        if (!r.U8(v)) return false;
        out.boolValue = (v != 0);
        return true;
    }
    if (type == "StrProperty" || type == "NameProperty") {
        return r.String(out.strValue);
    }
    if (type == "QWordProperty") {
        uint64_t v;
        if (!r.U64(v)) return false;
        out.intValue = static_cast<int64_t>(v);
        return true;
    }
    if (type == "ByteProperty") {
        // RL replays store these as (enum_name, enum_value), both strings.
        std::string enumName, enumValue;
        if (!r.String(enumName))  return false;
        if (!r.String(enumValue)) return false;
        out.strValue = std::move(enumValue);
        return true;
    }
    if (type == "ArrayProperty") {
        uint32_t count;
        if (!r.U32(count)) return false;
        // Sanity cap — a reasonable replay won't have >4096 array entries
        // anywhere we care about.
        if (count > 4096) return false;
        out.arrayValue.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            std::map<std::string, PropValue> elem;
            if (!ParseDict(r, elem)) return false;
            out.arrayValue.push_back(std::move(elem));
        }
        return true;
    }
    // Unknown property type — best-effort skip via the size field. May desync
    // the parser if the type's size accounting differs (e.g., struct types),
    // in which case downstream property reads will fail and we'll bail. For
    // the property names we care about (NumFrames/Goals/MatchGuid) we always
    // hit one of the recognized types above before this fallback matters.
    return r.Skip(size);
}

} // namespace

bool ParseReplayHeader(const std::filesystem::path& path, ReplayMetadata& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    Reader r(f);

    // File header: size+CRC of the property block, then version triplet, then
    // the replay class name string.
    uint32_t headerSize, headerCrc;
    if (!r.U32(headerSize) || !r.U32(headerCrc)) return false;

    uint32_t engineVersion, licenseeVersion, netVersion = 0;
    if (!r.U32(engineVersion) || !r.U32(licenseeVersion)) return false;
    // netVersion is only present when the engine/licensee versions are recent
    // enough. The exact threshold matches what jjbott's parser uses.
    if (engineVersion > 868 ||
        (engineVersion == 868 && licenseeVersion >= 18)) {
        if (!r.U32(netVersion)) return false;
    }

    std::string className;
    if (!r.String(className)) return false;

    // Tolerant top-level parse: keep every property that deserializes cleanly
    // and STOP at the first malformed one instead of discarding the whole
    // header. RL replay headers end with array-of-struct properties
    // (HighLights, PlayerStats) whose element layout — ByteProperty enums,
    // structs — shifts across game versions and can desync this minimal
    // parser. Everything we need (Goals, MatchStartEpoch, NumFrames,
    // RecordFPS) appears BEFORE them, so a desync deep in PlayerStats must
    // not cost us the goal frames we already read. (The old code called
    // ParseDict and bailed on any failure, which discarded a perfectly good
    // Goals array every time PlayerStats tripped the parser.)
    std::map<std::string, PropValue> props;
    for (int i = 0; i < 256; ++i) {
        std::string name;
        if (!r.String(name) || name == "None") break;
        std::string type;
        uint64_t size = 0;
        if (!r.String(type) || !r.U64(size)) break;
        PropValue val;
        if (!ParsePropertyValue(r, type, size, val)) break;
        props.emplace(std::move(name), std::move(val));
    }

    auto numFramesIt = props.find("NumFrames");
    if (numFramesIt != props.end() && numFramesIt->second.type == "IntProperty") {
        out.numFrames = static_cast<int>(numFramesIt->second.intValue);
    }

    auto guidIt = props.find("MatchGuid");
    if (guidIt != props.end()) {
        out.matchGuid = guidIt->second.strValue;
    }

    auto fpsIt = props.find("RecordFPS");
    if (fpsIt != props.end() && fpsIt->second.type == "FloatProperty" &&
        fpsIt->second.floatValue > 0.0) {
        out.recordFps = fpsIt->second.floatValue;
    }

    // Present on current RL replays; used to match a sidecar to its .replay
    // by time (MatchGuid is absent from the header on this RL version).
    auto epochIt = props.find("MatchStartEpoch");
    if (epochIt != props.end() && epochIt->second.type == "QWordProperty") {
        out.matchStartEpoch = epochIt->second.intValue;
    }

    auto secsIt = props.find("TotalSecondsPlayed");
    if (secsIt != props.end() && secsIt->second.type == "FloatProperty") {
        out.totalSecondsPlayed = secsIt->second.floatValue;
    }

    auto goalsIt = props.find("Goals");
    if (goalsIt != props.end() && goalsIt->second.type == "ArrayProperty") {
        for (const auto& goal : goalsIt->second.arrayValue) {
            // The frame field is conventionally lowercase 'frame'. Accept
            // either casing in case the property name varies across replay
            // versions.
            auto fIt = goal.find("frame");
            if (fIt == goal.end()) fIt = goal.find("Frame");
            if (fIt != goal.end() && fIt->second.type == "IntProperty") {
                out.goalFrames.push_back(static_cast<int>(fIt->second.intValue));
            }
        }
    }

    // Success if we got anything usable. NumFrames is absent on current RL
    // replays, so requiring it (the old gate) discarded every real file —
    // goalFrames alone is enough to anchor.
    return !out.goalFrames.empty() || out.numFrames > 0 || out.matchStartEpoch > 0;
}

} // namespace SyncComms
