#include "SerialPort.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <IOKit/serial/ioss.h>
#endif
#endif

namespace h89 {

const int kCommonBaudRates[] = {300,   1200,  2400,   4800,   9600,   19200,  38400,
                                57600, 76800, 115200, 230400, 460800, 921600};
const size_t kCommonBaudRateCount = sizeof kCommonBaudRates / sizeof kCommonBaudRates[0];

// --- Settings ------------------------------------------------------------------

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isNumber(const std::string& f) {
    return !f.empty() && std::all_of(f.begin(), f.end(), [](unsigned char c) { return std::isdigit(c); });
}

// "8N1", "7E2" ...
bool isFormat(const std::string& f) {
    return f.size() == 3 && (f[0] == '7' || f[0] == '8') && std::strchr("NEOneo", f[1]) &&
           (f[2] == '1' || f[2] == '2');
}

} // namespace

bool SerialSettings::parse(const std::string& spec, SerialSettings& out, std::string* error) {
    // Settings are taken off the end, so a device name keeps any colons of
    // its own.
    std::vector<std::string> fields;
    std::string device = spec;
    for (size_t colon; (colon = device.rfind(':')) != std::string::npos;) {
        std::string f = device.substr(colon + 1);
        if (!isNumber(f) && !isFormat(f) && lower(f) != "rtscts" && lower(f) != "none") break;
        fields.insert(fields.begin(), f);
        device.erase(colon);
    }
    if (device.empty()) {
        if (error) *error = "no serial device named in \"" + spec + "\"";
        return false;
    }
    SerialSettings s;
    s.device = device;
    for (const auto& f : fields) {
        if (isFormat(f)) {
            s.dataBits = f[0] - '0';
            s.parity = static_cast<char>(std::toupper(static_cast<unsigned char>(f[1])));
            s.stopBits = f[2] - '0';
        } else if (isNumber(f)) {
            s.baud = std::atoi(f.c_str());
            if (s.baud < 50 || s.baud > 12'000'000) {
                if (error) *error = "unusable baud rate " + f;
                return false;
            }
        } else {
            s.rtsCts = lower(f) == "rtscts";
        }
    }
    out = s;
    return true;
}

std::string SerialSettings::format() const {
    return std::to_string(dataBits) + parity + std::to_string(stopBits);
}

std::string SerialSettings::spec() const {
    return device + ":" + std::to_string(baud) + ":" + format() + (rtsCts ? ":rtscts" : "");
}

// --- POSIX ---------------------------------------------------------------------

#if !defined(_WIN32)

namespace {

speed_t standardSpeed(int baud) {
    switch (baud) {
    case 50: return B50;
    case 75: return B75;
    case 110: return B110;
    case 134: return B134;
    case 150: return B150;
    case 200: return B200;
    case 300: return B300;
    case 600: return B600;
    case 1200: return B1200;
    case 1800: return B1800;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
#if defined(B460800)
    case 460800: return B460800;
#endif
#if defined(B921600)
    case 921600: return B921600;
#endif
    default: return 0;
    }
}

} // namespace

bool SerialPort::open(const SerialSettings& s, std::string* error) {
    close();
    auto fail = [&](const std::string& what) {
        if (error) *error = s.device + ": " + what + (errno ? std::string(" (") + std::strerror(errno) + ")" : "");
        close();
        return false;
    };
    errno = 0;
    fd_ = ::open(s.device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) return fail("cannot open");
    ioctl(fd_, TIOCEXCL); // keep other programs off it

    termios t{};
    if (tcgetattr(fd_, &t) != 0) return fail("is not a serial port");
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
    t.c_cflag |= s.dataBits == 7 ? CS7 : CS8;
    if (s.parity != 'N') t.c_cflag |= PARENB | (s.parity == 'O' ? PARODD : 0);
    if (s.stopBits == 2) t.c_cflag |= CSTOPB;
    if (s.rtsCts) t.c_cflag |= CRTSCTS;
    t.c_iflag &= ~(IXON | IXOFF | IXANY);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;

    speed_t speed = standardSpeed(s.baud);
#if defined(__APPLE__)
    // Any rate: set a standard one with termios, then the real one.
    cfsetspeed(&t, speed ? speed : B9600);
    if (tcsetattr(fd_, TCSANOW, &t) != 0) return fail("cannot set the line format");
    speed_t custom = static_cast<speed_t>(s.baud);
    errno = 0;
    if (!speed && ioctl(fd_, IOSSIOSPEED, &custom) != 0)
        return fail(std::to_string(s.baud) + " baud is not supported");
#else
    errno = 0;
    if (!speed) return fail(std::to_string(s.baud) + " baud is not supported");
    cfsetspeed(&t, speed);
    if (tcsetattr(fd_, TCSANOW, &t) != 0) return fail("cannot set the line format");
#endif
    int lines = TIOCM_DTR | (s.rtsCts ? 0 : TIOCM_RTS);
    ioctl(fd_, TIOCMBIS, &lines);
    tcflush(fd_, TCIOFLUSH);
    return true;
}

void SerialPort::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

bool SerialPort::isOpen() const { return fd_ >= 0; }

