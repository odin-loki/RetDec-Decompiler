#pragma once
// Typed OS errors: turns errno (POSIX: Linux/glibc, Android/bionic, FreeBSD, macOS/iOS) and Win32 error
// codes into a small portable taxonomy and a C++ exception, and turns synchronous fault signals into
// exceptions where that can be done safely.
//
//   auto k = os::classify_errno(EACCES);             // os_errc::permission_denied
//   os::throw_if(os::write_file(p, d));              // throws os::os_exception{kind, code, what}
//   os::fault_guard([&]{ risky_native_call(); });    // SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGSYS -> os_exception{signal}
//   os::classify_ntstatus(0xC0000022);               // STATUS_ACCESS_DENIED -> permission_denied
//   os::install_cancel_signals(&flag);               // SIGINT/SIGTERM (Ctrl-C/Ctrl-Break/close on Windows) set flag
//
// SIGPIPE: install_fault_handlers() sets SIGPIPE to SIG_IGN (Linux/Android/FreeBSD) and SO_NOSIGPIPE is not
// needed on macOS/iOS for that reason; writes to a closed pipe/socket then fail with EPIPE -> broken_pipe.
//
// Sandbox violations: seccomp (Linux/Android) returns EPERM for denied syscalls; Capsicum (FreeBSD) returns
// ECAPMODE/ENOTCAPABLE; pledge (OpenBSD) and seccomp TRAP mode deliver SIGSYS/SIGABRT; macOS/iOS sandbox
// returns EPERM. When sandbox_status().sandboxed is true, EPERM/EACCES/ECAPMODE/ENOTCAPABLE classify as
// sandbox_violation.
//
// Fault recovery (fault_guard) uses sigsetjmp/siglongjmp with a per-thread jump target. It is only safe for
// code that holds no locks and owns no resources across the fault (C++ destructors in the skipped frames do
// not run). iOS/macOS: Mach exceptions are delivered as BSD signals unless a Mach exception port (e.g. a
// debugger or crash reporter) claims them first; then fault_guard cannot intercept. Windows: uses a vectored
// exception handler for access violations / illegal instructions / integer divide by zero, then longjmp.
#include <forge/os/os.hpp>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <csetjmp>
#include <stdexcept>
#include <string>
#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#endif

