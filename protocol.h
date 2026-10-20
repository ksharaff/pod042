#pragma once
#include <cstddef>
#include <string>

// ---- Limits. Everything that arrives from the network is bounded. ----
inline constexpr std::size_t kMaxFramePayload = 1024;       // bytes in one frame (type byte + text)
inline constexpr std::size_t kMaxMessageBytes = 500;        // longest message text we send or show
inline constexpr std::size_t kMaxPeerNameBytes = 32;        // longest pod name
inline constexpr std::size_t kMaxDecoderBuffer = 16 * 1024; // the decoder never holds more than this

// The first byte of every frame says what kind of frame it is.
inline constexpr char kFrameHello = 'H';     // "I am <name>": must be the first frame on a link
inline constexpr char kFrameMessage = 'M';   // a chat message
inline constexpr char kFramePing = 'P';      // keep-alive, carries no text

// An IPv4 address and a port, e.g. 192.168.1.20 and 4242.
struct HostPort {
    std::string host;
    unsigned int port = 0;
};

// TCP is a stream of bytes with no message boundaries, so every message is sent as
// a FRAME:   [2 bytes: length, big-endian] [length bytes: payload]
// Appends one frame to "out". Returns false (changing nothing) if the payload is
// empty or longer than kMaxFramePayload.
bool encodeFrame(const std::string& payload, std::string& out);

// Collects incoming bytes and cuts them into frames. Bytes can arrive in any size
// pieces: half a frame now, the rest later, or three frames at once.
class FrameDecoder {
public:
    enum class Status {
        NeedMore,   // no complete frame yet
        Frame,      // a frame was written to "payload"
        Error       // the peer broke the rules: hang up. The decoder stays failed afterwards.
    };

    void feed(const char* data, std::size_t size);
    Status next(std::string& payload);   // call repeatedly until it returns NeedMore

private:
    std::string buffer_;
    bool failed_ = false;
};

// Makes untrusted text safe to show and to write to a log:
//  - control characters (newline, ESC, DEL, and the C1 range) become '?'
//  - invalid UTF-8 becomes '?' (one '?' per bad byte)
//  - bidi override characters become '?' (they can make text display backwards)
//  - the result is cut at a CHARACTER boundary so it is never longer than maxBytes
std::string sanitizeText(const std::string& text, std::size_t maxBytes);

// Removes spaces, tabs and newlines from both ends.
std::string trimSpaces(const std::string& s);

// True only for a plain dotted IPv4 address like "192.168.1.20".
bool isIpv4Address(const std::string& text);

// Parses "192.168.1.20:4242". The host must be an IPv4 address (no DNS names: looking
// one up could block), the port a whole number from 1 to 65535.
bool parseHostPort(const std::string& text, HostPort& out);