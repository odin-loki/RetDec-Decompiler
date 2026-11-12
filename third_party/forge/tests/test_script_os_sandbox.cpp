// ForgeScript: capability flags are enforced by the OS sandbox (forge/os/sandbox.hpp), not only by the
// interpreter. Each case runs in a forked child because the sandbox is process-wide and one-way.
#include <forge/script/script.hpp>
#include "check.hpp"
#include <cstdio>
#include <string>
#if defined(__linux__)
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

using namespace forge;

// Returns the child's exit code: 0 = expectations met.
template <class F> static int in_child(F f) {
    pid_t p = ::fork(); if (p == 0) { ::_exit(f()); }
    int st = 0; ::waitpid(p, &st, 0); return WIFEXITED(st) ? WEXITSTATUS(st) : 100;
}

TEST(os_sandbox_no_net_blocks_sockets) {
    int rc = in_child([] {
        script::options o; o.allow_net = false; o.os_sandbox = script::options::os_sandbox_mode::require;
        script::engine e(o);
        if (e.sandbox_status().rfind("seccomp", 0) != 0) return 2;
        int s = ::socket(AF_INET, SOCK_STREAM, 0);   // native code in the same process is denied too
        if (s >= 0) return 3;
        int u = ::socket(AF_UNIX, SOCK_STREAM, 0);    // AF_UNIX stays allowed
        if (u < 0) return 4;
        return 0;
    });
    CHECK_MSG(rc == 0, "child rc=%d", rc);
}

TEST(os_sandbox_no_proc_blocks_exec) {
    int rc = in_child([] {
        script::options o; o.allow_proc = false; o.os_sandbox = script::options::os_sandbox_mode::require;
        script::engine e(o);
        char* argv[] = {const_cast<char*>("/bin/true"), nullptr};
        ::execv("/bin/true", argv);   // must fail with EPERM
        return 0;
    });
    CHECK_MSG(rc == 0, "child rc=%d", rc);
}

TEST(os_sandbox_no_fs_blocks_writes) {
    int rc = in_child([] {
        script::options o; o.allow_fs = false; o.os_sandbox = script::options::os_sandbox_mode::require;
        script::engine e(o);
        int fd = ::open("/tmp/forge_os_sandbox_probe", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) { ::close(fd); return 3; }
        return 0;
    });
    CHECK_MSG(rc == 0, "child rc=%d", rc);
}

TEST(os_sandbox_off_when_all_caps_on) {
    int rc = in_child([] { script::engine e; return e.sandbox_status() == "off" ? 0 : 2; });
    CHECK_MSG(rc == 0, "child rc=%d", rc);
}
#endif
