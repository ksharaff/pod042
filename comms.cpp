#include "comms.h"
#include <arpa/inet.h>    // inet_pton, inet_ntop, htons
#include <fcntl.h>        // O_NONBLOCK, O_CLOEXEC
#include <netinet/in.h>   // sockaddr_in
#include <poll.h>         // poll(), struct pollfd
#include <sys/socket.h>   // socket(), bind(), listen(), accept4(), connect(), send(), recv()
#include <unistd.h>       // close(), read(), write(), pipe2()
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>        // std::strerror
#include <ctime>
#include "log.h"

// A monotonic clock: it never jumps backwards, so it is the right one for timeouts.
using Clock = std::chrono::steady_clock;

// One connection to another pod. Only the I/O thread ever touches these.
struct CommsNode::Link {
    int fd = -1;                    // the socket
    bool outbound = false;          // we dialed it (true) or it dialed us (false)
    int dialIndex = -1;             // which comms_connect entry created it (outbound only)
    bool connecting = false;        // outbound connect() is still in progress
    bool helloDone = false;         // the peer has introduced itself
    bool dead = false;              // marked for closing at the end of this loop round
    std::string deadReason;
    std::string ip;
    unsigned int port = 0;
    std::string name;               // from the peer's HELLO (cleaned)
    FrameDecoder decoder;           // cuts incoming bytes into frames
    std::string outBuf;             // bytes waiting to be sent
    Clock::time_point startedAt;    // when the link (or its connect) started
    Clock::time_point lastRecv;     // last time ANY bytes arrived
    Clock::time_point lastPing;
    Clock::time_point windowStart;  // start of the current 1-second rate-limit window
    int framesInWindow = 0;
};

namespace {

using Link = CommsNode::Link;

// ---- Limits ----
const std::size_t kMaxLinks = 8;                  // connections at once, in and out together
const std::size_t kMaxHistoryLines = 200;
const std::size_t kMaxQueuedMessages = 32;        // messages waiting to be sent
const std::size_t kMaxOutBuffer = 64 * 1024;      // a peer that can't keep up gets dropped
const int kFramesPerSecondLimit = 20;
const auto kConnectTimeout = std::chrono::seconds(5);
const auto kHelloTimeout = std::chrono::seconds(5);    // time a new link has to introduce itself
const auto kPingInterval = std::chrono::seconds(5);
const auto kIdleTimeout = std::chrono::seconds(15);    // silence for this long = SIGNAL LOST
const int kPollMs = 250;                               // how often poll() wakes up on its own

// What we know about one comms_connect entry.
struct DialState {
    bool active = false;                 // a link for it exists (connecting or established)
    bool reportedDown = false;           // we already told the user it is unreachable
    Clock::time_point nextAttempt;       // earliest time to dial again
};

enum class LinkIssue { None, ClosedByPeer, SocketError, ProtocolError, TooFast };

const char* issueText(LinkIssue issue) {
    switch (issue) {
        case LinkIssue::ClosedByPeer:  return "connection closed";
        case LinkIssue::SocketError:   return "socket error";
        case LinkIssue::ProtocolError: return "protocol error";
        case LinkIssue::TooFast:       return "rate limit exceeded";
        case LinkIssue::None:          break;
    }
    return "unknown";
}

std::string nowText() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);   // the thread-safe version of localtime
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &local);
    return buf;
}

// Creates a listening socket. Returns its file descriptor, or -1 with the reason in "error".
// Sockets are NON-BLOCKING (a call that would have to wait returns immediately with
// EAGAIN instead) so one slow peer can never freeze the I/O thread.
// SOCK_CLOEXEC keeps the socket from leaking into any program we might launch.
int makeListener(const std::string& bindAddress, unsigned int port, std::string& error) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = std::strerror(errno);
        return -1;
    }

    // SO_REUSEADDR: allow restarting right away instead of waiting for the old socket to expire.
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};                                          // zero-initialized
    addr.sin_family = AF_INET;                                   // IPv4
    addr.sin_port = htons(static_cast<std::uint16_t>(port));     // htons: host byte order -> network (big-endian)
    if (inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) {
        error = "invalid bind address";
        ::close(fd);
        return -1;
    }

    // bind: attach the socket to an address and port. listen: start accepting
    // connections (8 = how many may wait in line).
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 8) != 0) {
        error = std::strerror(errno);   // read errno BEFORE close(), which may overwrite it
        ::close(fd);
        return -1;
    }
    return fd;
}

