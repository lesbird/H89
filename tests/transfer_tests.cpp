// File transfers end to end: each sender talks to its receiver over a
// simulated serial line (which can corrupt bytes) with a simulated clock.

#include "check.h"
#include "FileTransfer.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using h89::FileTransfer;

namespace {

fs::path scratch(const char* name) {
    fs::path dir = fs::temp_directory_path() / "h89_transfer_tests" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::vector<uint8_t> randomBytes(size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = static_cast<uint8_t>(rng());
    // Plenty of the bytes ZMODEM has to escape.
    for (size_t i = 0; i < n; i += 37) v[i] = (i / 37) % 2 ? 0x18 : 0x11;
    return v;
}

void writeFile(const fs::path& p, const std::vector<uint8_t>& data) {
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(data.data()),
                                             static_cast<std::streamsize>(data.size()));
}

std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

struct Line {
    // Bytes per 10 ms tick each way (115 is about 115,200 baud).
    size_t rate = 115;
    // Corrupts every Nth byte from the sender (0: never).
    size_t corruptEvery = 0;
    // Drops bytes from the receiver that this returns true for.
    std::function<bool(uint8_t)> dropToSender;
    // Where a ZMODEM receiver saves what it's offered.
    fs::path saveDir;
};

// Runs both ends until they finish (or 10 simulated minutes pass).
void run(FileTransfer& sender, FileTransfer& receiver, const Line& line) {
    auto t = FileTransfer::Clock::now();
    size_t sent = 0;
    for (int tick = 0; tick < 60'000 && !(sender.finished() && receiver.finished()); ++tick) {
        t += std::chrono::milliseconds(10);
        std::vector<uint8_t> a, b;
        sender.poll(t, a, line.rate);
        for (auto& c : a)
            if (line.corruptEvery && ++sent % line.corruptEvery == 0) c ^= 0x55;
        if (!a.empty()) receiver.received(a.data(), a.size(), t);
        if (receiver.state() == FileTransfer::State::NeedsSavePath)
            receiver.setSavePath((line.saveDir / receiver.offeredName()).string());
        receiver.poll(t, b, line.rate);
        std::vector<uint8_t> kept;
        for (uint8_t c : b)
            if (!line.dropToSender || !line.dropToSender(c)) kept.push_back(c);
        if (!kept.empty()) sender.received(kept.data(), kept.size(), t);
    }
}

// XMODEM pads to whole 128-byte blocks with ^Z.
bool xmodemMatches(const std::vector<uint8_t>& sent, const std::vector<uint8_t>& got, size_t block = 128) {
    if (got.size() % 128 || got.size() < sent.size() || got.size() - sent.size() >= block) return false;
    for (size_t i = 0; i < got.size(); ++i)
        if (got[i] != (i < sent.size() ? sent[i] : 0x1A)) return false;
    return true;
}

void xmodemCase(size_t size, bool oneK, const Line& line, size_t block = 128) {
    fs::path dir = scratch("xmodem");
    auto data = randomBytes(size, static_cast<unsigned>(size));
    writeFile(dir / "in.bin", data);
    auto sender = h89::xmodemSend((dir / "in.bin").string(), oneK);
    auto receiver = h89::xmodemReceive((dir / "out.bin").string());
    run(*sender, *receiver, line);
    CHECK_EQ(sender->state() == FileTransfer::State::Done, true);
    CHECK_EQ(receiver->state() == FileTransfer::State::Done, true);
    if (sender->state() != FileTransfer::State::Done || receiver->state() != FileTransfer::State::Done)
        std::printf("  size %zu: sender \"%s\", receiver \"%s\"\n", size, sender->status().c_str(),
                    receiver->status().c_str());
    CHECK_EQ(xmodemMatches(data, readFile(dir / "out.bin"), block), true);
}

} // namespace

TEST_CASE("XMODEM with CRCs, any size") {
    for (size_t size : {0, 1, 127, 128, 129, 1000, 5000}) xmodemCase(size, false, {});
}

TEST_CASE("XMODEM falls back to checksums") {
    Line line;
    line.dropToSender = [](uint8_t c) { return c == 'C'; }; // a sender that ignores CRC requests
    xmodemCase(3000, false, line);
}

TEST_CASE("XMODEM-1K") {
    xmodemCase(5000, true, {}, 1024); // 4 x 1K, then 904 bytes in a 1K block
    xmodemCase(4100, true, {});       // the last 4 bytes go in a 128-byte block
}

TEST_CASE("XMODEM recovers from line noise") {
    Line line;
    line.corruptEvery = 700; // about one 128-byte block in five
    xmodemCase(20000, false, line);
    line.corruptEvery = 2500; // 1K blocks: noisier than this, none would get through
    xmodemCase(20000, true, line, 1024);
}

