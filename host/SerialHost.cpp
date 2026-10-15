#include "SerialHost.h"

#include "Machine.h"

#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace h89 {

namespace {

constexpr intptr_t kNoSocket = -1;

#if defined(_WIN32)
SOCKET native(intptr_t s) { return static_cast<SOCKET>(s); }
#else
int native(intptr_t s) { return static_cast<int>(s); }
#endif

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

bool netInit() {
#if defined(_WIN32)
    static bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
#else
    return true;
#endif
}

void closeSocket(intptr_t s) {
    if (s == kNoSocket) return;
#if defined(_WIN32)
    closesocket(native(s));
#else
    ::close(native(s));
#endif
}

bool setNonBlocking(intptr_t s) {
#if defined(_WIN32)
    u_long on = 1;
    return ioctlsocket(native(s), FIONBIO, &on) == 0;
#else
    int flags = fcntl(native(s), F_GETFL, 0);
    return flags >= 0 && fcntl(native(s), F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool wouldBlock() {
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

std::string lastSocketError() {
#if defined(_WIN32)
    return "socket error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

} // namespace

bool SerialHost::open(const std::string& spec, std::string* error) {
    auto rest = [&](size_t n) { return spec.substr(n); };
    if (spec.rfind("listen:", 0) == 0) {
        bool ok = listen(std::atoi(rest(7).c_str()), error);
        if (ok) spec_ = spec;
        return ok;
    }
    if (spec.rfind("serial:", 0) == 0) {
        close();
        SerialSettings settings;
        auto port = std::make_unique<SerialPort>();
        if (!SerialSettings::parse(rest(7), settings, error) || !port->open(settings, error)) return false;
        serialPort_ = std::move(port);
        serialSettings_ = settings;
        mode_ = Mode::Serial;
        spec_ = spec;
        return true;
    }
    if (spec.rfind("connect:", 0) == 0) {
        std::string target = rest(8);
        size_t colon = target.rfind(':');
        if (colon == std::string::npos) {
            setError(error, "use connect:HOST:PORT");
            return false;
        }
        bool ok = connect(target.substr(0, colon), std::atoi(target.substr(colon + 1).c_str()), error);
        if (ok) spec_ = spec;
        return ok;
    }
    bool ok = openFile(spec.rfind("file:", 0) == 0 ? rest(5) : spec, error);
    if (ok) spec_ = spec;
    return ok;
}

bool SerialHost::openFile(const std::string& path, std::string* error) {
    close();
    file_ = std::fopen(path.c_str(), "ab");
    if (!file_) {
        setError(error, "cannot open " + path + ": " + std::strerror(errno));
        return false;
    }
    mode_ = Mode::File;
    path_ = path;
    spec_ = "file:" + path;
    return true;
}

bool SerialHost::listen(int port, std::string* error) {
    close();
    if (!netInit() || port <= 0 || port > 65535) {
        setError(error, "bad port " + std::to_string(port));
        return false;
    }
    intptr_t s = static_cast<intptr_t>(::socket(AF_INET, SOCK_STREAM, 0));
    if (s == kNoSocket) {
        setError(error, lastSocketError());
        return false;
    }
    int yes = 1;
    setsockopt(native(s), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // local connections only
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(native(s), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        ::listen(native(s), 1) != 0 || !setNonBlocking(s)) {
        setError(error, "cannot listen on port " + std::to_string(port) + ": " + lastSocketError());
        closeSocket(s);
        return false;
    }
    listener_ = s;
    mode_ = Mode::Listen;
    port_ = port;
    spec_ = "listen:" + std::to_string(port);
    return true;
}

bool SerialHost::connect(const std::string& host, int port, std::string* error) {
    close();
    if (!netInit()) {
        setError(error, "networking is unavailable");
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found) {
        setError(error, "cannot find host " + host);
        return false;
    }
    intptr_t s = kNoSocket;
    for (addrinfo* a = found; a; a = a->ai_next) {
        s = static_cast<intptr_t>(::socket(a->ai_family, a->ai_socktype, a->ai_protocol));
        if (s == kNoSocket) continue;
        if (::connect(native(s), a->ai_addr, static_cast<socklen_t>(a->ai_addrlen)) == 0) break;
        closeSocket(s);
        s = kNoSocket;
    }
    freeaddrinfo(found);
    if (s == kNoSocket || !setNonBlocking(s)) {
        setError(error, "cannot connect to " + host + ":" + std::to_string(port) + ": " + lastSocketError());
        closeSocket(s);
        return false;
    }
    int yes = 1;
    setsockopt(native(s), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof yes);
    client_ = s;
    mode_ = Mode::Connect;
    host_ = host;
    port_ = port;
    spec_ = "connect:" + host + ":" + std::to_string(port);
    return true;
}

void SerialHost::close() {
    if (file_) std::fclose(file_);
    file_ = nullptr;
    dropClient();
    closeSocket(listener_);
    listener_ = kNoSocket;
    serialPort_.reset();
    mode_ = Mode::None;
    spec_.clear();
}

void SerialHost::dropClient() {
    closeSocket(client_);
    client_ = kNoSocket;
    inbound_.clear();
    inboundPos_ = 0;
    outbound_.clear();
    if (mode_ == Mode::Connect) mode_ = Mode::None; // an outgoing link ends with its connection
}

bool SerialHost::connected() const {
    return mode_ == Mode::File || mode_ == Mode::Serial || client_ != kNoSocket;
}

std::string SerialHost::description() const {
    switch (mode_) {
    case Mode::None: return "not connected";
    case Mode::File: return path_;
    case Mode::Listen:
        return client_ != kNoSocket ? "client connected on port " + std::to_string(port_)
                                    : "listening on port " + std::to_string(port_);
    case Mode::Connect: return "connected to " + host_ + ":" + std::to_string(port_);
    case Mode::Serial:
        return serialSettings_.device + " at " + std::to_string(serialSettings_.baud) + " " +
               serialSettings_.format() + (serialPort_->isOpen() ? "" : " (gone)");
    }
    return "";
}

void SerialHost::output(uint8_t byte) {
    if (mode_ == Mode::File) {
        std::fputc(byte, file_);
        std::fflush(file_);
    } else if (client_ != kNoSocket || (mode_ == Mode::Serial && serialPort_->isOpen())) {
        outbound_.push_back(byte);
        flushPending();
    }
}

void SerialHost::flushPending() {
    if (mode_ == Mode::Serial) {
        int n = outbound_.empty() ? 0 : serialPort_->write(outbound_.data(), outbound_.size());
        if (n > 0) outbound_.erase(outbound_.begin(), outbound_.begin() + n);
        else if (n < 0) outbound_.clear(); // the port went away
        return;
    }
    while (!outbound_.empty() && client_ != kNoSocket) {
        auto n = ::send(native(client_), reinterpret_cast<const char*>(outbound_.data()),
                        static_cast<int>(outbound_.size()), 0);
        if (n > 0) {
            outbound_.erase(outbound_.begin(), outbound_.begin() + n);
        } else {
            if (n < 0 && wouldBlock()) return;
            dropClient();
        }
    }
}

int SerialHost::next() {
    if (inboundPos_ >= inbound_.size()) return -1;
    return inbound_[inboundPos_++];
}

void SerialHost::pump(Machine& machine, int port) {
    if (mode_ == Mode::Listen && client_ == kNoSocket) {
        intptr_t c = static_cast<intptr_t>(::accept(native(listener_), nullptr, nullptr));
        if (c != kNoSocket) {
            setNonBlocking(c);
            int yes = 1;
            setsockopt(native(c), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes),
                       sizeof yes);
            client_ = c;
        }
    }
    if (client_ != kNoSocket) {
        flushPending();
        uint8_t buf[512];
        if (inboundPos_ == inbound_.size()) inbound_.clear(), inboundPos_ = 0;
        while (client_ != kNoSocket && inbound_.size() - inboundPos_ < 4096) {
            auto n = ::recv(native(client_), reinterpret_cast<char*>(buf), static_cast<int>(sizeof buf), 0);
            if (n > 0) {
                inbound_.insert(inbound_.end(), buf, buf + n);
            } else {
                if (n < 0 && wouldBlock()) break;
                dropClient(); // closed by the other end
            }
        }
    }
    if (mode_ == Mode::Serial && serialPort_->isOpen()) {
        flushPending();
        uint8_t buf[512];
        if (inboundPos_ == inbound_.size()) inbound_.clear(), inboundPos_ = 0;
        while (inbound_.size() - inboundPos_ < 4096) {
            int n = serialPort_->read(buf, sizeof buf);
            if (n <= 0) break;
            inbound_.insert(inbound_.end(), buf, buf + n);
        }
    }
    // A TCP peer or a host serial port raises CTS, DSR and DCD like a modem.
    // A file leaves them low, which Heath's printer handshake reads as "not
    // busy"; so does a serial port on the printer port, for the same reason.
    bool now = client_ != kNoSocket ||
               (mode_ == Mode::Serial && serialPort_->isOpen() && port != Machine::kPrinter);
    if (now != lastConnected_) {
        machine.setSerialConnected(port, now);
        lastConnected_ = now;
    }
}

} // namespace h89