// Appends one frame (type byte + text) to the link's send buffer.
// False if it can't be encoded or the peer is too far behind (buffer full).
bool queueFrame(Link& link, char type, const std::string& text) {
    std::string payload(1, type);
    payload += text;

    std::string frame;
    if (!encodeFrame(payload, frame)) {
        return false;
    }
    if (link.outBuf.size() + frame.size() > kMaxOutBuffer) {
        return false;
    }
    link.outBuf += frame;
    return true;
}

void markDead(Link& link, const std::string& reason) {
    if (!link.dead) {
        link.dead = true;
        link.deadReason = reason;
    }
}

// Sends as much of the buffer as the socket accepts right now. False = the link failed.
bool flushLink(Link& link) {
    while (!link.outBuf.empty()) {
        // MSG_NOSIGNAL: if the peer already hung up, return an error instead of killing
        // the WHOLE PROGRAM with the SIGPIPE signal (the default for a write to a closed socket).
        const ssize_t n = ::send(link.fd, link.outBuf.data(), link.outBuf.size(), MSG_NOSIGNAL);
        if (n > 0) {
            link.outBuf.erase(0, static_cast<std::size_t>(n));
        } else if (n < 0 && errno == EINTR) {
            continue;                                   // interrupted by a signal: just retry
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;                                // buffer full: poll() says when to continue
        } else {
            return false;
        }
    }
    return true;
}

// Reads what has arrived on a link and cuts it into frames (appended to "frames").
LinkIssue readLink(Link& link, std::vector<std::string>& frames, Clock::time_point now) {
    char buffer[2048];
    for (int round = 0; round < 8; ++round) {   // bounded work per loop round
        const ssize_t n = ::recv(link.fd, buffer, sizeof(buffer), 0);

        if (n > 0) {
            link.lastRecv = now;
            link.decoder.feed(buffer, static_cast<std::size_t>(n));

            std::string payload;
            for (;;) {
                const FrameDecoder::Status status = link.decoder.next(payload);
                if (status == FrameDecoder::Status::NeedMore) break;
                if (status == FrameDecoder::Status::Error) return LinkIssue::ProtocolError;

                // Rate limit: count frames in a sliding one-second window.
                if (now - link.windowStart >= std::chrono::seconds(1)) {
                    link.windowStart = now;
                    link.framesInWindow = 0;
                }
                if (++link.framesInWindow > kFramesPerSecondLimit) {
                    return LinkIssue::TooFast;
                }
                frames.push_back(payload);
            }
        } else if (n == 0) {
            return LinkIssue::ClosedByPeer;             // recv() returning 0 means "the peer closed"
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return LinkIssue::None;                     // nothing more to read right now
        } else if (errno != EINTR) {
            return LinkIssue::SocketError;
        }
    }
    return LinkIssue::None;   // more may be waiting: poll() reports it again immediately
}

}  // namespace

CommsNode::~CommsNode() {
    stop();
}

void CommsNode::setOnChange(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    onChange_ = std::move(callback);
}

void CommsNode::notifyChange() {
    // Copy the callback under the lock, but CALL it after releasing the lock, so
    // the callback can never deadlock against us.
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = onChange_;
    }
    if (callback) {
        callback();
    }
}

void CommsNode::addLine(CommsKind kind, const std::string& text) {
    CommsLine line;
    line.time = nowText();
    line.kind = kind;
    line.text = text;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(std::move(line));
        while (lines_.size() > kMaxHistoryLines) {
            lines_.pop_front();   // forget the oldest
        }
    }
    notifyChange();
}

