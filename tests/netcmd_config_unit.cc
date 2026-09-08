#include "netcmd_unit.h"
#include "src/cmd/server_tail.cc"
#include <filesystem>
#include <thread>
#include <barrier>
#include <sys/stat.h>

namespace {
std::atomic<bool> rewrite_arm{false};
std::atomic<unsigned> rewrite_opens{0};
std::barrier rewrite_opened(2);
ino_t rewrite_inodes[2]{};
void observe_open(int fd) {
    if (!rewrite_arm.load()) return;
    const auto index = rewrite_opens.fetch_add(1);
    check(index < 2, "exactly two rewrite opens");
    struct stat info{}; check(::fstat(fd, &info) == 0, "rewrite inode identified");
    rewrite_inodes[index] = info.st_ino;
    rewrite_opened.arrive_and_wait(); // both files are open before either writer may write/rename
}
}
extern "C" int __real_mkstemp(char*);
extern "C" FILE* __real_fopen(const char*, const char*);
extern "C" int __wrap_mkstemp(char* path) {
    const int fd = __real_mkstemp(path);
    if (fd >= 0 && std::strstr(path, ".rewrite.")) observe_open(fd);
    return fd;
}
extern "C" FILE* __wrap_fopen(const char* path, const char* mode) {
    FILE* file = __real_fopen(path, mode);
    if (file && std::strstr(path, ".rewrite.")) observe_open(::fileno(file));
    return file;
}

void test_config_rewrite() {
    check(tomo::config_quote("77") == "77", "simple scalar keeps its original spelling");
    const std::string value = "pass with spaces\n\r\t\"'\\tail";
    std::vector<std::string> words;
    check(tomo::cfg_split_args(tomo::config_quote(value).c_str(), words), "quoted config parses");
    check(words.size() == 1 && words[0] == value, "config string round trip");
    // The harness binds a server whose config path is initialized below by its friend fixture.
    const char* path = tomo::command_server()->cfg().conf_path;
    std::FILE* original = std::fopen(path, "w"); check(original, "create original config");
    std::fputs("user reader on nopass ~* +get\nload \"/tmp/recovery source.tomo\"\nrequirepass old\n", original);
    std::fclose(original);
    std::string error;
    check(tomo::config_rewrite(error), "first rewrite");
    std::ifstream in(path); std::string body((std::istreambuf_iterator<char>(in)), {});
    check(body.find("user reader on nopass ~* +get\n") != std::string::npos, "inline ACL survived rewrite");
    check(body.find("load \"/tmp/recovery source.tomo\"\n") != std::string::npos, "recovery source survived rewrite");
    std::vector<std::string> loaded;
    check(tomo::load_conf_file(path, loaded), "rewritten config loads");
    auto password = std::find(loaded.begin(), loaded.end(), "--requirepass");
    check(password != loaded.end() && ++password != loaded.end() && *password == value, "password preserved as one token");
    rewrite_arm.store(true);
    bool success[2]{};
    std::thread a([&] { std::string err; success[0] = tomo::config_rewrite(err); });
    std::thread b([&] { std::string err; success[1] = tomo::config_rewrite(err); });
    a.join(); b.join(); rewrite_arm.store(false);
    check(rewrite_opens.load() == 2, "two rewrite opens overlapped before either rename");
    check(rewrite_inodes[0] != rewrite_inodes[1], "concurrent writers own distinct temporary inodes");
    check(success[0] && success[1], "both concurrent rewrites completed");
    std::ifstream after(path); std::string final((std::istreambuf_iterator<char>(after)), {});
    check(final == body, "concurrent rewrites preserve exact complete bytes");
}
