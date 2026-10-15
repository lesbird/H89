#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace h89 {

// How to open a host serial port.
struct SerialSettings {
    std::string device; // /dev/cu.usbserial-1410, /dev/ttyUSB0, COM3
    int baud = 9600;    // any rate the adapter takes
    int dataBits = 8;   // 7 or 8
    char parity = 'N';  // N, E or O
    int stopBits = 1;   // 1 or 2
    bool rtsCts = false; // hardware flow control

    // "DEVICE[:BAUD[:FORMAT[:rtscts]]]", e.g. "COM3:115200:8N1" or
    // "/dev/cu.usbserial-1410:38400:7E1:rtscts". False (with `error`) if
    // malformed.
    static bool parse(const std::string& spec, SerialSettings& out, std::string* error = nullptr);
    std::string spec() const;
    std::string format() const; // "8N1"
};

// Rates offered in menus; any other rate the adapter accepts works too.
extern const int kCommonBaudRates[];
extern const size_t kCommonBaudRateCount;

// A host serial port (a USB serial adapter, say), raw and non-blocking.
class SerialPort {
public:
    SerialPort() = default;
    ~SerialPort() { close(); }
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    bool open(const SerialSettings& settings, std::string* error = nullptr);
    void close();
    bool isOpen() const;

    // Return the number of bytes moved (0 if none), or -1 if the port failed
    // (unplugged, say); it is then closed.
    int read(uint8_t* buf, size_t size);
    int write(const uint8_t* buf, size_t size);

    void setBreak(bool on);

    // The serial ports on this computer, by the names open() takes.
    static std::vector<std::string> list();

private:
#if defined(_WIN32)
    void* handle_ = nullptr; // HANDLE; INVALID_HANDLE_VALUE is never stored
#else
    int fd_ = -1;
#endif
};

} // namespace h89