int SerialPort::read(uint8_t* buf, size_t size) {
    if (fd_ < 0) return -1;
    ssize_t n = ::read(fd_, buf, size);
    if (n > 0) return static_cast<int>(n);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        close();
        return -1;
    }
    // Nothing to read, or the line hung up (an adapter unplugged reads as
    // end of file on some systems): ask.
    pollfd p{fd_, POLLIN, 0};
    if (poll(&p, 1, 0) > 0 && (p.revents & (POLLHUP | POLLERR | POLLNVAL))) {
        close();
        return -1;
    }
    return 0;
}

int SerialPort::write(const uint8_t* buf, size_t size) {
    if (fd_ < 0) return -1;
    ssize_t n = ::write(fd_, buf, size);
    if (n >= 0) return static_cast<int>(n);
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    close();
    return -1;
}

void SerialPort::setBreak(bool on) {
    if (fd_ >= 0) ioctl(fd_, on ? TIOCSBRK : TIOCCBRK);
}

std::vector<std::string> SerialPort::list() {
    std::vector<std::string> ports;
    DIR* dir = opendir("/dev");
    if (!dir) return ports;
    while (dirent* e = readdir(dir)) {
        std::string name = e->d_name;
#if defined(__APPLE__)
        // Call-out devices; skip the built-in ones that never lead anywhere.
        bool take = name.rfind("cu.", 0) == 0 && name != "cu.Bluetooth-Incoming-Port" && name != "cu.debug-console";
#else
        bool take = name.rfind("ttyUSB", 0) == 0 || name.rfind("ttyACM", 0) == 0 || name.rfind("ttyS", 0) == 0;
#endif
        if (take) ports.push_back("/dev/" + name);
    }
    closedir(dir);
    std::sort(ports.begin(), ports.end());
    return ports;
}

// --- Windows -------------------------------------------------------------------

#else

namespace {

std::string lastError() {
    DWORD code = GetLastError();
    char buf[256] = "";
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf, sizeof buf,
                   nullptr);
    std::string s = buf;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.')) s.pop_back();
    return s.empty() ? "error " + std::to_string(code) : s;
}

} // namespace

bool SerialPort::open(const SerialSettings& s, std::string* error) {
    close();
    auto fail = [&](const std::string& what) {
        if (error) *error = s.device + ": " + what + " (" + lastError() + ")";
        close();
        return false;
    };
    // COM10 and up only open through the \\.\ namespace; it works for all.
    std::string path = s.device.rfind("\\\\", 0) == 0 ? s.device : "\\\\.\\" + s.device;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail("cannot open");
    handle_ = h;

    DCB dcb{};
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb)) return fail("is not a serial port");
    dcb.BaudRate = static_cast<DWORD>(s.baud);
    dcb.ByteSize = static_cast<BYTE>(s.dataBits);
    dcb.Parity = s.parity == 'E' ? EVENPARITY : s.parity == 'O' ? ODDPARITY : NOPARITY;
    dcb.StopBits = s.stopBits == 2 ? TWOSTOPBITS : ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = s.parity != 'N';
    dcb.fOutxCtsFlow = s.rtsCts;
    dcb.fRtsControl = s.rtsCts ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_ENABLE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) return fail(std::to_string(s.baud) + " baud " + s.format() + " is not supported");
    SetupComm(h, 1 << 16, 1 << 16);

    // Reads return at once with whatever has arrived; writes give up after
    // a moment (when flow control holds them), reporting what went.
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.WriteTotalTimeoutConstant = 10;
    SetCommTimeouts(h, &timeouts);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
}

void SerialPort::close() {
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
}

bool SerialPort::isOpen() const { return handle_ != nullptr; }

int SerialPort::read(uint8_t* buf, size_t size) {
    if (!handle_) return -1;
    DWORD n = 0;
    if (!ReadFile(static_cast<HANDLE>(handle_), buf, static_cast<DWORD>(size), &n, nullptr)) {
        close();
        return -1;
    }
    return static_cast<int>(n);
}

int SerialPort::write(const uint8_t* buf, size_t size) {
    if (!handle_) return -1;
    DWORD n = 0;
    if (!WriteFile(static_cast<HANDLE>(handle_), buf, static_cast<DWORD>(size), &n, nullptr) &&
        GetLastError() != ERROR_TIMEOUT) {
        close();
        return -1;
    }
    return static_cast<int>(n);
}

void SerialPort::setBreak(bool on) {
    if (!handle_) return;
    if (on) SetCommBreak(static_cast<HANDLE>(handle_));
    else ClearCommBreak(static_cast<HANDLE>(handle_));
}

std::vector<std::string> SerialPort::list() {
    std::vector<std::string> ports;
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return ports;
    for (DWORD i = 0;; ++i) {
        char name[256], data[256];
        DWORD nameLen = sizeof name, dataLen = sizeof data, type = 0;
        if (RegEnumValueA(key, i, name, &nameLen, nullptr, &type, reinterpret_cast<BYTE*>(data), &dataLen) !=
            ERROR_SUCCESS)
            break;
        if (type == REG_SZ) ports.emplace_back(data, strnlen(data, dataLen));
    }
    RegCloseKey(key);
    // COM2 before COM10.
    std::sort(ports.begin(), ports.end(), [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    return ports;
}

#endif

} // namespace h89
