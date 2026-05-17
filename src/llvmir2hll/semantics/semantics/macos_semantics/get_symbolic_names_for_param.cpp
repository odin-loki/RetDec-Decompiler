/**
* @file src/llvmir2hll/semantics/semantics/macos_semantics/get_symbolic_names_for_param.cpp
* @brief Implementation of getSymbolicNamesForParam() for macOS / BSD targets.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*
* Overrides/supplements gcc_general for macOS-specific constant values that
* differ from the Linux defaults (signal numbering, open(2) flags, mmap flags,
* kqueue, Grand Central Dispatch, sysctl).  All POSIX-portable entries that
* share values with Linux are inherited from libc_semantics.
*/

#include "retdec/llvmir2hll/semantics/semantics/impl_support/get_symbolic_names_for_param.h"
#include "retdec/llvmir2hll/semantics/semantics/macos_semantics/get_symbolic_names_for_param.h"

namespace retdec {
namespace llvmir2hll {
namespace semantics {
namespace macos {

namespace {

// ── Signals (BSD numbering differs from Linux from signal 7 onward) ──────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForSignals)
	symbolicNamesMap[1]  = "SIGHUP";
	symbolicNamesMap[2]  = "SIGINT";
	symbolicNamesMap[3]  = "SIGQUIT";
	symbolicNamesMap[4]  = "SIGILL";
	symbolicNamesMap[5]  = "SIGTRAP";
	symbolicNamesMap[6]  = "SIGABRT";
	symbolicNamesMap[7]  = "SIGEMT";   // BSD: EMT; Linux: SIGBUS
	symbolicNamesMap[8]  = "SIGFPE";
	symbolicNamesMap[9]  = "SIGKILL";
	symbolicNamesMap[10] = "SIGBUS";   // BSD; Linux has this at 7
	symbolicNamesMap[11] = "SIGSEGV";
	symbolicNamesMap[12] = "SIGSYS";   // BSD; Linux at 31
	symbolicNamesMap[13] = "SIGPIPE";
	symbolicNamesMap[14] = "SIGALRM";
	symbolicNamesMap[15] = "SIGTERM";
	symbolicNamesMap[16] = "SIGURG";
	symbolicNamesMap[17] = "SIGSTOP";
	symbolicNamesMap[18] = "SIGTSTP";
	symbolicNamesMap[19] = "SIGCONT";
	symbolicNamesMap[20] = "SIGCHLD";
	symbolicNamesMap[21] = "SIGTTIN";
	symbolicNamesMap[22] = "SIGTTOU";
	symbolicNamesMap[23] = "SIGIO";
	symbolicNamesMap[24] = "SIGXCPU";
	symbolicNamesMap[25] = "SIGXFSZ";
	symbolicNamesMap[26] = "SIGVTALRM";
	symbolicNamesMap[27] = "SIGPROF";
	symbolicNamesMap[28] = "SIGWINCH";
	symbolicNamesMap[29] = "SIGINFO";  // BSD; Linux: SIGIO/SIGPOLL
	symbolicNamesMap[30] = "SIGUSR1";
	symbolicNamesMap[31] = "SIGUSR2";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForSigmaskHow)
	symbolicNamesMap[1] = "SIG_BLOCK";
	symbolicNamesMap[2] = "SIG_UNBLOCK";
	symbolicNamesMap[3] = "SIG_SETMASK";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── open(2) flags (macOS <fcntl.h>) ──────────────────────────────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForOpenFlags)
	symbolicNamesMap[0x0000] = "O_RDONLY";
	symbolicNamesMap[0x0001] = "O_WRONLY";
	symbolicNamesMap[0x0002] = "O_RDWR";
	symbolicNamesMap[0x0004] = "O_NONBLOCK";
	symbolicNamesMap[0x0008] = "O_APPEND";
	symbolicNamesMap[0x0010] = "O_SHLOCK";
	symbolicNamesMap[0x0020] = "O_EXLOCK";
	symbolicNamesMap[0x0040] = "O_ASYNC";
	symbolicNamesMap[0x0080] = "O_FSYNC";    // synonym O_SYNC on macOS
	symbolicNamesMap[0x0100] = "O_SYNC";
	symbolicNamesMap[0x0200] = "O_CREAT";
	symbolicNamesMap[0x0400] = "O_TRUNC";
	symbolicNamesMap[0x0800] = "O_EXCL";
	symbolicNamesMap[0x8000] = "O_NOCTTY";
	symbolicNamesMap[0x1000000]  = "O_CLOEXEC";
	symbolicNamesMap[0x4000000]  = "O_NOFOLLOW";
	symbolicNamesMap[0x20000]    = "O_DIRECTORY";
	symbolicNamesMap[0x80000]    = "O_SYMLINK";
	symbolicNamesMap[0x200000]   = "O_NOATIME";  // not standard on macOS, but present
	symbolicNamesMap[0x10000000] = "O_EVTONLY";  // macOS only
	symbolicNamesMap[0x20000000] = "O_NDELAY";   // synonym for O_NONBLOCK
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── mmap flags (macOS <sys/mman.h>) ──────────────────────────────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForMmapFlags)
	symbolicNamesMap[0x0001] = "MAP_SHARED";
	symbolicNamesMap[0x0002] = "MAP_PRIVATE";
	symbolicNamesMap[0x0010] = "MAP_FIXED";
	symbolicNamesMap[0x0020] = "MAP_RENAME";
	symbolicNamesMap[0x0040] = "MAP_NORESERVE";
	symbolicNamesMap[0x0100] = "MAP_NOEXTEND";
	symbolicNamesMap[0x0200] = "MAP_HASSEMAPHORE";
	symbolicNamesMap[0x1000] = "MAP_ANON";
	// symbolicNamesMap[0x1000] = "MAP_ANONYMOUS"; // synonym
	symbolicNamesMap[0x4000] = "MAP_NOCACHE";
	symbolicNamesMap[0x8000] = "MAP_JIT";
	// Common combos
	symbolicNamesMap[0x1002] = "MAP_PRIVATE|MAP_ANON";
	symbolicNamesMap[0x1001] = "MAP_SHARED|MAP_ANON";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── mmap prot ────────────────────────────────────────────────────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForMmapProt)
	symbolicNamesMap[0x00] = "PROT_NONE";
	symbolicNamesMap[0x01] = "PROT_READ";
	symbolicNamesMap[0x02] = "PROT_WRITE";
	symbolicNamesMap[0x04] = "PROT_EXEC";
	symbolicNamesMap[0x03] = "PROT_READ|PROT_WRITE";
	symbolicNamesMap[0x05] = "PROT_READ|PROT_EXEC";
	symbolicNamesMap[0x07] = "PROT_READ|PROT_WRITE|PROT_EXEC";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── file permission mode ─────────────────────────────────────────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForPermMode)
	symbolicNamesMap[00700] = "S_IRWXU";
	symbolicNamesMap[00400] = "S_IRUSR";
	symbolicNamesMap[00200] = "S_IWUSR";
	symbolicNamesMap[00100] = "S_IXUSR";
	symbolicNamesMap[00070] = "S_IRWXG";
	symbolicNamesMap[00040] = "S_IRGRP";
	symbolicNamesMap[00020] = "S_IWGRP";
	symbolicNamesMap[00010] = "S_IXGRP";
	symbolicNamesMap[00007] = "S_IRWXO";
	symbolicNamesMap[00004] = "S_IROTH";
	symbolicNamesMap[00002] = "S_IWOTH";
	symbolicNamesMap[00001] = "S_IXOTH";
	symbolicNamesMap[04000] = "S_ISUID";
	symbolicNamesMap[02000] = "S_ISGID";
	symbolicNamesMap[01000] = "S_ISVTX";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── access(2) mode ───────────────────────────────────────────────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForAccessMode)
	symbolicNamesMap[0] = "F_OK";
	symbolicNamesMap[1] = "X_OK";
	symbolicNamesMap[2] = "W_OK";
	symbolicNamesMap[4] = "R_OK";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── waitpid options ───────────────────────────────────────────────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForWaitOptions)
	symbolicNamesMap[0x01] = "WNOHANG";
	symbolicNamesMap[0x02] = "WUNTRACED";
	symbolicNamesMap[0x04] = "WEXITED";
	symbolicNamesMap[0x08] = "WCONTINUED";
	symbolicNamesMap[0x20] = "WNOWAIT";   // macOS value differs from Linux
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── SEEK whence ───────────────────────────────────────────────────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForSeekWhence)
	symbolicNamesMap[0] = "SEEK_SET";
	symbolicNamesMap[1] = "SEEK_CUR";
	symbolicNamesMap[2] = "SEEK_END";
	symbolicNamesMap[3] = "SEEK_DATA";
	symbolicNamesMap[4] = "SEEK_HOLE";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── socket address families (macOS values same as Linux for common ones) ──────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForAddressFamilies)
	symbolicNamesMap[0]  = "AF_UNSPEC";
	symbolicNamesMap[1]  = "AF_UNIX";
	symbolicNamesMap[2]  = "AF_INET";
	symbolicNamesMap[4]  = "AF_IPX";
	symbolicNamesMap[5]  = "AF_APPLETALK";
	symbolicNamesMap[10] = "AF_INET6";
	symbolicNamesMap[16] = "AF_NATM";       // macOS (ATM)
	symbolicNamesMap[18] = "AF_PPP";        // macOS
	symbolicNamesMap[30] = "AF_INET6";      // macOS internal alias
	symbolicNamesMap[36] = "AF_BLUETOOTH";  // macOS
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── socket types (same values as Linux) ──────────────────────────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForSocketTypes)
	symbolicNamesMap[1] = "SOCK_STREAM";
	symbolicNamesMap[2] = "SOCK_DGRAM";
	symbolicNamesMap[3] = "SOCK_RAW";
	symbolicNamesMap[4] = "SOCK_RDM";
	symbolicNamesMap[5] = "SOCK_SEQPACKET";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── socket shutdown how (BSD: SHUT_* same values) ────────────────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForShutdownHow)
	symbolicNamesMap[0] = "SHUT_RD";
	symbolicNamesMap[1] = "SHUT_WR";
	symbolicNamesMap[2] = "SHUT_RDWR";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── fcntl cmd (BSD <fcntl.h>) ─────────────────────────────────────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForFcntlCmd)
	symbolicNamesMap[0]  = "F_DUPFD";
	symbolicNamesMap[1]  = "F_GETFD";
	symbolicNamesMap[2]  = "F_SETFD";
	symbolicNamesMap[3]  = "F_GETFL";
	symbolicNamesMap[4]  = "F_SETFL";
	symbolicNamesMap[5]  = "F_GETOWN";      // BSD order differs
	symbolicNamesMap[6]  = "F_SETOWN";
	symbolicNamesMap[7]  = "F_GETLK";
	symbolicNamesMap[8]  = "F_SETLK";
	symbolicNamesMap[9]  = "F_SETLKW";
	symbolicNamesMap[10] = "F_FLUSH_DATA";  // macOS only
	symbolicNamesMap[11] = "F_CHKCLEAN";    // macOS only
	symbolicNamesMap[12] = "F_PREALLOCATE"; // macOS only
	symbolicNamesMap[13] = "F_SETSIZE";     // macOS only
	symbolicNamesMap[14] = "F_RDADVISE";    // macOS only
	symbolicNamesMap[15] = "F_RDAHEAD";     // macOS only
	symbolicNamesMap[20] = "F_NOCACHE";     // macOS only
	symbolicNamesMap[21] = "F_LOG2PHYS";    // macOS only
	symbolicNamesMap[22] = "F_GETPATH";     // macOS only
	symbolicNamesMap[23] = "F_FULLFSYNC";   // macOS only
	symbolicNamesMap[24] = "F_PATHPKG_CHECK"; // macOS only
	symbolicNamesMap[25] = "F_FREEZE_FS";   // macOS only
	symbolicNamesMap[26] = "F_THAW_FS";     // macOS only
	symbolicNamesMap[27] = "F_GLOBAL_NOCACHE"; // macOS only
	symbolicNamesMap[28] = "F_ADDSIGS";     // macOS only
	symbolicNamesMap[40] = "F_NODIRECT";    // macOS only
	symbolicNamesMap[67] = "F_DUPFD_CLOEXEC"; // POSIX.1-2008
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── flock operation (same as Linux) ──────────────────────────────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForFlockOp)
	symbolicNamesMap[1] = "LOCK_SH";
	symbolicNamesMap[2] = "LOCK_EX";
	symbolicNamesMap[4] = "LOCK_NB";
	symbolicNamesMap[8] = "LOCK_UN";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── rlimit resource (BSD numbering for RSS/NPROC/MEMLOCK differs) ─────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForRlimitResource)
	symbolicNamesMap[0]  = "RLIMIT_CPU";
	symbolicNamesMap[1]  = "RLIMIT_FSIZE";
	symbolicNamesMap[2]  = "RLIMIT_DATA";
	symbolicNamesMap[3]  = "RLIMIT_STACK";
	symbolicNamesMap[4]  = "RLIMIT_CORE";
	symbolicNamesMap[5]  = "RLIMIT_RSS";      // BSD: 5; Linux: no fixed slot
	symbolicNamesMap[6]  = "RLIMIT_MEMLOCK";  // BSD: 6
	symbolicNamesMap[7]  = "RLIMIT_NPROC";    // BSD: 7
	symbolicNamesMap[8]  = "RLIMIT_NOFILE";   // BSD: 8
	symbolicNamesMap[9]  = "RLIMIT_SBSIZE";   // BSD only
	symbolicNamesMap[10] = "RLIMIT_AS";       // BSD: 10
	symbolicNamesMap[11] = "RLIMIT_NPTS";     // FreeBSD
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── getpriority which ─────────────────────────────────────────────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForPrioWhich)
	symbolicNamesMap[0] = "PRIO_PROCESS";
	symbolicNamesMap[1] = "PRIO_PGRP";
	symbolicNamesMap[2] = "PRIO_USER";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── setitimer / getitimer which ───────────────────────────────────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForItimerWhich)
	symbolicNamesMap[0] = "ITIMER_REAL";
	symbolicNamesMap[1] = "ITIMER_VIRTUAL";
	symbolicNamesMap[2] = "ITIMER_PROF";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── clock_* clockid (macOS values — fewer than Linux) ─────────────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForClockId)
	symbolicNamesMap[0] = "CLOCK_REALTIME";
	symbolicNamesMap[1] = "CLOCK_MONOTONIC";
	symbolicNamesMap[2] = "CLOCK_PROCESS_CPUTIME_ID";
	symbolicNamesMap[3] = "CLOCK_THREAD_CPUTIME_ID";
	symbolicNamesMap[4] = "CLOCK_MONOTONIC_RAW";
	symbolicNamesMap[5] = "CLOCK_REALTIME_COARSE";
	symbolicNamesMap[6] = "CLOCK_MONOTONIC_RAW_APPROX"; // macOS
	symbolicNamesMap[7] = "CLOCK_UPTIME_RAW";           // macOS
	symbolicNamesMap[8] = "CLOCK_UPTIME_RAW_APPROX";    // macOS
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── syslog option / facility / priority (same values as Linux) ────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForOpenlogOption)
	symbolicNamesMap[0x01] = "LOG_PID";
	symbolicNamesMap[0x02] = "LOG_CONS";
	symbolicNamesMap[0x04] = "LOG_ODELAY";
	symbolicNamesMap[0x08] = "LOG_NDELAY";
	symbolicNamesMap[0x10] = "LOG_NOWAIT";
	symbolicNamesMap[0x20] = "LOG_PERROR";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForSyslogFacility)
	symbolicNamesMap[0<<3]  = "LOG_KERN";
	symbolicNamesMap[1<<3]  = "LOG_USER";
	symbolicNamesMap[2<<3]  = "LOG_MAIL";
	symbolicNamesMap[3<<3]  = "LOG_DAEMON";
	symbolicNamesMap[4<<3]  = "LOG_AUTH";
	symbolicNamesMap[5<<3]  = "LOG_SYSLOG";
	symbolicNamesMap[6<<3]  = "LOG_LPR";
	symbolicNamesMap[7<<3]  = "LOG_NEWS";
	symbolicNamesMap[8<<3]  = "LOG_UUCP";
	symbolicNamesMap[9<<3]  = "LOG_CRON";
	symbolicNamesMap[10<<3] = "LOG_AUTHPRIV";
	symbolicNamesMap[16<<3] = "LOG_LOCAL0";
	symbolicNamesMap[17<<3] = "LOG_LOCAL1";
	symbolicNamesMap[18<<3] = "LOG_LOCAL2";
	symbolicNamesMap[19<<3] = "LOG_LOCAL3";
	symbolicNamesMap[20<<3] = "LOG_LOCAL4";
	symbolicNamesMap[21<<3] = "LOG_LOCAL5";
	symbolicNamesMap[22<<3] = "LOG_LOCAL6";
	symbolicNamesMap[23<<3] = "LOG_LOCAL7";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForSyslogPriority)
	symbolicNamesMap[0] = "LOG_EMERG";
	symbolicNamesMap[1] = "LOG_ALERT";
	symbolicNamesMap[2] = "LOG_CRIT";
	symbolicNamesMap[3] = "LOG_ERR";
	symbolicNamesMap[4] = "LOG_WARNING";
	symbolicNamesMap[5] = "LOG_NOTICE";
	symbolicNamesMap[6] = "LOG_INFO";
	symbolicNamesMap[7] = "LOG_DEBUG";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── dlopen (same as Linux) ────────────────────────────────────────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForDlopenFlag)
	symbolicNamesMap[0x1] = "RTLD_LAZY";
	symbolicNamesMap[0x2] = "RTLD_NOW";
	symbolicNamesMap[0x4] = "RTLD_GLOBAL";
	symbolicNamesMap[0x8] = "RTLD_LOCAL";
	symbolicNamesMap[0x5] = "RTLD_FIRST";  // macOS only
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── Grand Central Dispatch ────────────────────────────────────────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForGCDPriority)
	symbolicNamesMap[0]   = "DISPATCH_QUEUE_PRIORITY_DEFAULT";
	symbolicNamesMap[2]   = "DISPATCH_QUEUE_PRIORITY_HIGH";
	symbolicNamesMap[-2]  = "DISPATCH_QUEUE_PRIORITY_LOW";
	symbolicNamesMap[-16] = "DISPATCH_QUEUE_PRIORITY_BACKGROUND";
	// QoS class values
	symbolicNamesMap[0x09] = "QOS_CLASS_USER_INTERACTIVE";
	symbolicNamesMap[0x19] = "QOS_CLASS_USER_INITIATED";
	symbolicNamesMap[0x15] = "QOS_CLASS_DEFAULT";
	symbolicNamesMap[0x11] = "QOS_CLASS_UTILITY";
	symbolicNamesMap[0x00] = "QOS_CLASS_BACKGROUND";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── kqueue event filter ───────────────────────────────────────────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForKqueueFilter)
	symbolicNamesMap[-1]  = "EVFILT_READ";
	symbolicNamesMap[-2]  = "EVFILT_WRITE";
	symbolicNamesMap[-3]  = "EVFILT_AIO";
	symbolicNamesMap[-4]  = "EVFILT_VNODE";
	symbolicNamesMap[-5]  = "EVFILT_PROC";
	symbolicNamesMap[-6]  = "EVFILT_SIGNAL";
	symbolicNamesMap[-7]  = "EVFILT_TIMER";
	symbolicNamesMap[-8]  = "EVFILT_MACHPORT";
	symbolicNamesMap[-9]  = "EVFILT_FS";
	symbolicNamesMap[-10] = "EVFILT_USER";
	symbolicNamesMap[-13] = "EVFILT_VM";
	symbolicNamesMap[-14] = "EVFILT_SOCK";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── kqueue EV_* action flags ──────────────────────────────────────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForKqueueFlags)
	symbolicNamesMap[0x0001] = "EV_ADD";
	symbolicNamesMap[0x0002] = "EV_DELETE";
	symbolicNamesMap[0x0004] = "EV_ENABLE";
	symbolicNamesMap[0x0008] = "EV_DISABLE";
	symbolicNamesMap[0x0010] = "EV_ONESHOT";
	symbolicNamesMap[0x0020] = "EV_CLEAR";
	symbolicNamesMap[0x0040] = "EV_RECEIPT";
	symbolicNamesMap[0x0080] = "EV_DISPATCH";
	symbolicNamesMap[0x0100] = "EV_UDATA_SPECIFIC";
	symbolicNamesMap[0x0200] = "EV_DISPATCH2";
	symbolicNamesMap[0x0400] = "EV_VANISHED";
	symbolicNamesMap[0x8000] = "EV_EOF";
	symbolicNamesMap[0x4000] = "EV_ERROR";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── sysctl top-level MIB  <sys/sysctl.h> ─────────────────────────────────────
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForSysctlMib)
	symbolicNamesMap[1] = "CTL_KERN";
	symbolicNamesMap[2] = "CTL_VM";
	symbolicNamesMap[3] = "CTL_VFS";
	symbolicNamesMap[4] = "CTL_NET";
	symbolicNamesMap[5] = "CTL_DEBUG";
	symbolicNamesMap[6] = "CTL_HW";
	symbolicNamesMap[7] = "CTL_MACHDEP";
	symbolicNamesMap[8] = "CTL_USER";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── mach_absolute_time / OSAtomicAdd32 — no integer params to annotate ────────