void CommsNode::wake() {
    std::lock_guard<std::mutex> lock(wakeMutex_);
    if (wakeWrite_ >= 0) {
        const ssize_t ignored = ::write(wakeWrite_, "x", 1);   // one byte is enough
        (void)ignored;   // if the pipe is full, a wake-up is already pending: nothing to do
    }
}

bool CommsNode::start(const CommsConfig& config) {
    if (thread_.joinable()) {
        return true;   // already started
    }

    // A pipe is a one-way channel inside the process. poll() watches its read end, so
    // writing a byte to its write end wakes the I/O thread at once.
    int fds[2];
    if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
        addLine(CommsKind::System, "COMMS: CANNOT START (PIPE FAILED)");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(wakeMutex_);
        wakeRead_ = fds[0];
        wakeWrite_ = fds[1];
    }

    config_ = config;
    stopRequested_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        outgoing_.clear();
        linkInfos_.clear();
        listening_ = false;
        listenText_.clear();
    }

    running_ = true;
    thread_ = std::thread([this] { ioLoop(); });
    return true;
}

void CommsNode::stop() {
    if (!thread_.joinable()) {
        return;   // never started, or already stopped
    }
    stopRequested_ = true;
    wake();
    thread_.join();   // the I/O thread closes all its sockets before it returns

    {
        std::lock_guard<std::mutex> lock(wakeMutex_);
        ::close(wakeRead_);
        ::close(wakeWrite_);
        wakeRead_ = -1;
        wakeWrite_ = -1;
    }
    running_ = false;
    addLine(CommsKind::System, "COMMS OFFLINE");
}

bool CommsNode::send(const std::string& text) {
    const std::string clean = trimSpaces(sanitizeText(trimSpaces(text), kMaxMessageBytes));
    if (clean.empty() || !running_) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (outgoing_.size() >= kMaxQueuedMessages) {
            return false;
        }
        outgoing_.push_back(clean);
    }
    wake();   // the I/O thread picks it up right away instead of after its next timeout
    return true;
}

CommsSnapshot CommsNode::snapshot() const {
    CommsSnapshot s;
    s.running = running_;
    std::lock_guard<std::mutex> lock(mutex_);
    s.listening = listening_;
    s.listenText = listenText_;
    s.links = linkInfos_;
    s.lines.assign(lines_.begin(), lines_.end());
    return s;
}

// Handles one complete frame from a peer. Returns false if the peer broke the rules.
bool CommsNode::handleFrame(Link& link, const std::string& payload) {
    const char type = payload[0];                // never empty: the decoder rejects length 0
    const std::string body = payload.substr(1);

    if (type == kFramePing) {
        return true;                             // only proves the peer is alive
    }

    if (type == kFrameHello) {
        if (link.helloDone) {
            return false;                        // introducing yourself twice is not allowed
        }
        // The name is chosen by the peer and nobody has checked it: clean it like any untrusted text.
        std::string name = trimSpaces(sanitizeText(body, kMaxPeerNameBytes));
        if (name.empty()) {
            name = "UNKNOWN";
        }
        link.name = name;
        link.helloDone = true;
        addLine(CommsKind::System, "LINK ESTABLISHED: " + name + " (" + link.ip + ")");
        logEvent(LogLevel::Info, "Comms: link established with " + name + " (" + link.ip + ")");
        return true;
    }

    if (type == kFrameMessage) {
        if (!link.helloDone) {
            return false;                        // messages are only accepted AFTER the introduction
        }
        const std::string text = trimSpaces(sanitizeText(body, kMaxMessageBytes));
        if (!text.empty()) {
            addLine(CommsKind::Incoming, link.name + ": " + text);
        }
        return true;
    }

    return false;                                // unknown frame type
}