namespace forge::os {

enum class os_errc {
    ok, not_found, already_exists, permission_denied, sandbox_violation, not_supported, invalid_argument,
    would_block, interrupted, timed_out, connection_refused, connection_reset, network_unreachable,
    address_in_use, no_space, too_many_files, busy, not_a_directory, is_a_directory, broken_pipe,
    out_of_memory, signal, io, cancelled, bad_address, read_only, cross_device, not_empty, name_too_long,
    no_such_process, deadlock, too_many_links, file_too_large, other
};

inline const char* name(os_errc k) {
    switch (k) {
    case os_errc::ok: return "ok"; case os_errc::not_found: return "not_found"; case os_errc::already_exists: return "already_exists";
    case os_errc::permission_denied: return "permission_denied"; case os_errc::sandbox_violation: return "sandbox_violation";
    case os_errc::not_supported: return "not_supported"; case os_errc::invalid_argument: return "invalid_argument";
    case os_errc::would_block: return "would_block"; case os_errc::interrupted: return "interrupted"; case os_errc::timed_out: return "timed_out";
    case os_errc::connection_refused: return "connection_refused"; case os_errc::connection_reset: return "connection_reset";
    case os_errc::network_unreachable: return "network_unreachable"; case os_errc::address_in_use: return "address_in_use";
    case os_errc::no_space: return "no_space"; case os_errc::too_many_files: return "too_many_files"; case os_errc::busy: return "busy";
    case os_errc::not_a_directory: return "not_a_directory"; case os_errc::is_a_directory: return "is_a_directory";
    case os_errc::broken_pipe: return "broken_pipe"; case os_errc::out_of_memory: return "out_of_memory"; case os_errc::signal: return "signal";
    case os_errc::io: return "io"; case os_errc::cancelled: return "cancelled"; case os_errc::bad_address: return "bad_address";
    case os_errc::read_only: return "read_only"; case os_errc::cross_device: return "cross_device"; case os_errc::not_empty: return "not_empty";
    case os_errc::name_too_long: return "name_too_long"; case os_errc::no_such_process: return "no_such_process";
    case os_errc::deadlock: return "deadlock"; case os_errc::too_many_links: return "too_many_links";
    case os_errc::file_too_large: return "file_too_large"; default: return "other";
    }
}

// ForgeScript exception class for each kind (see script/src/builtins.cpp).
inline const char* script_error_type(os_errc k) {
    switch (k) {
    case os_errc::not_found: return "FileNotFoundError";
    case os_errc::already_exists: return "FileExistsError";
    case os_errc::permission_denied: return "PermissionError";
    case os_errc::sandbox_violation: return "SandboxViolationError";
    case os_errc::not_supported: return "NotSupportedError";
    case os_errc::timed_out: return "TimeoutError";
    case os_errc::interrupted: return "InterruptedError";
    case os_errc::connection_refused: return "ConnectionRefusedError";
    case os_errc::connection_reset: return "ConnectionResetError";
    case os_errc::broken_pipe: return "BrokenPipeError";
    case os_errc::network_unreachable: case os_errc::address_in_use: return "ConnectionError";
    case os_errc::would_block: return "BlockingIOError";
    case os_errc::not_a_directory: return "NotADirectoryError";
    case os_errc::is_a_directory: return "IsADirectoryError";
    case os_errc::no_such_process: return "ProcessLookupError";
    case os_errc::no_space: return "NoSpaceError";
    case os_errc::too_many_files: return "TooManyFilesError";
    case os_errc::busy: return "BusyError";
    case os_errc::read_only: return "ReadOnlyFileSystemError";
    case os_errc::cancelled: return "CancelledError";
    case os_errc::deadlock: return "DeadlockError";
    case os_errc::bad_address: return "SignalError";
    case os_errc::out_of_memory: return "OutOfMemoryError";
    case os_errc::signal: return "SignalError";
    case os_errc::invalid_argument: return "ValueError";
    default: return "OSError";
    }
}

// What kind of operation a failing call was, so EPERM/EACCES is only called a sandbox violation when the
// active sandbox policy actually denies that kind of operation (a plain chmod-000 file stays PermissionError).
enum class os_op : std::uint8_t { any, fs_read, fs_write, net, exec };
namespace detail {
struct sbx_state { std::atomic<bool> on{false}, all{false}, net{false}, exec{false}, fs_write{false}; };
inline sbx_state& sbx() { static sbx_state s; return s; }
inline thread_local os_op t_op = os_op::any;
// True when the sandbox is active and denies the current (or given) operation kind. With note_sandboxed(true)
// (policy unknown) every operation matches; with a recorded policy, os_op::any and fs_read never match.
inline bool assume_sandboxed(os_op op) {
    auto& s = sbx(); if (!s.on.load()) return false;
    if (s.all.load()) return true;
    switch (op) {
    case os_op::net: return s.net.load();
    case os_op::exec: return s.exec.load();
    case os_op::fs_write: return s.fs_write.load();
    default: return false;
    }
}
inline bool assume_sandboxed() { return assume_sandboxed(t_op); }
}
// Set by whoever applies the sandbox. note_sandboxed(true): sandboxed with an unknown policy (any EPERM counts);
// note_sandbox_policy(): the exact denied classes (what engine / process_pool / sandbox{isolate} apply).
inline void note_sandboxed(bool on = true) { auto& s = detail::sbx(); s.on = on; s.all = on; s.net = s.exec = s.fs_write = false; }
inline void note_sandbox_policy(bool deny_net, bool deny_exec, bool deny_fs_write) {
    auto& s = detail::sbx(); s.all = false; s.net = deny_net; s.exec = deny_exec; s.fs_write = deny_fs_write;
    s.on = deny_net || deny_exec || deny_fs_write;
}
// RAII: mark the operations in this scope (on this thread) as `op` for classification.
struct os_op_scope {
    os_op prev;
    explicit os_op_scope(os_op o) : prev(detail::t_op) { detail::t_op = o; }
    ~os_op_scope() { detail::t_op = prev; }
    os_op_scope(const os_op_scope&) = delete; os_op_scope& operator=(const os_op_scope&) = delete;
};

inline os_errc classify_errno(int e, os_op op) {
    bool sbx = detail::assume_sandboxed(op);
    switch (e) {
    case 0: return os_errc::ok;
    case ENOENT: return os_errc::not_found;
    case EEXIST: return os_errc::already_exists;
    // seccomp ERRNO, macOS sandbox and pledge report denials as EPERM; EACCES is ordinary file-mode/ACL denial,
    // so with a recorded policy only EPERM counts (EACCES only when the policy is unknown).
    case EPERM: return sbx ? os_errc::sandbox_violation : os_errc::permission_denied;
    case EACCES: return sbx && detail::sbx().all.load() ? os_errc::sandbox_violation : os_errc::permission_denied;
#if defined(ECAPMODE)       // FreeBSD Capsicum
    case ECAPMODE: return os_errc::sandbox_violation;
#endif
#if defined(ENOTCAPABLE)
    case ENOTCAPABLE: return os_errc::sandbox_violation;
#endif
    case ENOSYS: return os_errc::not_supported;
#if defined(ENOTSUP)
    case ENOTSUP: return os_errc::not_supported;
#endif
#if defined(EOPNOTSUPP) && (!defined(ENOTSUP) || EOPNOTSUPP != ENOTSUP)
    case EOPNOTSUPP: return os_errc::not_supported;
#endif
    case EINVAL: return os_errc::invalid_argument;
    case EAGAIN: return os_errc::would_block;
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK: return os_errc::would_block;
#endif
    case EINTR: return os_errc::interrupted;
    case ETIMEDOUT: return os_errc::timed_out;
    case ECONNREFUSED: return os_errc::connection_refused;
    case ECONNRESET: return os_errc::connection_reset;
    case ENETUNREACH: case EHOSTUNREACH: return os_errc::network_unreachable;
    case EADDRINUSE: return os_errc::address_in_use;
    case ENOSPC: return os_errc::no_space;
#if defined(EDQUOT)
    case EDQUOT: return os_errc::no_space;
#endif
    case EMFILE: case ENFILE: return os_errc::too_many_files;
    case EBUSY: return os_errc::busy;
    case ENOTDIR: return os_errc::not_a_directory;
    case EISDIR: return os_errc::is_a_directory;
    case EPIPE: return os_errc::broken_pipe;
    case ENOMEM: return os_errc::out_of_memory;
    case EIO: return os_errc::io;
    case EFAULT: return os_errc::bad_address;
    case EROFS: return os_errc::read_only;
    case EXDEV: return os_errc::cross_device;
    case ENOTEMPTY: return os_errc::not_empty;
    case ENAMETOOLONG: return os_errc::name_too_long;
    case ESRCH: return os_errc::no_such_process;
    case EDEADLK: return os_errc::deadlock;
    case EMLINK: return os_errc::too_many_links;
#if defined(ELOOP)
    case ELOOP: return os_errc::too_many_links;
#endif
    case EFBIG: return os_errc::file_too_large;
    case ECHILD: return os_errc::no_such_process;
    case ENODEV: case ENXIO: return os_errc::not_found;
    case ENOTTY: return os_errc::not_supported;
    case ECONNABORTED: return os_errc::connection_reset;
#if defined(ENOTCONN)
    case ENOTCONN: return os_errc::connection_reset;
#endif
#if defined(ENETDOWN)
    case ENETDOWN: return os_errc::network_unreachable;
#endif
#if defined(EADDRNOTAVAIL)
    case EADDRNOTAVAIL: return os_errc::address_in_use;
#endif
#if defined(EALREADY)
    case EALREADY: case EINPROGRESS: return os_errc::would_block;
#endif
#if defined(ECANCELED)
    case ECANCELED: return os_errc::cancelled;
#endif
#if defined(ETXTBSY)
    case ETXTBSY: return os_errc::busy;
#endif
#if defined(EOVERFLOW)
    case EOVERFLOW: return os_errc::invalid_argument;
#endif
#if defined(ERANGE)
    case ERANGE: return os_errc::invalid_argument;
#endif
#if defined(EBADF)
    case EBADF: return os_errc::invalid_argument;
#endif
#if defined(ESPIPE)
    case ESPIPE: return os_errc::invalid_argument;
#endif
#if defined(ENOEXEC)
    case ENOEXEC: return os_errc::not_supported;
#endif
#if defined(E2BIG)
    case E2BIG: return os_errc::invalid_argument;
#endif
#if defined(ENOLCK)
    case ENOLCK: return os_errc::too_many_files;
#endif
#if defined(ENOBUFS)
    case ENOBUFS: return os_errc::out_of_memory;
#endif
#if defined(ESHUTDOWN)
    case ESHUTDOWN: return os_errc::broken_pipe;
#endif
#if defined(EAUTH)          // FreeBSD / macOS
    case EAUTH: return os_errc::permission_denied;
#endif
#if defined(ENEEDAUTH)
    case ENEEDAUTH: return os_errc::permission_denied;
#endif
#if defined(EBADEXEC)       // macOS / iOS
    case EBADEXEC: case EBADARCH: case EBADMACHO: return os_errc::not_supported;
#endif
#if defined(EINTEGRITY)     // FreeBSD
    case EINTEGRITY: return os_errc::io;
#endif
#if defined(EKEYREJECTED)   // Linux key management / some LSMs
    case EKEYREJECTED: return os_errc::permission_denied;
#endif
    default: return os_errc::other;
    }
}
inline os_errc classify_errno(int e) { return classify_errno(e, detail::t_op); }

#if defined(_WIN32)
inline os_errc classify_win32(unsigned long e) {
    switch (e) {
    case ERROR_SUCCESS: return os_errc::ok;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE: case ERROR_MOD_NOT_FOUND: return os_errc::not_found;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return os_errc::already_exists;
    case ERROR_ACCESS_DENIED: case ERROR_PRIVILEGE_NOT_HELD: return detail::assume_sandboxed() ? os_errc::sandbox_violation : os_errc::permission_denied;
    case ERROR_NOT_SUPPORTED: case ERROR_CALL_NOT_IMPLEMENTED: return os_errc::not_supported;
    case ERROR_INVALID_PARAMETER: case ERROR_INVALID_NAME: return os_errc::invalid_argument;
    case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_BUSY: return os_errc::busy;
    case ERROR_TIMEOUT: case WAIT_TIMEOUT: return os_errc::timed_out;
    case ERROR_OPERATION_ABORTED: return os_errc::interrupted;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return os_errc::no_space;
    case ERROR_TOO_MANY_OPEN_FILES: return os_errc::too_many_files;
    case ERROR_DIRECTORY: return os_errc::not_a_directory;
    case ERROR_BROKEN_PIPE: case ERROR_NO_DATA: return os_errc::broken_pipe;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return os_errc::out_of_memory;
    case ERROR_CONNECTION_REFUSED: case 10061 /*WSAECONNREFUSED*/: return os_errc::connection_refused;
    case 10054 /*WSAECONNRESET*/: return os_errc::connection_reset;
    case 10051: case 10065: return os_errc::network_unreachable;
    case 10048 /*WSAEADDRINUSE*/: return os_errc::address_in_use;
    case 10060 /*WSAETIMEDOUT*/: return os_errc::timed_out;
    case 10035 /*WSAEWOULDBLOCK*/: return os_errc::would_block;
    case 10053 /*WSAECONNABORTED*/: case 10057 /*WSAENOTCONN*/: return os_errc::connection_reset;
    case 10050 /*WSAENETDOWN*/: return os_errc::network_unreachable;
    case 10049 /*WSAEADDRNOTAVAIL*/: return os_errc::address_in_use;
    case 10013 /*WSAEACCES*/: return detail::assume_sandboxed() ? os_errc::sandbox_violation : os_errc::permission_denied;
    case 10036 /*WSAEINPROGRESS*/: case 10037 /*WSAEALREADY*/: return os_errc::would_block;
    case 10004 /*WSAEINTR*/: return os_errc::interrupted;
    case 10022 /*WSAEINVAL*/: return os_errc::invalid_argument;
    case 10024 /*WSAEMFILE*/: return os_errc::too_many_files;
    case 10055 /*WSAENOBUFS*/: return os_errc::out_of_memory;
    case ERROR_DIR_NOT_EMPTY: return os_errc::not_empty;
    case ERROR_NOT_SAME_DEVICE: return os_errc::cross_device;
    case ERROR_WRITE_PROTECT: return os_errc::read_only;
    case ERROR_FILENAME_EXCED_RANGE: case ERROR_BUFFER_OVERFLOW: return os_errc::name_too_long;
    case ERROR_INVALID_HANDLE: return os_errc::invalid_argument;
    case ERROR_NOACCESS: return os_errc::bad_address;
    case ERROR_CANCELLED: return os_errc::cancelled;
    case ERROR_POSSIBLE_DEADLOCK: return os_errc::deadlock;
    case ERROR_FILE_TOO_LARGE: return os_errc::file_too_large;
    case ERROR_PIPE_BUSY: return os_errc::busy;   // every instance of the pipe is taken
    case ERROR_PIPE_NOT_CONNECTED: return os_errc::broken_pipe;
    case ERROR_IO_PENDING: return os_errc::would_block;
    case ERROR_NETNAME_DELETED: return os_errc::connection_reset;
    case ERROR_BAD_NETPATH: case ERROR_BAD_NET_NAME: return os_errc::not_found;
    case ERROR_INVALID_FUNCTION: return os_errc::not_supported;
    case ERROR_CRC: case ERROR_READ_FAULT: case ERROR_WRITE_FAULT: case ERROR_GEN_FAILURE: return os_errc::io;
    case ERROR_ELEVATION_REQUIRED: return os_errc::permission_denied;
    case ERROR_HANDLE_EOF: case ERROR_SEEK: case ERROR_NOT_READY: return os_errc::io;
    case ERROR_SEM_TIMEOUT: return os_errc::timed_out;
    case ERROR_PROC_NOT_FOUND: case ERROR_NOT_FOUND: case ERROR_DEV_NOT_EXIST: case 11001 /*WSAHOST_NOT_FOUND*/: case 11004 /*WSANO_DATA*/:
        return os_errc::not_found;
    case 11002 /*WSATRY_AGAIN*/: return os_errc::would_block;
    case 11003 /*WSANO_RECOVERY*/: case ERROR_NETWORK_UNREACHABLE: case ERROR_HOST_UNREACHABLE: case 10064 /*WSAEHOSTDOWN*/:
        return os_errc::network_unreachable;
    case ERROR_CONNECTION_ABORTED: case 10052 /*WSAENETRESET*/: return os_errc::connection_reset;
    case ERROR_PORT_UNREACHABLE: return os_errc::connection_refused;
    case ERROR_ADDRESS_ALREADY_ASSOCIATED: return os_errc::address_in_use;
    case 10058 /*WSAESHUTDOWN*/: case ERROR_BAD_PIPE: return os_errc::broken_pipe;
    case 10063 /*WSAENAMETOOLONG*/: return os_errc::name_too_long;
    case 10047 /*WSAEAFNOSUPPORT*/: case 10043 /*WSAEPROTONOSUPPORT*/: case 10045 /*WSAEOPNOTSUPP*/: case ERROR_BAD_EXE_FORMAT: return os_errc::not_supported;
    case 10038 /*WSAENOTSOCK*/: case 10040 /*WSAEMSGSIZE*/: case 10009 /*WSAEBADF*/: case ERROR_INVALID_DATA: case ERROR_NEGATIVE_SEEK:
    case ERROR_BAD_PATHNAME: case ERROR_NOT_A_REPARSE_POINT: case ERROR_INSUFFICIENT_BUFFER: case ERROR_ARITHMETIC_OVERFLOW: return os_errc::invalid_argument;
    case 10014 /*WSAEFAULT*/: case ERROR_INVALID_ADDRESS: return os_errc::bad_address;
    case 10103 /*WSAECANCELLED*/: return os_errc::cancelled;
    case ERROR_PATH_BUSY: case ERROR_DRIVE_LOCKED: case ERROR_USER_MAPPED_FILE: return os_errc::busy;
    case ERROR_TOO_MANY_LINKS: case ERROR_CANT_RESOLVE_FILENAME: return os_errc::too_many_links;
    case ERROR_NO_SYSTEM_RESOURCES: case ERROR_COMMITMENT_LIMIT: case ERROR_WORKING_SET_QUOTA: return os_errc::out_of_memory;
    default: return os_errc::other;
    }
}
#endif

// NTSTATUS (Windows native API / SEH exception codes). Portable: plain integers, usable on any host so
// cross-platform tools can classify a Windows crash code. Values from ntstatus.h.
inline os_errc classify_ntstatus(std::uint32_t st) {
    switch (st) {
    case 0x00000000u: return os_errc::ok;
    case 0xC0000022u /*ACCESS_DENIED*/: case 0xC0000061u /*PRIVILEGE_NOT_HELD*/:
        return detail::assume_sandboxed() ? os_errc::sandbox_violation : os_errc::permission_denied;
    case 0xC0000034u /*OBJECT_NAME_NOT_FOUND*/: case 0xC000003Au /*OBJECT_PATH_NOT_FOUND*/: case 0xC000000Fu /*NO_SUCH_FILE*/:
    case 0xC0000135u /*DLL_NOT_FOUND*/: return os_errc::not_found;
    case 0xC0000035u /*OBJECT_NAME_COLLISION*/: return os_errc::already_exists;
    case 0xC00000BBu /*NOT_SUPPORTED*/: case 0xC0000002u /*NOT_IMPLEMENTED*/: return os_errc::not_supported;
    case 0xC000000Du /*INVALID_PARAMETER*/: case 0xC0000008u /*INVALID_HANDLE*/: case 0xC0000033u /*OBJECT_NAME_INVALID*/: return os_errc::invalid_argument;
    case 0xC0000043u /*SHARING_VIOLATION*/: case 0xC0000054u /*FILE_LOCK_CONFLICT*/: return os_errc::busy;
    case 0x00000102u /*TIMEOUT*/: case 0xC00000B5u /*IO_TIMEOUT*/: return os_errc::timed_out;
    case 0xC0000120u /*CANCELLED*/: return os_errc::cancelled;
    case 0xC000007Fu /*DISK_FULL*/: return os_errc::no_space;
    case 0xC0000017u /*NO_MEMORY*/: case 0xC000009Au /*INSUFFICIENT_RESOURCES*/: case 0xC000012Du /*COMMITMENT_LIMIT*/: return os_errc::out_of_memory;
    case 0xC000014Bu /*PIPE_BROKEN*/: case 0xC00000B1u /*PIPE_CLOSING*/: return os_errc::broken_pipe;
    case 0xC0000236u /*CONNECTION_REFUSED*/: return os_errc::connection_refused;
    case 0xC000020Du /*CONNECTION_RESET*/: case 0xC0000241u /*CONNECTION_ABORTED*/: return os_errc::connection_reset;
    case 0xC000023Cu /*NETWORK_UNREACHABLE*/: case 0xC000023Du /*HOST_UNREACHABLE*/: return os_errc::network_unreachable;
    case 0xC0000101u /*DIRECTORY_NOT_EMPTY*/: return os_errc::not_empty;
    case 0xC0000103u /*NOT_A_DIRECTORY*/: return os_errc::not_a_directory;
    case 0xC00000BAu /*FILE_IS_A_DIRECTORY*/: return os_errc::is_a_directory;
    case 0xC00000A2u /*MEDIA_WRITE_PROTECTED*/: return os_errc::read_only;
    case 0xC00000D4u /*NOT_SAME_DEVICE*/: return os_errc::cross_device;
    case 0xC0000185u /*IO_DEVICE_ERROR*/: case 0xC000009Cu /*DEVICE_DATA_ERROR*/: return os_errc::io;
    // SEH exception codes: hardware faults surface as signals
    case 0xC0000005u /*ACCESS_VIOLATION*/: case 0xC0000006u /*IN_PAGE_ERROR*/: case 0x80000002u /*DATATYPE_MISALIGNMENT*/:
    case 0xC000001Du /*ILLEGAL_INSTRUCTION*/: case 0xC0000096u /*PRIV_INSTRUCTION*/: case 0xC0000094u /*INT_DIVIDE_BY_ZERO*/:
    case 0xC0000095u /*INT_OVERFLOW*/: case 0xC000008Eu /*FLT_DIVIDE_BY_ZERO*/: case 0xC0000090u /*FLT_INVALID_OPERATION*/:
    case 0xC0000091u /*FLT_OVERFLOW*/: case 0xC0000093u /*FLT_UNDERFLOW*/: case 0xC000008Fu /*FLT_INEXACT_RESULT*/:
    case 0xC000008Du /*FLT_DENORMAL_OPERAND*/: case 0xC0000092u /*FLT_STACK_CHECK*/: case 0xC00000FDu /*STACK_OVERFLOW*/:
    case 0xC000008Cu /*ARRAY_BOUNDS_EXCEEDED*/: case 0xC0000409u /*STACK_BUFFER_OVERRUN / fail-fast*/: case 0x80000003u /*BREAKPOINT*/:
        return os_errc::signal;
    case 0xC000013Au /*CONTROL_C_EXIT*/: return os_errc::cancelled;
    default: return os_errc::other;
    }
}

// Mach exceptions (macOS / iOS). The kernel turns these into BSD signals for the faulting thread unless a
// task/thread exception port claims them (debugger, crash reporter); fault_guard relies on the signal path.
// This table classifies a Mach exception type + code for crash-report / child-status consumers. Values are
// the stable <mach/exception_types.h> numbers so it compiles on every host.
inline os_errc classify_mach_exception(int exc_type, long long code = 0) {
    switch (exc_type) {
    case 1 /*EXC_BAD_ACCESS*/: return code == 0x32 /*KERN_CODESIGN_ERROR*/ ? os_errc::sandbox_violation : os_errc::signal;
    case 2 /*EXC_BAD_INSTRUCTION*/: case 3 /*EXC_ARITHMETIC*/: case 6 /*EXC_BREAKPOINT*/: case 10 /*EXC_CRASH*/: return os_errc::signal;
    case 11 /*EXC_RESOURCE*/: return os_errc::out_of_memory;
    case 12 /*EXC_GUARD*/: return os_errc::sandbox_violation;   // guarded fd / mach port / sandbox guard violation
    case 7 /*EXC_SYSCALL*/: case 8 /*EXC_MACH_SYSCALL*/: return os_errc::sandbox_violation;
    default: return os_errc::other;
    }
}
// Mach exception -> the BSD signal the kernel delivers for it (what fault_guard sees).
inline int mach_exception_signal(int exc_type) {
#if !defined(_WIN32)
    switch (exc_type) {
    case 1: return SIGSEGV; case 2: return SIGILL; case 3: return SIGFPE; case 6: return SIGTRAP; case 10: return SIGABRT;
    case 7: case 8: return SIGSYS;
    default: return 0;
    }
#else
    (void)exc_type; return 0;
#endif
}

// getaddrinfo EAI_* codes (os::socket::resolve reports them with err_domain::gai). If-chain, not a switch: some
// libcs alias EAI_NODATA to EAI_NONAME.
inline os_errc classify_gai(int e) {
    if (e == 0) return os_errc::ok;
#if defined(EAI_NONAME)
    if (e == EAI_NONAME) return os_errc::not_found;
#endif
#if defined(EAI_NODATA)
    if (e == EAI_NODATA) return os_errc::not_found;
#endif
#if defined(EAI_AGAIN)
    if (e == EAI_AGAIN) return os_errc::would_block;
#endif
#if defined(EAI_FAIL)
    if (e == EAI_FAIL) return os_errc::network_unreachable;
#endif
#if defined(EAI_MEMORY)
    if (e == EAI_MEMORY) return os_errc::out_of_memory;
#endif
#if defined(EAI_FAMILY)
    if (e == EAI_FAMILY) return os_errc::not_supported;
#endif
#if defined(EAI_SOCKTYPE)
    if (e == EAI_SOCKTYPE) return os_errc::not_supported;
#endif
#if defined(EAI_SERVICE)
    if (e == EAI_SERVICE) return os_errc::invalid_argument;
#endif
#if defined(EAI_BADFLAGS)
    if (e == EAI_BADFLAGS) return os_errc::invalid_argument;
#endif
    return os_errc::other;
}

// os::error carries the number space of its code (err_domain): native codes are errno on POSIX and
// GetLastError()/WSAGetLastError() on Windows; posix codes are errno everywhere (also the Windows C runtime and
// libstdc++'s std::filesystem); gai codes are EAI_*. unsupported_code maps to not_supported.
inline os_errc classify(const error& e) {
    if (!e) return os_errc::ok;
    if (is_unsupported(e)) return os_errc::not_supported;
    switch (e.domain) {
    case err_domain::posix: return classify_errno(e.code);
    case err_domain::gai: return classify_gai(e.code);
#if defined(_WIN32)
    case err_domain::win32: case err_domain::native: return classify_win32(static_cast<unsigned long>(e.code));
#else
    case err_domain::win32: return os_errc::other;
    case err_domain::native: return classify_errno(e.code);
#endif
    }
    return os_errc::other;
}

struct os_exception : std::runtime_error {
    os_errc kind; int code; int signo = 0;
    enum class source : std::uint8_t { posix_errno, win32, ntstatus, signal, mach, sandbox } src = source::posix_errno;
    os_exception(os_errc k, int c, const std::string& w, int sig = 0) : std::runtime_error(w), kind(k), code(c), signo(sig) {}
    const char* script_type() const { return script_error_type(kind); }
};

inline void throw_if(const error& e) { if (e) throw os_exception(classify(e), e.code, e.what); }
// Throw from a raw code (errno now, or a given value).
[[noreturn]] inline void throw_errno(int e, const std::string& what) { throw os_exception(classify_errno(e), e, what); }
#if defined(_WIN32)
[[noreturn]] inline void throw_last_error(const std::string& what) {
    auto e = GetLastError(); os_exception x(classify_win32(e), int(e), what); x.src = os_exception::source::win32; throw x;
}
#else
[[noreturn]] inline void throw_last_error(const std::string& what) { throw_errno(errno, what); }
#endif
[[noreturn]] inline void throw_ntstatus(std::uint32_t st, const std::string& what) {
    os_exception x(classify_ntstatus(st), int(st), what); x.src = os_exception::source::ntstatus; throw x;
}
[[noreturn]] inline void throw_sandbox_violation(const std::string& what, int code = 0) {
    os_exception x(os_errc::sandbox_violation, code, what); x.src = os_exception::source::sandbox; throw x;
}
template <class T> T throw_if(result<T> r) { throw_if(r.err); return std::move(r.value); }

// ---------------------------------------------------------------- fault signals -> exceptions
namespace detail {
// volatile: the handler writes sig and reads t_fault asynchronously. Without it an optimiser that sees a fault-free
// (or provably faulting) f() inline drops the t_fault store as dead and constant-folds sig to 0.
struct fault_frame { std::jmp_buf* volatile jb = nullptr; volatile int sig = 0; };   // jb volatile too: only the handler reads it, so a plain store is dead to the optimiser (clang 23 -O3 dropped it -> siglongjmp(nullptr))
inline thread_local fault_frame* volatile t_fault = nullptr;
}

inline const char* signal_name(int s) {
    switch (s) {
#if !defined(_WIN32)
    case SIGSEGV: return "SIGSEGV"; case SIGBUS: return "SIGBUS"; case SIGFPE: return "SIGFPE"; case SIGILL: return "SIGILL";
    case SIGPIPE: return "SIGPIPE"; case SIGTERM: return "SIGTERM"; case SIGINT: return "SIGINT"; case SIGABRT: return "SIGABRT";
    case SIGTRAP: return "SIGTRAP"; case SIGHUP: return "SIGHUP"; case SIGQUIT: return "SIGQUIT";
#if defined(SIGSYS)
    case SIGSYS: return "SIGSYS (system call denied by the sandbox)";
#endif
#else
    case 1: return "EXCEPTION_ACCESS_VIOLATION"; case 2: return "EXCEPTION_ILLEGAL_INSTRUCTION"; case 3: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case 4: return "EXCEPTION_IN_PAGE_ERROR"; case 5: return "EXCEPTION_DATATYPE_MISALIGNMENT"; case 6: return "EXCEPTION_FLT_*";
    case 7: return "EXCEPTION_PRIV_INSTRUCTION"; case 8: return "EXCEPTION_INT_OVERFLOW"; case 9: return "EXCEPTION_STACK_OVERFLOW";
    case 10: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
#endif
    default: return "signal";
    }
}

#if !defined(_WIN32)
namespace detail {
extern "C" inline void forge_fault_handler(int sig, siginfo_t*, void*) {
    if (auto* f = t_fault) { f->sig = sig; ::siglongjmp(*reinterpret_cast<sigjmp_buf*>(f->jb), 1); }
    ::signal(sig, SIG_DFL); ::raise(sig);   // not inside a guard: default action (crash/core) as before
}
inline void install_fault_handlers() {
    static bool done = [] {
        struct sigaction sa{}; sa.sa_sigaction = forge_fault_handler; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigemptyset(&sa.sa_mask);
        int sigs[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL,
#if defined(SIGSYS)
                      SIGSYS,
#endif
        };
        for (int s : sigs) ::sigaction(s, &sa, nullptr);
        ::signal(SIGPIPE, SIG_IGN);   // EPIPE -> broken_pipe instead of killing the process
        return true; }();
    (void)done;
}
}
// Runs f; a synchronous fault inside it becomes os_exception{signal}. See the safety notes at the top.
template <class F> auto fault_guard(F&& f) -> decltype(f()) {
    detail::install_fault_handlers();
    sigjmp_buf jb; detail::fault_frame fr; fr.jb = reinterpret_cast<std::jmp_buf*>(&jb);
    auto* prev = detail::t_fault;
    if (sigsetjmp(jb, 1)) {
        detail::t_fault = prev;
        throw os_exception(fr.sig == SIGSYS ? os_errc::sandbox_violation : os_errc::signal, fr.sig, std::string("fault: ") + signal_name(fr.sig), fr.sig);
    }
    detail::t_fault = &fr;
    struct restore { detail::fault_frame* p; ~restore() { detail::t_fault = p; } } r{prev};
    return f();
}
#else
namespace detail {
inline LONG CALLBACK forge_veh(EXCEPTION_POINTERS* ep) {
    auto* f = t_fault; if (!f) return EXCEPTION_CONTINUE_SEARCH;
    switch (ep->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: f->sig = 1; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: f->sig = 2; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: f->sig = 3; break;
    case EXCEPTION_IN_PAGE_ERROR: f->sig = 4; break;
    case EXCEPTION_DATATYPE_MISALIGNMENT: f->sig = 5; break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: case EXCEPTION_FLT_INVALID_OPERATION: case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_UNDERFLOW: case EXCEPTION_FLT_INEXACT_RESULT: case EXCEPTION_FLT_DENORMAL_OPERAND: case EXCEPTION_FLT_STACK_CHECK: f->sig = 6; break;
    case EXCEPTION_PRIV_INSTRUCTION: f->sig = 7; break;
    case EXCEPTION_INT_OVERFLOW: f->sig = 8; break;
    // Stack overflow: the guard page is gone; longjmp out is the documented-risky path, but the thread's
    // stack is reset by unwinding to the guard frame. _resetstkoflw() is called after the jump.
    case EXCEPTION_STACK_OVERFLOW: f->sig = 9; break;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: f->sig = 10; break;
    default: return EXCEPTION_CONTINUE_SEARCH;
    }
    std::longjmp(*f->jb, 1);
}
inline void install_fault_handlers() { static void* h = AddVectoredExceptionHandler(1, forge_veh); (void)h; }
}
template <class F> auto fault_guard(F&& f) -> decltype(f()) {
    detail::install_fault_handlers();
    std::jmp_buf jb; detail::fault_frame fr; fr.jb = &jb;
    auto* prev = detail::t_fault;
    if (setjmp(jb)) { detail::t_fault = prev; if (fr.sig == 9) _resetstkoflw(); throw os_exception(os_errc::signal, fr.sig, std::string("fault: ") + signal_name(fr.sig), fr.sig); }
    detail::t_fault = &fr;
    struct restore { detail::fault_frame* p; ~restore() { detail::t_fault = p; } } r{prev};
    return f();
}
#endif

// ---------------------------------------------------------------- cancellation signals
// SIGINT / SIGTERM (POSIX) and Ctrl-C / Ctrl-Break / console close / logoff / shutdown (Windows) set *flag and
// record the signal; the consumer (ForgeScript's step()) turns that into CancelledError. Pass nullptr to restore
// the default handlers.
namespace detail {
inline std::atomic<std::atomic<bool>*>& cancel_flag() { static std::atomic<std::atomic<bool>*> f{nullptr}; return f; }
inline std::atomic<int>& last_cancel_signal() { static std::atomic<int> s{0}; return s; }
#if !defined(_WIN32)
extern "C" inline void forge_cancel_handler(int sig) {
    last_cancel_signal().store(sig);
    if (auto* f = cancel_flag().load()) f->store(true);
}
#else
inline BOOL WINAPI forge_console_ctrl(DWORD ev) {
    auto* f = cancel_flag().load(); if (!f) return FALSE;
    last_cancel_signal().store(ev == CTRL_C_EVENT ? 2 /*SIGINT*/ : 15 /*SIGTERM*/);
    f->store(true); return TRUE;
}
#endif
}
// Installing saves the handlers that were there; install_cancel_signals(nullptr) puts them back.
inline void install_cancel_signals(std::atomic<bool>* flag) {
    auto* was = detail::cancel_flag().exchange(flag);
#if !defined(_WIN32)
    static struct sigaction old_int{}, old_term{};
    static bool saved = false;
    if (flag) {
        struct sigaction sa{}; sa.sa_handler = detail::forge_cancel_handler; sigemptyset(&sa.sa_mask);
        if (!was || !saved) { ::sigaction(SIGINT, &sa, &old_int); ::sigaction(SIGTERM, &sa, &old_term); saved = true; }
        else { ::sigaction(SIGINT, &sa, nullptr); ::sigaction(SIGTERM, &sa, nullptr); }
    } else if (saved) { ::sigaction(SIGINT, &old_int, nullptr); ::sigaction(SIGTERM, &old_term, nullptr); saved = false; }
#else
    SetConsoleCtrlHandler(detail::forge_console_ctrl, flag ? TRUE : FALSE);
#endif
}
// Signal number that last requested cancellation (SIGINT=2 / SIGTERM=15), 0 if none; resets it.
inline int take_cancel_signal() { return detail::last_cancel_signal().exchange(0); }
inline os_exception cancellation_exception(int sig) {
    os_exception x(os_errc::cancelled, sig, std::string("cancelled by ") + (sig == 15 ? "SIGTERM" : sig == 2 ? "SIGINT" : "signal"), sig);
    x.src = os_exception::source::signal; return x;
}

} // namespace forge::os
