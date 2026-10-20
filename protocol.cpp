#include "protocol.h"
#include <arpa/inet.h>   // inet_pton: checks that text is a valid IPv4 address

// Helpers private to this file.
namespace {

// Looks at the UTF-8 character that starts at s[pos]. Returns how many bytes it uses
// (1-4) and stores its code point in "codePoint"; returns 0 if the bytes are not valid UTF-8.
// UTF-8 recap: ASCII is one byte (0xxxxxxx). Longer characters start with a lead byte
// (110xxxxx, 1110xxxx or 11110xxx) followed by continuation bytes (10xxxxxx).
std::size_t utf8Length(const std::string& s, std::size_t pos, unsigned long& codePoint) {
    const unsigned char first = static_cast<unsigned char>(s[pos]);
    if (first < 0x80) {
        codePoint = first;
        return 1;
    }

    std::size_t length = 0;
    unsigned long cp = 0;
    if ((first & 0xE0) == 0xC0)      { length = 2; cp = first & 0x1F; }
    else if ((first & 0xF0) == 0xE0) { length = 3; cp = first & 0x0F; }
    else if ((first & 0xF8) == 0xF0) { length = 4; cp = first & 0x07; }
    else return 0;   // a stray continuation byte, or a byte that can never start a character

    if (pos + length > s.size()) return 0;   // the character is cut off at the end

    for (std::size_t i = 1; i < length; ++i) {
        const unsigned char next = static_cast<unsigned char>(s[pos + i]);
        if ((next & 0xC0) != 0x80) return 0;   // every following byte must look like 10xxxxxx
        cp = (cp << 6) | (next & 0x3F);        // append its 6 payload bits
    }

    // Reject "overlong" encodings (a short character written with too many bytes, a
    // classic trick to sneak past filters), surrogates, and values past U+10FFFF.
    if ((length == 2 && cp < 0x80) || (length == 3 && cp < 0x800) || (length == 4 && cp < 0x10000)) return 0;
    if (cp > 0x10FFFF) return 0;
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;

    codePoint = cp;
    return length;
}

// Characters that must never reach the screen or the log.
bool isDangerousCodePoint(unsigned long cp) {
    if (cp < 0x20 || cp == 0x7F) return true;       // newline, tab, ESC (starts terminal commands), DEL...
    if (cp >= 0x80 && cp <= 0x9F) return true;      // C1 controls: some terminals obey them
    if (cp >= 0x202A && cp <= 0x202E) return true;  // bidi embeddings/overrides (text can display reversed)
    if (cp >= 0x2066 && cp <= 0x2069) return true;  // bidi isolates
    return false;
}

}  // namespace

bool encodeFrame(const std::string& payload, std::string& out) {
    if (payload.empty() || payload.size() > kMaxFramePayload) {
        return false;
    }
    const std::size_t length = payload.size();
    // Big-endian: the high byte first. "& 0xFF" keeps the lowest 8 bits.
    out.push_back(static_cast<char>((length >> 8) & 0xFF));
    out.push_back(static_cast<char>(length & 0xFF));
    out.append(payload);
    return true;
}

void FrameDecoder::feed(const char* data, std::size_t size) {
    if (failed_) {
        return;
    }
    // A peer that sends endless garbage must not make us use unlimited memory.
    if (buffer_.size() + size > kMaxDecoderBuffer) {
        failed_ = true;
        buffer_.clear();
        return;
    }
    buffer_.append(data, size);
}

FrameDecoder::Status FrameDecoder::next(std::string& payload) {
    if (failed_) {
        return Status::Error;
    }
    if (buffer_.size() < 2) {
        return Status::NeedMore;   // not even the length prefix yet
    }

    // Rebuild the 2-byte big-endian length. The cast to unsigned char matters:
    // a plain char above 127 would be negative and corrupt the number.
    const std::size_t length =
        (static_cast<std::size_t>(static_cast<unsigned char>(buffer_[0])) << 8) |
        static_cast<std::size_t>(static_cast<unsigned char>(buffer_[1]));

    if (length == 0 || length > kMaxFramePayload) {
        failed_ = true;            // the peer is lying about the size, or is not one of us
        buffer_.clear();
        return Status::Error;
    }
    if (buffer_.size() < 2 + length) {
        return Status::NeedMore;   // the rest of this frame is still on its way
    }

    payload = buffer_.substr(2, length);
    buffer_.erase(0, 2 + length);   // drop the frame we just took; keep any bytes after it
    return Status::Frame;
}

std::string sanitizeText(const std::string& text, std::size_t maxBytes) {
    std::string out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        unsigned long cp = 0;
        std::size_t length = utf8Length(text, pos, cp);

        std::string piece;
        if (length == 0) {                 // invalid UTF-8: replace ONE byte and move on
            piece = "?";
            length = 1;
        } else if (isDangerousCodePoint(cp)) {
            piece = "?";                   // "length" stays the real size, so all its bytes are skipped
        } else {
            piece = text.substr(pos, length);
        }

        if (out.size() + piece.size() > maxBytes) {
            break;                         // stop BEFORE a character that doesn't fit: never cut one in half
        }
        out += piece;
        pos += length;
    }
    return out;
}

std::string trimSpaces(const std::string& s) {
    const char* whitespace = " \t\r\n";
    const std::size_t start = s.find_first_not_of(whitespace);   // npos = "not found"
    if (start == std::string::npos) {
        return "";   // the string is empty or only whitespace
    }
    const std::size_t end = s.find_last_not_of(whitespace);
    return s.substr(start, end - start + 1);
}

bool isIpv4Address(const std::string& text) {
    // A NUL inside the text would make c_str() cut it short and hide trailing junk.
    if (text.empty() || text.find('\0') != std::string::npos) {
        return false;
    }
    unsigned char buffer[sizeof(in_addr)];
    // inet_pton ("presentation to network") returns 1 only for a valid address.
    return inet_pton(AF_INET, text.c_str(), buffer) == 1;
}

bool parseHostPort(const std::string& text, HostPort& out) {
    const std::size_t colon = text.rfind(':');   // search from the END
    if (colon == std::string::npos) {
        return false;
    }

    const std::string host = text.substr(0, colon);
    const std::string portText = text.substr(colon + 1);
    if (!isIpv4Address(host)) {
        return false;
    }
    if (portText.empty() || portText.size() > 5) {
        return false;
    }

    unsigned long port = 0;
    for (char ch : portText) {
        if (ch < '0' || ch > '9') {
            return false;   // only plain digits: no signs, spaces or "0x"
        }
        port = port * 10 + static_cast<unsigned long>(ch - '0');
    }
    if (port < 1 || port > 65535) {
        return false;
    }

    out.host = host;
    out.port = static_cast<unsigned int>(port);
    return true;
}