// The I/O thread: one loop that listens, dials, reads, writes and watches the clock.
// poll() is the heart of it: "sleep until ANY of these sockets needs attention,
// or 250 ms pass". That lets one thread serve every connection.
void CommsNode::ioLoop() {
    std::vector<Link> links;                                      // every open connection
    std::vector<DialState> targets(config_.connectTo.size());     // one per comms_connect entry
    int listenFd = -1;
    const auto redial = std::chrono::milliseconds(config_.redialMs);
    Clock::time_point lastRefusalLog = Clock::now() - std::chrono::seconds(10);
    int refusedSinceLog = 0;
    bool linksChanged = true;   // publish the (empty) link list once at the start

    addLine(CommsKind::System, "COMMS ONLINE");
    if (config_.allowFrom.empty() && config_.connectTo.empty()) {
        addLine(CommsKind::System,
                "NO PEERS CONFIGURED. SET comms_connect AND/OR comms_allow IN config.json.");
    }

    // ---- The listener: only opened if somebody is allowed to connect ----
    if (!config_.allowFrom.empty()) {
        std::string error;
        listenFd = makeListener(config_.bindAddress, config_.port, error);
        const std::string where = config_.bindAddress + ":" + std::to_string(config_.port);
        if (listenFd >= 0) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                listening_ = true;
                listenText_ = where;
            }
            addLine(CommsKind::System, "LISTENING ON " + where);
            logEvent(LogLevel::Info, "Comms: listening on " + where);
        } else {
            addLine(CommsKind::System, "CANNOT LISTEN ON " + where + ": " + error);
            logEvent(LogLevel::Warning, "Comms: cannot listen on " + where + ": " + error);
        }
    }

    // Tells the user once that a peer can't be reached (not every retry).
    auto reportUnreachable = [&](std::size_t index) {
        if (targets[index].reportedDown) {
            return;
        }
        targets[index].reportedDown = true;
        const HostPort& hp = config_.connectTo[index];
        addLine(CommsKind::System,
                "CANNOT REACH " + hp.host + ":" + std::to_string(hp.port) + ". WILL KEEP TRYING.");
    };

    while (!stopRequested_) {
        const Clock::time_point now = Clock::now();

        // ---- 1. Dial the peers we are not connected to ----
        for (std::size_t i = 0; i < targets.size(); ++i) {
            DialState& t = targets[i];
            if (t.active || now < t.nextAttempt || links.size() >= kMaxLinks) {
                continue;
            }
            const HostPort& target = config_.connectTo[i];
            t.nextAttempt = now + redial;

            const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            bool started = false;
            bool inProgress = false;
            if (fd >= 0) {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(static_cast<std::uint16_t>(target.port));
                if (inet_pton(AF_INET, target.host.c_str(), &addr.sin_addr) == 1) {
                    // A NON-BLOCKING connect() returns at once: 0 = already connected
                    // (can happen on loopback), or -1 with EINPROGRESS = "working on it".
                    // poll() tells us later when it finished.
                    const int r = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
                    if (r == 0) {
                        started = true;
                    } else if (errno == EINPROGRESS) {
                        started = true;
                        inProgress = true;
                    }
                }
                if (!started) {
                    ::close(fd);
                }
            }
            if (!started) {
                reportUnreachable(i);
                continue;
            }

            Link link;
            link.fd = fd;
            link.outbound = true;
            link.dialIndex = static_cast<int>(i);
            link.connecting = inProgress;
            link.ip = target.host;
            link.port = target.port;
            link.startedAt = link.lastRecv = link.lastPing = link.windowStart = now;
            if (!inProgress) {
                queueFrame(link, kFrameHello, config_.localName);
            }
            links.push_back(std::move(link));
            t.active = true;
            linksChanged = true;
        }

        // ---- 2. Take the messages the UI queued and hand them to every linked pod ----
        std::deque<std::string> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending.swap(outgoing_);   // take everything, leave the shared queue empty
        }
        for (const std::string& text : pending) {
            std::size_t sentTo = 0;
            for (Link& l : links) {
                if (l.dead || l.connecting || !l.helloDone) {
                    continue;
                }
                if (queueFrame(l, kFrameMessage, text)) {
                    ++sentTo;
                } else {
                    markDead(l, "peer too slow");
                }
            }
            if (sentTo > 0) {
                addLine(CommsKind::Outgoing, "YOU: " + text);
            } else {
                addLine(CommsKind::System, "NO PEER LINKED: MESSAGE NOT SENT");
            }
        }

        // ---- 3. Keep-alive pings, so silence means "gone" and not just "nothing to say" ----
        for (Link& l : links) {
            if (!l.dead && l.helloDone && now - l.lastPing >= kPingInterval) {
                l.lastPing = now;
                if (!queueFrame(l, kFramePing, "")) {
                    markDead(l, "peer too slow");
                }
            }
        }

        // ---- 4. Timeouts ----
        for (Link& l : links) {
            if (l.dead) {
                continue;
            }
            if (l.connecting) {
                if (now - l.startedAt > kConnectTimeout) markDead(l, "connection timed out");
            } else if (!l.helloDone) {
                if (now - l.startedAt > kHelloTimeout) markDead(l, "no introduction");
            } else if (now - l.lastRecv > kIdleTimeout) {
                markDead(l, "no signal");
            }
        }

        // ---- 5. Tell poll() what to watch ----
        std::vector<pollfd> pfds;
        pfds.push_back({wakeRead_, POLLIN, 0});   // index 0: the wake pipe
        int listenIndex = -1;
        if (listenFd >= 0) {
            listenIndex = static_cast<int>(pfds.size());
            pfds.push_back({listenFd, POLLIN, 0});   // POLLIN on a listener = "someone is connecting"
        }
        const std::size_t linkBase = pfds.size();
        for (const Link& l : links) {
            short events = 0;                          // 0 = only report errors / hang-ups
            if (!l.dead) {
                if (l.connecting) {
                    events = POLLOUT;                  // a connecting socket becomes WRITABLE when done
                } else {
                    events = POLLIN;                   // POLLIN = data to read
                    if (!l.outBuf.empty()) {
                        events = static_cast<short>(events | POLLOUT);   // POLLOUT = we have data to write
                    }
                }
            }
            pfds.push_back({l.fd, events, 0});
        }
        const std::size_t polledLinks = links.size();

        // ---- 6. Sleep until something happens (or 250 ms pass) ----
        const int ready = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), kPollMs);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;   // a signal interrupted the wait: harmless
            }
            addLine(CommsKind::System, "COMMS I/O FAILURE: STOPPED");
            logEvent(LogLevel::Error, "Comms: poll failed, link stopped");
            break;
        }

        // Empty the wake pipe (its only job was to wake us).
        if (pfds[0].revents & POLLIN) {
            char drain[64];
            while (::read(wakeRead_, drain, sizeof(drain)) > 0) {
            }
        }

        // ---- 7. Serve each existing link ----
        for (std::size_t i = 0; i < polledLinks; ++i) {
            Link& l = links[i];
            const short revents = pfds[linkBase + i].revents;
            if (l.dead) {
                continue;
            }

            if (l.connecting) {
                if (revents & (POLLOUT | POLLERR | POLLHUP)) {
                    // The connect attempt is over. Did it work? SO_ERROR holds the answer.
                    int err = 0;
                    socklen_t len = sizeof(err);
                    if (::getsockopt(l.fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
                        err = errno;
                    }
                    if (err != 0) {
                        markDead(l, "connection failed");
                    } else {
                        l.connecting = false;
                        l.startedAt = l.lastRecv = l.lastPing = l.windowStart = now;   // restart the clocks
                        if (!queueFrame(l, kFrameHello, config_.localName)) {
                            markDead(l, "peer too slow");
                        }
                    }
                }
                continue;
            }

            if (revents & POLLNVAL) {
                markDead(l, "socket error");
                continue;
            }

            if (revents & (POLLIN | POLLHUP | POLLERR)) {
                const bool wasReady = l.helloDone;
                std::vector<std::string> frames;
                const LinkIssue issue = readLink(l, frames, now);

                // Handle what arrived BEFORE the problem (a peer may send its last words, then close).
                for (const std::string& payload : frames) {
                    if (!handleFrame(l, payload)) {
                        markDead(l, "protocol error");
                        break;
                    }
                }
                if (!wasReady && l.helloDone) {
                    linksChanged = true;
                    if (l.outbound) {
                        targets[static_cast<std::size_t>(l.dialIndex)].reportedDown = false;
                    }
                }
                if (issue != LinkIssue::None) {
                    markDead(l, issueText(issue));
                }
            }

            if (!l.dead && (revents & POLLOUT)) {
                if (!flushLink(l)) {
                    markDead(l, "send failed");
                }
            }
        }

        // ---- 8. Accept new inbound connections ----
        if (listenIndex >= 0 && (pfds[static_cast<std::size_t>(listenIndex)].revents & POLLIN)) {
            for (int accepted = 0; accepted < 8; ++accepted) {   // bounded work per round
                sockaddr_in peer{};
                socklen_t peerLen = sizeof(peer);
                const int fd = ::accept4(listenFd, reinterpret_cast<sockaddr*>(&peer), &peerLen,
                                         SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (fd < 0) {
                    break;   // EAGAIN = nobody else waiting; any other error: try next round
                }

                char ipText[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &peer.sin_addr, ipText, sizeof(ipText));
                const std::string ip = ipText;

                // The allowlist check happens BEFORE we read a single byte from the stranger.
                if (config_.allowFrom.count(ip) == 0 || links.size() >= kMaxLinks) {
                    ::close(fd);
                    ++refusedSinceLog;
                    continue;
                }

                Link link;
                link.fd = fd;
                link.outbound = false;
                link.ip = ip;
                link.port = ntohs(peer.sin_port);   // ntohs: network byte order -> host
                link.startedAt = link.lastRecv = link.lastPing = link.windowStart = now;
                queueFrame(link, kFrameHello, config_.localName);   // introduce ourselves first
                links.push_back(std::move(link));
                linksChanged = true;
            }
        }

        // ---- 9. Close the dead links (walking backwards, so erasing is safe) ----
        for (std::size_t i = links.size(); i-- > 0;) {
            Link& l = links[i];
            if (!l.dead) {
                continue;
            }
            ::close(l.fd);

            if (l.helloDone) {
                addLine(CommsKind::System, "SIGNAL LOST: " + l.name + " (" + l.deadReason + ")");
                logEvent(LogLevel::Info,
                         "Comms: link lost with " + l.name + " (" + l.ip + "): " + l.deadReason);
            } else if (!l.outbound) {
                ++refusedSinceLog;   // an inbound stranger that never completed the introduction
            }

            if (l.outbound && l.dialIndex >= 0) {
                const std::size_t index = static_cast<std::size_t>(l.dialIndex);
                targets[index].active = false;
                targets[index].nextAttempt = now + redial;
                if (!l.helloDone) {
                    reportUnreachable(index);
                }
            }
            links.erase(links.begin() + static_cast<std::ptrdiff_t>(i));
            linksChanged = true;
        }

        // Summarize refused/dropped connections: at most one line every 10 seconds,
        // so a flood of connection attempts can't flood the log.
        if (refusedSinceLog > 0 && now - lastRefusalLog >= std::chrono::seconds(10)) {
            logEvent(LogLevel::Warning, "Comms: refused or dropped " + std::to_string(refusedSinceLog) +
                                            " connection(s) from hosts not in comms_allow or that broke the protocol");
            refusedSinceLog = 0;
            lastRefusalLog = now;
        }

        // ---- 10. Publish the link list for the UI ----
        if (linksChanged) {
            std::vector<CommsLink> infos;
            for (const Link& l : links) {
                CommsLink info;
                info.name = l.name;
                info.address = l.ip + ":" + std::to_string(l.port);
                info.outbound = l.outbound;
                info.ready = l.helloDone && !l.connecting;
                infos.push_back(info);
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                linkInfos_ = std::move(infos);
            }
            linksChanged = false;
            notifyChange();
        }
    }

    // ---- Shutdown: close everything we opened ----
    for (Link& l : links) {
        ::close(l.fd);
    }
    if (listenFd >= 0) {
        ::close(listenFd);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    linkInfos_.clear();
    listening_ = false;
    listenText_.clear();
}