TEST_CASE("ZMODEM moves a file exactly") {
    for (size_t corrupt : {0, 2500}) {
        fs::path dir = scratch("zmodem"), out = scratch("zmodem_out");
        auto data = randomBytes(100'000, 7);
        writeFile(dir / "game.com", data);
        auto sender = h89::zmodemSend({(dir / "game.com").string()});
        auto receiver = h89::zmodemReceive();
        Line line;
        line.corruptEvery = corrupt;
        line.saveDir = out;
        run(*sender, *receiver, line);
        CHECK_EQ(sender->state() == FileTransfer::State::Done, true);
        CHECK_EQ(receiver->state() == FileTransfer::State::Done, true);
        if (receiver->state() != FileTransfer::State::Done)
            std::printf("  noise %zu: sender \"%s\", receiver \"%s\"\n", corrupt, sender->status().c_str(),
                        receiver->status().c_str());
        CHECK_EQ(receiver->savedFiles().size(), size_t{1});
        CHECK_EQ(sender->bytesDone(), uint64_t{100'000});
        CHECK_EQ(receiver->bytesDone(), uint64_t{100'000});
        CHECK_EQ(fs::exists(out / "GAME.COM"), true);
        CHECK_EQ(readFile(out / "GAME.COM") == data, true);
    }
}

TEST_CASE("ZMODEM sends several files, including an empty one") {
    fs::path dir = scratch("zmulti"), out = scratch("zmulti_out");
    std::vector<std::vector<uint8_t>> files = {randomBytes(3000, 1), {}, randomBytes(70'000, 2)};
    std::vector<std::string> paths;
    const char* names[] = {"one.txt", "empty.dat", "a long name.asm"};
    for (int i = 0; i < 3; ++i) {
        writeFile(dir / names[i], files[static_cast<size_t>(i)]);
        paths.push_back((dir / names[i]).string());
    }
    auto sender = h89::zmodemSend(paths);
    auto receiver = h89::zmodemReceive();
    Line line;
    line.saveDir = out;
    run(*sender, *receiver, line);
    CHECK_EQ(sender->state() == FileTransfer::State::Done, true);
    CHECK_EQ(receiver->state() == FileTransfer::State::Done, true);
    CHECK_EQ(receiver->savedFiles().size(), size_t{3});
    CHECK_EQ(readFile(out / "ONE.TXT") == files[0], true);
    CHECK_EQ(fs::exists(out / "EMPTY.DAT") && readFile(out / "EMPTY.DAT").empty(), true);
    CHECK_EQ(readFile(out / "ALONGNAM.ASM") == files[2], true);
}

TEST_CASE("a skipped ZMODEM file isn't saved") {
    fs::path dir = scratch("zskip");
    writeFile(dir / "a.bin", randomBytes(5000, 3));
    auto sender = h89::zmodemSend({(dir / "a.bin").string()});
    auto receiver = h89::zmodemReceive();
    auto t = FileTransfer::Clock::now();
    for (int tick = 0; tick < 6000 && !(sender->finished() && receiver->finished()); ++tick) {
        t += std::chrono::milliseconds(10);
        std::vector<uint8_t> a, b;
        sender->poll(t, a, 115);
        receiver->received(a.data(), a.size(), t);
        if (receiver->state() == FileTransfer::State::NeedsSavePath) receiver->setSavePath("");
        receiver->poll(t, b, 115);
        sender->received(b.data(), b.size(), t);
    }
    CHECK_EQ(sender->state() == FileTransfer::State::Done, true);
    CHECK_EQ(receiver->state() == FileTransfer::State::Done, true);
    CHECK_EQ(receiver->savedFiles().empty(), true);
}

TEST_CASE("cancelling stops the other end") {
    fs::path dir = scratch("cancel"), out = scratch("cancel_out");
    writeFile(dir / "big.bin", randomBytes(200'000, 4));
    for (int protocol = 0; protocol < 2; ++protocol) {
        auto sender = protocol ? h89::zmodemSend({(dir / "big.bin").string()})
                               : h89::xmodemSend((dir / "big.bin").string(), false);
        auto receiver = protocol ? h89::zmodemReceive() : h89::xmodemReceive((out / "x.bin").string());
        auto t = FileTransfer::Clock::now();
        for (int tick = 0; tick < 3000 && !receiver->finished(); ++tick) {
            t += std::chrono::milliseconds(10);
            std::vector<uint8_t> a, b;
            if (tick == 300) sender->cancel(a);
            else sender->poll(t, a, 115);
            receiver->received(a.data(), a.size(), t);
            if (receiver->state() == FileTransfer::State::NeedsSavePath)
                receiver->setSavePath((out / receiver->offeredName()).string());
            receiver->poll(t, b, 115);
            sender->received(b.data(), b.size(), t);
        }
        CHECK_EQ(sender->state() == FileTransfer::State::Cancelled, true);
        CHECK_EQ(receiver->state() == FileTransfer::State::Failed, true);
        CHECK_EQ(receiver->status(), std::string("Cancelled by the computer"));
        CHECK_EQ(receiver->savedFiles().empty(), true);
    }
    CHECK_EQ(fs::exists(out / "x.bin"), false); // the partial download is removed
    CHECK_EQ(fs::exists(out / "BIG.BIN"), false);
}

TEST_CASE("text is sent with the chosen line endings, up to ^Z") {
    fs::path dir = scratch("text");
    std::string text = "10 PRINT \"HI\"\n20 GOTO 10\r\n30 END\rLAST\x1aJUNK";
    writeFile(dir / "prog.bas", std::vector<uint8_t>(text.begin(), text.end()));
    struct {
        h89::TextSendOptions::LineEnding ending;
        std::string want;
    } cases[] = {
        {h89::TextSendOptions::LineEnding::CR, "10 PRINT \"HI\"\r20 GOTO 10\r30 END\rLAST"},
        {h89::TextSendOptions::LineEnding::CRLF, "10 PRINT \"HI\"\r\n20 GOTO 10\r\n30 END\r\nLAST"},
        {h89::TextSendOptions::LineEnding::LF, "10 PRINT \"HI\"\n20 GOTO 10\n30 END\nLAST"},
        {h89::TextSendOptions::LineEnding::AsIs, "10 PRINT \"HI\"\n20 GOTO 10\r\n30 END\rLAST"},
    };
    for (const auto& c : cases) {
        h89::TextSendOptions options;
        options.lineEnding = c.ending;
        auto sender = h89::sendTextFile((dir / "prog.bas").string(), options);
        CHECK_EQ(sender->echoesToScreen(), true);
        std::vector<uint8_t> out;
        auto t = FileTransfer::Clock::now();
        for (int i = 0; i < 100 && !sender->finished(); ++i) sender->poll(t, out, 8);
        CHECK_EQ(std::string(out.begin(), out.end()), c.want);
    }
}

TEST_CASE("text pacing and XOFF") {
    fs::path dir = scratch("pace");
    std::string text = "AB\nCD\n";
    writeFile(dir / "t.txt", std::vector<uint8_t>(text.begin(), text.end()));
    h89::TextSendOptions options;
    options.charDelayMs = 10;
    options.lineDelayMs = 100;
    auto sender = h89::sendTextFile((dir / "t.txt").string(), options);
    // Each character waits 10 ms; each line end another 100.
    std::vector<uint8_t> out;
    std::vector<int> when;
    auto t0 = FileTransfer::Clock::now();
    for (int ms = 0; ms < 1000 && !sender->finished(); ++ms) {
        size_t before = out.size();
        sender->poll(t0 + std::chrono::milliseconds(ms), out, 100);
        for (size_t i = before; i < out.size(); ++i) when.push_back(ms);
    }
    CHECK_EQ(std::string(out.begin(), out.end()), std::string("AB\rCD\r"));
    CHECK_EQ(when.size(), size_t{6});
    if (when.size() == 6) {
        CHECK_EQ(when[1], 10);  // B
        CHECK_EQ(when[2], 20);  // CR
        CHECK_EQ(when[3], 130); // C, after the line delay
    }
    // XOFF from the computer holds the rest until XON.
    auto paused = h89::sendTextFile((dir / "t.txt").string(), {});
    uint8_t xoff = 0x13, xon = 0x11;
    std::vector<uint8_t> got;
    paused->poll(t0, got, 2);
    paused->received(&xoff, 1, t0);
    paused->poll(t0, got, 100);
    CHECK_EQ(got.size(), size_t{2});
    paused->received(&xon, 1, t0);
    paused->poll(t0, got, 100);
    CHECK_EQ(got.size(), size_t{6});
}

TEST_CASE("CP/M names") {
    CHECK_EQ(h89::cpmFileName("/tmp/hello world.asm"), std::string("HELLOWOR.ASM"));
    CHECK_EQ(h89::cpmFileName("game.com"), std::string("GAME.COM"));
    CHECK_EQ(h89::cpmFileName("README"), std::string("README"));
    CHECK_EQ(h89::cpmFileName("archive.tar.gz"), std::string("ARCHIVET.GZ"));
    CHECK_EQ(h89::cpmFileName(".profile"), std::string("PROFILE"));
}

int main() { return check::runAll(); }
