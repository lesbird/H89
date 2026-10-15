// h8dtool: lists, extracts, inserts and deletes files in H17 (.h8d) and
// H37 (.h37, .imd) disk images holding HDOS 2.0 or Heath CP/M 2.2.
//
//   h8dtool list image.h8d
//   h8dtool get  image.h8d [-d dir] [file...]   (no files: extract them all)
//   h8dtool put  image.h8d [--replace] hostfile...
//   h8dtool rm   image.h8d file...

#include "media/DiskFiles.h"
#include "media/H17Disk.h"
#include "media/SoftDisk.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

[[noreturn]] void usage() {
    std::fprintf(stderr,
                 "usage: h8dtool list image.h8d\n"
                 "       h8dtool get  image.h8d [-d dir] [file...]\n"
                 "       h8dtool put  image.h8d [--replace] hostfile...\n"
                 "       h8dtool rm   image.h8d file...\n");
    std::exit(2);
}

int fail(const std::string& message) {
    std::fprintf(stderr, "h8dtool: %s\n", message.c_str());
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) usage();
    std::string command = argv[1];
    std::string image = argv[2];
    std::vector<std::string> args(argv + 3, argv + argc);

    bool writing = command == "put" || command == "rm";
    if (!writing && command != "list" && command != "get") usage();

    // .h8d images are H17 disks; anything else is soft-sectored.
    std::string ext = fs::path(image).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    h89::H17Disk h17;
    h89::SoftDisk soft;
    std::string error;
    std::unique_ptr<h89::DiskFileSystem> fsys;
    if (ext == ".h8d") {
        if (!h17.load(image, !writing, &error)) return fail(error);
        fsys = h89::DiskFileSystem::open(h17, &error);
    } else {
        if (!soft.load(image, !writing, &error)) return fail(error);
        fsys = h89::DiskFileSystem::open(soft, &error);
    }
    if (!fsys) return fail(image + ": " + error);

    if (command == "list") {
        std::printf("%s\n\n", fsys->description().c_str());
        auto files = fsys->list();
        bool cpm = fsys->format() == h89::DiskFileSystem::Format::Cpm;
        for (const auto& f : files) {
            if (cpm) std::printf("%2d  %-12s %7u  %s\n", f.user, f.name.c_str(), f.size, f.flags.c_str());
            else std::printf("%-12s %7u  %-9s  %s\n", f.name.c_str(), f.size, f.date.c_str(), f.flags.c_str());
        }
        std::printf("\n%zu files, %u bytes free\n", files.size(), fsys->freeBytes());
        return 0;
    }

    if (command == "get") {
        fs::path dir = ".";
        std::vector<std::string> names;
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "-d" && i + 1 < args.size()) dir = args[++i];
            else names.push_back(args[i]);
        }
        std::error_code ec;
        fs::create_directories(dir, ec);
        int status = 0;
        for (const auto& f : fsys->list()) {
            bool wanted = names.empty();
            for (const auto& n : names) wanted = wanted || n == f.name;
            if (!wanted) continue;
            std::vector<uint8_t> data;
            if (!fsys->read(f, &data, &error)) {
                status = fail(error);
                continue;
            }
            fs::path out = dir;
            if (f.user != 0) out /= "user" + std::to_string(f.user);
            fs::create_directories(out, ec);
            out /= f.name;
            std::ofstream o(out, std::ios::binary);
            o.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!o) status = fail("cannot write " + out.string());
            else std::printf("%s\n", out.string().c_str());
        }
        return status;
    }

    if (command == "put") {
        bool replace = false;
        int status = 0;
        for (const auto& path : args) {
            if (path == "--replace") {
                replace = true;
                continue;
            }
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                status = fail("cannot open " + path);
                continue;
            }
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
            std::string name = fsys->diskName(path, &error);
            if (name.empty() || !fsys->write(name, data, replace, &error)) {
                status = fail(error);
                continue;
            }
            std::printf("%s -> %s\n", path.c_str(), name.c_str());
        }
        return status;
    }

    // rm
    int status = 0;
    for (const auto& name : args) {
        h89::DiskFile file;
        if (!fsys->find(name, &file)) {
            status = fail(name + " is not on this disk");
            continue;
        }
        if (!fsys->remove(file, &error)) status = fail(error);
    }
    return status;
}
