#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "protocol.h"

// Everything the node needs to know. main() fills it in from config.json.
struct CommsConfig {
    std::string localName = "UNIT";          // how we introduce ourselves to other pods
    std::string bindAddress = "127.0.0.1";   // which of OUR addresses we listen on
    unsigned int port = 4242;                // the port we listen on
    std::vector<HostPort> connectTo;         // pods we dial out to
    std::set<std::string> allowFrom;         // IPv4 addresses allowed to dial IN; empty = never listen
    int redialMs = 3000;                     // wait this long before dialing a missing peer again
};

enum class CommsKind { Incoming, Outgoing, System };

// One line of the message history.
struct CommsLine {
    std::string time;                        // "HH:MM:SS"
    CommsKind kind = CommsKind::System;
    std::string text;
};

// What the UI needs to know about one connection.
struct CommsLink {
    std::string name;                        // empty until the peer has introduced itself
    std::string address;                     // "192.168.1.20:4242"
    bool outbound = false;                   // true = we dialed it, false = it dialed us
    bool ready = false;                      // connected AND introduced: messages can flow
};

struct CommsSnapshot {
    bool running = false;
    bool listening = false;
    std::string listenText;                  // "192.168.1.10:4242" while listening
    std::vector<CommsLink> links;
    std::vector<CommsLine> lines;            // oldest first
};

// Sends and receives text messages between pods over TCP. All the socket work happens
// on ONE background thread. The other threads only talk to it through three
// thread-safe doors: send() (queue a message), snapshot() (copy the current state)
// and start()/stop().
//
// start() and stop() must always be called from the SAME thread (main() uses the
// refresher thread, and calls stop() again after that thread has finished).
class CommsNode {
public:
    // One connection. Defined in comms.cpp: the outside world can't look inside it.
    struct Link;

    ~CommsNode();   // calls stop()

    // Called from the I/O thread whenever something changed on screen: the UI uses it to redraw.
    // Set it BEFORE start().
    void setOnChange(std::function<void()> callback);

    bool start(const CommsConfig& config);   // starts the I/O thread; true if it is running
    void stop();                             // stops it and closes every socket

    bool running() const { return running_; }

    // Queues a message for every linked pod. The text is cleaned and shortened first.
    // Returns false if it was empty, the node is not running, or the queue is full.
    bool send(const std::string& text);

    CommsSnapshot snapshot() const;          // a COPY, safe to read on any thread

private:
    void ioLoop();                                               // the thread's main function
    bool handleFrame(Link& link, const std::string& payload);    // false = the peer broke the rules
    void addLine(CommsKind kind, const std::string& text);
    void notifyChange();
    void wake();   // makes the I/O thread's poll() return immediately

    CommsConfig config_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::function<void()> onChange_;

    // The "wake pipe": writing one byte to wakeWrite_ makes wakeRead_ readable,
    // which wakes the I/O thread out of poll().
    std::mutex wakeMutex_;
    int wakeRead_ = -1;
    int wakeWrite_ = -1;

    // Shared between the I/O thread and the UI thread; always accessed under mutex_.
    mutable std::mutex mutex_;
    std::deque<CommsLine> lines_;
    std::vector<CommsLink> linkInfos_;
    std::deque<std::string> outgoing_;
    bool listening_ = false;
    std::string listenText_;
};