// ─────────────────────────────────────────────────────────────────────────────
// Main mapping table
// ─────────────────────────────────────────────────────────────────────────────
const FuncParamsMap &initFuncParamsMap() {
	static FuncParamsMap funcParamsMap;
	ParamSymbolsMap paramSymbolsMap;
	IntStringMap    symbolicNamesMap;

	// ── SEEK ─────────────────────────────────────────────────────────────
	for (auto &fn : {"fseek", "fseeko"}) {
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForSeekWhence();
		funcParamsMap[fn]  = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForSeekWhence();
		funcParamsMap["lseek"] = paramSymbolsMap;
	}

	// ── open / openat ─────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForOpenFlags();
		funcParamsMap["open"]   = paramSymbolsMap;
		funcParamsMap["creat"]  = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForOpenFlags();
		funcParamsMap["openat"] = paramSymbolsMap;
	}

	// ── mmap ──────────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForMmapProt();
		paramSymbolsMap[4] = getSymbolicNamesForMmapFlags();
		funcParamsMap["mmap"] = paramSymbolsMap;
	}

	// ── chmod ─────────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForPermMode();
		funcParamsMap["chmod"]  = paramSymbolsMap;
		funcParamsMap["fchmod"] = paramSymbolsMap;
		funcParamsMap["mkdir"]  = paramSymbolsMap;
	}

	// ── access ────────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForAccessMode();
		funcParamsMap["access"] = paramSymbolsMap;
	}

	// ── signal / kill ─────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForSignals();
		symbolicNamesMap.clear();
		symbolicNamesMap[0] = "SIG_DFL";
		symbolicNamesMap[1] = "SIG_IGN";
		paramSymbolsMap[2] = symbolicNamesMap;
		funcParamsMap["signal"]  = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForSignals();
		funcParamsMap["kill"]         = paramSymbolsMap;
		funcParamsMap["pthread_kill"] = paramSymbolsMap;
		funcParamsMap["sigqueue"]     = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForSignals();
		funcParamsMap["raise"]     = paramSymbolsMap;
		funcParamsMap["sigaction"] = paramSymbolsMap;
		funcParamsMap["psignal"]   = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForSigmaskHow();
		funcParamsMap["sigprocmask"]     = paramSymbolsMap;
		funcParamsMap["pthread_sigmask"] = paramSymbolsMap;
	}

	// ── socket ────────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForAddressFamilies();
		paramSymbolsMap[2] = getSymbolicNamesForSocketTypes();
		funcParamsMap["socket"]     = paramSymbolsMap;
		funcParamsMap["socketpair"] = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForShutdownHow();
		funcParamsMap["shutdown"] = paramSymbolsMap;
	}

	// ── fcntl ─────────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForFcntlCmd();
		funcParamsMap["fcntl"] = paramSymbolsMap;
	}

	// ── flock ─────────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForFlockOp();
		funcParamsMap["flock"] = paramSymbolsMap;
	}

	// ── wait ──────────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForWaitOptions();
		funcParamsMap["waitpid"] = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[4] = getSymbolicNamesForWaitOptions();
		funcParamsMap["waitid"] = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForWaitOptions();
		funcParamsMap["wait3"] = paramSymbolsMap;
	}

	// ── rlimit / priority ─────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForRlimitResource();
		funcParamsMap["getrlimit"] = paramSymbolsMap;
		funcParamsMap["setrlimit"] = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForPrioWhich();
		funcParamsMap["getpriority"] = paramSymbolsMap;
		funcParamsMap["setpriority"] = paramSymbolsMap;
	}

	// ── itimer / clock ────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForItimerWhich();
		funcParamsMap["setitimer"] = paramSymbolsMap;
		funcParamsMap["getitimer"] = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForClockId();
		funcParamsMap["clock_gettime"] = paramSymbolsMap;
		funcParamsMap["clock_settime"] = paramSymbolsMap;
		funcParamsMap["clock_getres"]  = paramSymbolsMap;
	}

	// ── syslog ────────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForOpenlogOption();
		paramSymbolsMap[3] = getSymbolicNamesForSyslogFacility();
		funcParamsMap["openlog"] = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForSyslogPriority();
		funcParamsMap["syslog"]  = paramSymbolsMap;
		funcParamsMap["vsyslog"] = paramSymbolsMap;
	}

	// ── dlopen ────────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForDlopenFlag();
		funcParamsMap["dlopen"] = paramSymbolsMap;
	}

	// ── GCD ───────────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForGCDPriority();
		funcParamsMap["dispatch_get_global_queue"] = paramSymbolsMap;
	}

	// ── kqueue ────────────────────────────────────────────────────────────
	// kevent(kq, changelist, nchanges, eventlist, nevents, timeout)
	// filter/flags are fields in struct kevent, not direct params of kevent(2).
	// We expose them under synthetic names for struct recovery use.
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForKqueueFilter();
		funcParamsMap["__kevent_filter"] = paramSymbolsMap;   // internal
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForKqueueFlags();
		funcParamsMap["__kevent_flags"]  = paramSymbolsMap;   // internal
	}

	// ── sysctl ────────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForSysctlMib();
		funcParamsMap["sysctl"]  = paramSymbolsMap;
		funcParamsMap["sysctlbyname"] = paramSymbolsMap;
	}

	return funcParamsMap;
}

const FuncParamsMap &FUNC_PARAMS_MAP(initFuncParamsMap());

} // anonymous namespace

/**
* @brief Implements getSymbolicNamesForParam() for macOS/BSD targets.
*/
std::optional<IntStringMap> getSymbolicNamesForParam(const std::string &funcName,
		unsigned paramPos) {
	return getSymbolicNamesForParamFromMap(funcName, paramPos, FUNC_PARAMS_MAP);
}

} // namespace macos
} // namespace semantics
} // namespace llvmir2hll
} // namespace retdec
