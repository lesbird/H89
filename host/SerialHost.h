#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace h89 {

class Machine;

// The host end of one of the machine's serial ports: nothing, a file that
// receives what the H89 sends (for the printer), or a TCP connection, either
// listening for one client ("telnet localhost 8089") or connecting out.
// Everything is non-blocking; call pump() regularly (e.g. once a frame).
class SerialHost {
public:
    SerialHost() = default;
    ~SerialHost() { close(); }
    SerialHost(const SerialHost&) = delete;
    SerialHost& operator=(const SerialHost&) = delete;

    // Parses "file:PATH", "listen:PORT" or "connect:HOST:PORT" (a bare path
    // means file:). Returns false and sets `error` if it can't be opened.
    bool open(const std::string& spec, std::string* error = nullptr);
    bool openFile(const std::string& path, std::string* error = nullptr);
    bool listen(int port, std::string* error = nullptr);
    bool connect(const std::string& host, int port, std::string* error = nullptr);
    void close();

    bool active() const { return mode_ != Mode::None; }
    // Whether a peer is attached: a file always is; a listener once a client
    // connects.
    bool connected() const;
    // "printer.txt", "listening on port 8089", "connected to host:23", ...
    std::string description() const;
    const std::string& spec() const { return spec_; }

    // Called with each byte the H89 sends.
    void output(uint8_t byte);
    // The next byte received for the H89, or -1 (for Machine::serialSource).
    int next();
    // Accepts clients, moves data, and raises the port's CTS, DSR and DCD
    // while a TCP peer is connected.
    void pump(Machine& machine, int port);

private:
    enum class Mode { None, File, Listen, Connect };
    void dropClient();
    void flushPending();

    Mode mode_ = Mode::None;
    std::string spec_;
    std::string path_;
    std::FILE* file_ = nullptr;
    std::string host_;
    int port_ = 0;
    intptr_t listener_ = -1; // sockets (SOCKET on Windows)
    intptr_t client_ = -1;
    std::vector<uint8_t> inbound_;  // received, not yet given to the port
    size_t inboundPos_ = 0;
    std::vector<uint8_t> outbound_; // sent by the H89, not yet written
    bool lastConnected_ = false;
};

} // namespace h89
