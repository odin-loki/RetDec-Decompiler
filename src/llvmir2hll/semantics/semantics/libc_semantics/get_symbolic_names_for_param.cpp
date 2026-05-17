/**
* @file src/llvmir2hll/semantics/semantics/libc_semantics/get_symbolic_names_for_param.cpp
* @brief Implementation of semantics::libc::getSymbolicNamesForParam() for
*        LibcSemantics.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*
* Extended to cover Linux, macOS/BSD, and POSIX symbolic constants for
* commonly decompiled functions.
*/

#include "retdec/llvmir2hll/semantics/semantics/impl_support/get_symbolic_names_for_param.h"
#include "retdec/llvmir2hll/semantics/semantics/libc_semantics/get_symbolic_names_for_param.h"

namespace retdec {
namespace llvmir2hll {
namespace semantics {
namespace libc {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Helper symbol sets reused across multiple functions
// ─────────────────────────────────────────────────────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForSeekWhence)
	// POSIX / ISO C  <stdio.h> / <unistd.h>
	symbolicNamesMap[0] = "SEEK_SET";
	symbolicNamesMap[1] = "SEEK_CUR";
	symbolicNamesMap[2] = "SEEK_END";
	// macOS/BSD extension
	symbolicNamesMap[3] = "SEEK_DATA"; // Linux 3.1+, macOS 10.12+
	symbolicNamesMap[4] = "SEEK_HOLE"; // Linux 3.1+, macOS 10.12+
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// Linux signal numbers  <signal.h>
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForLinuxSignals)
	symbolicNamesMap[1]  = "SIGHUP";
	symbolicNamesMap[2]  = "SIGINT";
	symbolicNamesMap[3]  = "SIGQUIT";
	symbolicNamesMap[4]  = "SIGILL";
	symbolicNamesMap[5]  = "SIGTRAP";
	symbolicNamesMap[6]  = "SIGABRT";
	symbolicNamesMap[7]  = "SIGBUS";
	symbolicNamesMap[8]  = "SIGFPE";
	symbolicNamesMap[9]  = "SIGKILL";
	symbolicNamesMap[10] = "SIGUSR1";
	symbolicNamesMap[11] = "SIGSEGV";
	symbolicNamesMap[12] = "SIGUSR2";
	symbolicNamesMap[13] = "SIGPIPE";
	symbolicNamesMap[14] = "SIGALRM";
	symbolicNamesMap[15] = "SIGTERM";
	symbolicNamesMap[16] = "SIGSTKFLT";
	symbolicNamesMap[17] = "SIGCHLD";
	symbolicNamesMap[18] = "SIGCONT";
	symbolicNamesMap[19] = "SIGSTOP";
	symbolicNamesMap[20] = "SIGTSTP";
	symbolicNamesMap[21] = "SIGTTIN";
	symbolicNamesMap[22] = "SIGTTOU";
	symbolicNamesMap[23] = "SIGURG";
	symbolicNamesMap[24] = "SIGXCPU";
	symbolicNamesMap[25] = "SIGXFSZ";
	symbolicNamesMap[26] = "SIGVTALRM";
	symbolicNamesMap[27] = "SIGPROF";
	symbolicNamesMap[28] = "SIGWINCH";
	symbolicNamesMap[29] = "SIGIO";
	symbolicNamesMap[30] = "SIGPWR";
	symbolicNamesMap[31] = "SIGSYS";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// macOS/BSD signal numbers  <signal.h>  (differ from Linux in numbering)
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForBSDSignals)
	symbolicNamesMap[1]  = "SIGHUP";
	symbolicNamesMap[2]  = "SIGINT";
	symbolicNamesMap[3]  = "SIGQUIT";
	symbolicNamesMap[4]  = "SIGILL";
	symbolicNamesMap[5]  = "SIGTRAP";
	symbolicNamesMap[6]  = "SIGABRT";
	symbolicNamesMap[7]  = "SIGEMT";
	symbolicNamesMap[8]  = "SIGFPE";
	symbolicNamesMap[9]  = "SIGKILL";
	symbolicNamesMap[10] = "SIGBUS";
	symbolicNamesMap[11] = "SIGSEGV";
	symbolicNamesMap[12] = "SIGSYS";
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
	symbolicNamesMap[29] = "SIGINFO";
	symbolicNamesMap[30] = "SIGUSR1";
	symbolicNamesMap[31] = "SIGUSR2";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForSigmaskHow)
	// <signal.h>
	symbolicNamesMap[0] = "SIG_BLOCK";
	symbolicNamesMap[1] = "SIG_UNBLOCK";
	symbolicNamesMap[2] = "SIG_SETMASK";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// Linux open(2) flags  <fcntl.h>  — octal constants
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForLinuxOpenFlags)
	symbolicNamesMap[00000000] = "O_RDONLY";
	symbolicNamesMap[00000001] = "O_WRONLY";
	symbolicNamesMap[00000002] = "O_RDWR";
	symbolicNamesMap[00000100] = "O_CREAT";
	symbolicNamesMap[00000200] = "O_EXCL";
	symbolicNamesMap[00000400] = "O_NOCTTY";
	symbolicNamesMap[00001000] = "O_TRUNC";
	symbolicNamesMap[00002000] = "O_APPEND";
	symbolicNamesMap[00004000] = "O_NONBLOCK";
	symbolicNamesMap[00010000] = "O_DSYNC";
	symbolicNamesMap[00020000] = "FASYNC";
	symbolicNamesMap[00040000] = "O_DIRECT";
	symbolicNamesMap[00100000] = "O_LARGEFILE";
	symbolicNamesMap[00200000] = "O_DIRECTORY";
	symbolicNamesMap[00400000] = "O_NOFOLLOW";
	symbolicNamesMap[01000000] = "O_NOATIME";
	symbolicNamesMap[02000000] = "O_CLOEXEC";
	symbolicNamesMap[04010000] = "O_SYNC";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// macOS/BSD open(2) flags  <fcntl.h>
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForBSDOpenFlags)
	symbolicNamesMap[0x0000] = "O_RDONLY";
	symbolicNamesMap[0x0001] = "O_WRONLY";
	symbolicNamesMap[0x0002] = "O_RDWR";
	symbolicNamesMap[0x0008] = "O_APPEND";
	symbolicNamesMap[0x0200] = "O_CREAT";
	symbolicNamesMap[0x0400] = "O_TRUNC";
	symbolicNamesMap[0x0800] = "O_EXCL";
	symbolicNamesMap[0x0004] = "O_NONBLOCK";
	symbolicNamesMap[0x8000] = "O_NOCTTY";
	symbolicNamesMap[0x1000000] = "O_CLOEXEC";
	symbolicNamesMap[0x0100] = "O_SYNC";
	symbolicNamesMap[0x4000000] = "O_NOFOLLOW";
	symbolicNamesMap[0x20000]   = "O_DIRECTORY";
	symbolicNamesMap[0x80000]   = "O_SYMLINK";     // macOS
	symbolicNamesMap[0x200000]  = "O_NOATIME";     // Linux compat on some BSDs
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// chmod / open mode bits
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
	// Common combos
	symbolicNamesMap[00644] = "S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH";
	symbolicNamesMap[00755] = "S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// access(2) mode
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForAccessMode)
	symbolicNamesMap[0] = "F_OK";
	symbolicNamesMap[1] = "X_OK";
	symbolicNamesMap[2] = "W_OK";
	symbolicNamesMap[4] = "R_OK";
	symbolicNamesMap[6] = "R_OK|W_OK";
	symbolicNamesMap[7] = "R_OK|W_OK|X_OK";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// mmap(2) prot
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForMmapProt)
	symbolicNamesMap[0x0] = "PROT_NONE";
	symbolicNamesMap[0x1] = "PROT_READ";
	symbolicNamesMap[0x2] = "PROT_WRITE";
	symbolicNamesMap[0x4] = "PROT_EXEC";
	symbolicNamesMap[0x3] = "PROT_READ|PROT_WRITE";
	symbolicNamesMap[0x5] = "PROT_READ|PROT_EXEC";
	symbolicNamesMap[0x7] = "PROT_READ|PROT_WRITE|PROT_EXEC";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// mmap(2) flags — Linux
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForLinuxMmapFlags)
	symbolicNamesMap[0x01]    = "MAP_SHARED";
	symbolicNamesMap[0x02]    = "MAP_PRIVATE";
	symbolicNamesMap[0x10]    = "MAP_FIXED";
	symbolicNamesMap[0x20]    = "MAP_ANONYMOUS";
	symbolicNamesMap[0x22]    = "MAP_PRIVATE|MAP_ANONYMOUS";
	symbolicNamesMap[0x21]    = "MAP_SHARED|MAP_ANONYMOUS";
	symbolicNamesMap[0x40]    = "MAP_GROWSDOWN";
	symbolicNamesMap[0x100]   = "MAP_DENYWRITE";
	symbolicNamesMap[0x200]   = "MAP_EXECUTABLE";
	symbolicNamesMap[0x400]   = "MAP_LOCKED";
	symbolicNamesMap[0x800]   = "MAP_NORESERVE";
	symbolicNamesMap[0x1000]  = "MAP_POPULATE";
	symbolicNamesMap[0x2000]  = "MAP_NONBLOCK";
	symbolicNamesMap[0x4000]  = "MAP_STACK";
	symbolicNamesMap[0x8000]  = "MAP_HUGETLB";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// mmap(2) flags — macOS/BSD
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForBSDMmapFlags)
	symbolicNamesMap[0x0001] = "MAP_SHARED";
	symbolicNamesMap[0x0002] = "MAP_PRIVATE";
	symbolicNamesMap[0x0010] = "MAP_FIXED";
	symbolicNamesMap[0x0020] = "MAP_RENAME";
	symbolicNamesMap[0x0040] = "MAP_NORESERVE";
	symbolicNamesMap[0x0080] = "MAP_INHERIT";
	symbolicNamesMap[0x0100] = "MAP_NOEXTEND";
	symbolicNamesMap[0x0200] = "MAP_HASSEMAPHORE";
	symbolicNamesMap[0x1000] = "MAP_ANON";
	symbolicNamesMap[0x4000] = "MAP_NOCACHE";    // macOS
	symbolicNamesMap[0x8000] = "MAP_JIT";        // macOS
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// socket address families
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForAddressFamilies)
	symbolicNamesMap[0]  = "AF_UNSPEC";
	symbolicNamesMap[1]  = "AF_UNIX";
	symbolicNamesMap[2]  = "AF_INET";
	symbolicNamesMap[3]  = "AF_AX25";
	symbolicNamesMap[4]  = "AF_IPX";
	symbolicNamesMap[5]  = "AF_APPLETALK";
	symbolicNamesMap[9]  = "AF_X25";
	symbolicNamesMap[10] = "AF_INET6";
	symbolicNamesMap[16] = "AF_NETLINK";
	symbolicNamesMap[17] = "AF_PACKET";
	symbolicNamesMap[29] = "AF_CAN";
	symbolicNamesMap[31] = "AF_BLUETOOTH";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// socket types
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForSocketTypes)
	symbolicNamesMap[1]  = "SOCK_STREAM";
	symbolicNamesMap[2]  = "SOCK_DGRAM";
	symbolicNamesMap[3]  = "SOCK_RAW";
	symbolicNamesMap[4]  = "SOCK_RDM";
	symbolicNamesMap[5]  = "SOCK_SEQPACKET";
	symbolicNamesMap[6]  = "SOCK_DCCP";
	symbolicNamesMap[10] = "SOCK_PACKET";
	// Linux: SOCK_CLOEXEC / SOCK_NONBLOCK may be ORed in
	symbolicNamesMap[0x80000] = "SOCK_NONBLOCK";
	symbolicNamesMap[0x100000]= "SOCK_CLOEXEC";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// IP protocols
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForIPProtocols)
	symbolicNamesMap[0]   = "IPPROTO_IP";
	symbolicNamesMap[1]   = "IPPROTO_ICMP";
	symbolicNamesMap[2]   = "IPPROTO_IGMP";
	symbolicNamesMap[4]   = "IPPROTO_IPIP";
	symbolicNamesMap[6]   = "IPPROTO_TCP";
	symbolicNamesMap[17]  = "IPPROTO_UDP";
	symbolicNamesMap[41]  = "IPPROTO_IPV6";
	symbolicNamesMap[47]  = "IPPROTO_GRE";
	symbolicNamesMap[50]  = "IPPROTO_ESP";
	symbolicNamesMap[51]  = "IPPROTO_AH";
	symbolicNamesMap[58]  = "IPPROTO_ICMPV6";
	symbolicNamesMap[132] = "IPPROTO_SCTP";
	symbolicNamesMap[255] = "IPPROTO_RAW";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// socket(2) / setsockopt(2) level
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForSockoptLevel)
	symbolicNamesMap[0]   = "IPPROTO_IP";
	symbolicNamesMap[1]   = "SOL_SOCKET";
	symbolicNamesMap[6]   = "IPPROTO_TCP";
	symbolicNamesMap[17]  = "IPPROTO_UDP";
	symbolicNamesMap[41]  = "IPPROTO_IPV6";
	symbolicNamesMap[132] = "IPPROTO_SCTP";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// SO_* options (generic, work on both Linux and BSD)
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForSocketOptions)
	// Linux values (SOL_SOCKET level)
	symbolicNamesMap[1]  = "SO_DEBUG";
	symbolicNamesMap[2]  = "SO_REUSEADDR";
	symbolicNamesMap[3]  = "SO_TYPE";
	symbolicNamesMap[4]  = "SO_ERROR";
	symbolicNamesMap[5]  = "SO_DONTROUTE";
	symbolicNamesMap[6]  = "SO_BROADCAST";
	symbolicNamesMap[7]  = "SO_SNDBUF";
	symbolicNamesMap[8]  = "SO_RCVBUF";
	symbolicNamesMap[9]  = "SO_KEEPALIVE";
	symbolicNamesMap[10] = "SO_OOBINLINE";
	symbolicNamesMap[13] = "SO_LINGER";
	symbolicNamesMap[15] = "SO_REUSEPORT";
	symbolicNamesMap[18] = "SO_RCVLOWAT";
	symbolicNamesMap[19] = "SO_SNDLOWAT";
	symbolicNamesMap[20] = "SO_RCVTIMEO";
	symbolicNamesMap[21] = "SO_SNDTIMEO";
	symbolicNamesMap[29] = "SO_TIMESTAMP";
	symbolicNamesMap[30] = "SO_ACCEPTCONN";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// IPPROTO_TCP level options
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForTCPOptions)
	symbolicNamesMap[1]  = "TCP_NODELAY";
	symbolicNamesMap[2]  = "TCP_MAXSEG";
	symbolicNamesMap[3]  = "TCP_CORK";        // Linux
	symbolicNamesMap[4]  = "TCP_KEEPIDLE";
	symbolicNamesMap[5]  = "TCP_KEEPINTVL";
	symbolicNamesMap[6]  = "TCP_KEEPCNT";
	symbolicNamesMap[7]  = "TCP_SYNCNT";
	symbolicNamesMap[8]  = "TCP_LINGER2";
	symbolicNamesMap[9]  = "TCP_DEFER_ACCEPT";
	symbolicNamesMap[10] = "TCP_WINDOW_CLAMP";
	symbolicNamesMap[11] = "TCP_INFO";
	symbolicNamesMap[12] = "TCP_QUICKACK";
	symbolicNamesMap[23] = "TCP_FASTOPEN";
	symbolicNamesMap[24] = "TCP_TIMESTAMP";
	// macOS/BSD extras
	symbolicNamesMap[0x10] = "TCP_NOPUSH";    // macOS (= TCP_CORK on Linux)
	symbolicNamesMap[0x15] = "TCP_NOOPT";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// recv/send msg flags
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForMsgFlags)
	symbolicNamesMap[0x0001] = "MSG_OOB";
	symbolicNamesMap[0x0002] = "MSG_PEEK";
	symbolicNamesMap[0x0004] = "MSG_DONTROUTE";
	symbolicNamesMap[0x0008] = "MSG_CTRUNC";
	symbolicNamesMap[0x0020] = "MSG_TRUNC";
	symbolicNamesMap[0x0040] = "MSG_DONTWAIT";
	symbolicNamesMap[0x0080] = "MSG_EOR";
	symbolicNamesMap[0x0100] = "MSG_WAITALL";
	symbolicNamesMap[0x0200] = "MSG_FIN";
	symbolicNamesMap[0x0400] = "MSG_SYN";
	symbolicNamesMap[0x0800] = "MSG_CONFIRM";
	symbolicNamesMap[0x2000] = "MSG_ERRQUEUE";
	symbolicNamesMap[0x4000] = "MSG_NOSIGNAL";
	symbolicNamesMap[0x8000] = "MSG_MORE";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// shutdown(2) how
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForShutdownHow)
	symbolicNamesMap[0] = "SHUT_RD";
	symbolicNamesMap[1] = "SHUT_WR";
	symbolicNamesMap[2] = "SHUT_RDWR";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// fcntl(2) cmd — Linux
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForFcntlCmd)
	symbolicNamesMap[0]  = "F_DUPFD";
	symbolicNamesMap[1]  = "F_GETFD";
	symbolicNamesMap[2]  = "F_SETFD";
	symbolicNamesMap[3]  = "F_GETFL";
	symbolicNamesMap[4]  = "F_SETFL";
	symbolicNamesMap[5]  = "F_GETLK";
	symbolicNamesMap[6]  = "F_SETLK";
	symbolicNamesMap[7]  = "F_SETLKW";
	symbolicNamesMap[8]  = "F_SETOWN";
	symbolicNamesMap[9]  = "F_GETOWN";
	symbolicNamesMap[10] = "F_SETSIG";
	symbolicNamesMap[11] = "F_GETSIG";
	symbolicNamesMap[12] = "F_GETLK64";
	symbolicNamesMap[13] = "F_SETLK64";
	symbolicNamesMap[14] = "F_SETLKW64";
	symbolicNamesMap[15] = "F_SETOWN_EX";
	symbolicNamesMap[16] = "F_GETOWN_EX";
	symbolicNamesMap[1030] = "F_DUPFD_CLOEXEC";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// mmap prot flags — Linux
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForMprotectProt)
	const IntStringMap &m(getSymbolicNamesForMmapProt());
	symbolicNamesMap.insert(m.begin(), m.end());
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// waitpid options
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForWaitOptions)
	symbolicNamesMap[0x01] = "WNOHANG";
	symbolicNamesMap[0x02] = "WUNTRACED";
	symbolicNamesMap[0x04] = "WEXITED";
	symbolicNamesMap[0x08] = "WCONTINUED";
	symbolicNamesMap[0x01000000] = "WNOWAIT";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// flock(2) operation
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForFlockOp)
	symbolicNamesMap[1] = "LOCK_SH";
	symbolicNamesMap[2] = "LOCK_EX";
	symbolicNamesMap[4] = "LOCK_NB";
	symbolicNamesMap[8] = "LOCK_UN";
	symbolicNamesMap[5] = "LOCK_SH|LOCK_NB";
	symbolicNamesMap[6] = "LOCK_EX|LOCK_NB";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// prctl option  <linux/prctl.h>
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForPrctlOpt)
	symbolicNamesMap[1]  = "PR_SET_PDEATHSIG";
	symbolicNamesMap[2]  = "PR_GET_PDEATHSIG";
	symbolicNamesMap[3]  = "PR_GET_DUMPABLE";
	symbolicNamesMap[4]  = "PR_SET_DUMPABLE";
	symbolicNamesMap[7]  = "PR_GET_KEEPCAPS";
	symbolicNamesMap[8]  = "PR_SET_KEEPCAPS";
	symbolicNamesMap[13] = "PR_GET_TIMING";
	symbolicNamesMap[14] = "PR_SET_TIMING";
	symbolicNamesMap[15] = "PR_SET_NAME";
	symbolicNamesMap[16] = "PR_GET_NAME";
	symbolicNamesMap[19] = "PR_GET_ENDIAN";
	symbolicNamesMap[20] = "PR_SET_ENDIAN";
	symbolicNamesMap[21] = "PR_GET_SECCOMP";
	symbolicNamesMap[22] = "PR_SET_SECCOMP";
	symbolicNamesMap[23] = "PR_CAPBSET_READ";
	symbolicNamesMap[24] = "PR_CAPBSET_DROP";
	symbolicNamesMap[29] = "PR_SET_TIMERSLACK";
	symbolicNamesMap[30] = "PR_GET_TIMERSLACK";
	symbolicNamesMap[33] = "PR_MCE_KILL";
	symbolicNamesMap[34] = "PR_MCE_KILL_GET";
	symbolicNamesMap[35] = "PR_SET_MM";
	symbolicNamesMap[36] = "PR_SET_CHILD_SUBREAPER";
	symbolicNamesMap[37] = "PR_GET_CHILD_SUBREAPER";
	symbolicNamesMap[38] = "PR_SET_NO_NEW_PRIVS";
	symbolicNamesMap[39] = "PR_GET_NO_NEW_PRIVS";
	symbolicNamesMap[40] = "PR_GET_TID_ADDRESS";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ioctl request values  <asm/ioctls.h>
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForIoctlRequest)
	// TTY
	symbolicNamesMap[0x5401] = "TCGETS";
	symbolicNamesMap[0x5402] = "TCSETS";
	symbolicNamesMap[0x5403] = "TCSETSW";
	symbolicNamesMap[0x5404] = "TCSETSF";
	symbolicNamesMap[0x5409] = "TCSBRK";
	symbolicNamesMap[0x540A] = "TCXONC";
	symbolicNamesMap[0x540B] = "TCFLSH";
	symbolicNamesMap[0x540E] = "TIOCSCTTY";
	symbolicNamesMap[0x540F] = "TIOCGPGRP";
	symbolicNamesMap[0x5410] = "TIOCSPGRP";
	symbolicNamesMap[0x5413] = "TIOCGWINSZ";
	symbolicNamesMap[0x5414] = "TIOCSWINSZ";
	symbolicNamesMap[0x541B] = "FIONREAD";
	symbolicNamesMap[0x5421] = "FIONBIO";
	symbolicNamesMap[0x5450] = "TIOCM_LE";
	// Network
	symbolicNamesMap[0x8912] = "SIOCSIFADDR";
	symbolicNamesMap[0x8913] = "SIOCGIFADDR";
	symbolicNamesMap[0x8921] = "SIOCADDRT";
	symbolicNamesMap[0x8922] = "SIOCDELRT";
	// macOS/BSD ioctl requests (different values)
	symbolicNamesMap[0x40087468] = "TIOCSWINSZ"; // macOS
	symbolicNamesMap[0x40047476] = "TIOCSPGRP";  // macOS
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// madvise(2) advice  <sys/mman.h>
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForMadviseAdvice)
	// Linux and macOS share most values
	symbolicNamesMap[0]  = "MADV_NORMAL";
	symbolicNamesMap[1]  = "MADV_RANDOM";
	symbolicNamesMap[2]  = "MADV_SEQUENTIAL";
	symbolicNamesMap[3]  = "MADV_WILLNEED";
	symbolicNamesMap[4]  = "MADV_DONTNEED";
	// Linux-only
	symbolicNamesMap[8]  = "MADV_REMOVE";
	symbolicNamesMap[9]  = "MADV_DONTFORK";
	symbolicNamesMap[10] = "MADV_DOFORK";
	symbolicNamesMap[12] = "MADV_MERGEABLE";
	symbolicNamesMap[13] = "MADV_UNMERGEABLE";
	symbolicNamesMap[14] = "MADV_HUGEPAGE";
	symbolicNamesMap[15] = "MADV_NOHUGEPAGE";
	symbolicNamesMap[16] = "MADV_DONTDUMP";
	symbolicNamesMap[17] = "MADV_DODUMP";
	symbolicNamesMap[19] = "MADV_FREE";
	symbolicNamesMap[20] = "MADV_WIPEONFORK";
	symbolicNamesMap[21] = "MADV_KEEPONFORK";
	// macOS-only
	symbolicNamesMap[5]  = "MADV_FREE";        // macOS value differs
	symbolicNamesMap[6]  = "MADV_ZERO_WIRED_PAGES"; // macOS
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// mlockall flags
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForMlockallFlags)
	symbolicNamesMap[1] = "MCL_CURRENT";
	symbolicNamesMap[2] = "MCL_FUTURE";
	symbolicNamesMap[3] = "MCL_CURRENT|MCL_FUTURE";
	symbolicNamesMap[4] = "MCL_ONFAULT"; // Linux 4.4+
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// sched_setscheduler policy
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForSchedPolicy)
	symbolicNamesMap[0] = "SCHED_OTHER";
	symbolicNamesMap[1] = "SCHED_FIFO";
	symbolicNamesMap[2] = "SCHED_RR";
	symbolicNamesMap[3] = "SCHED_BATCH";  // Linux
	symbolicNamesMap[5] = "SCHED_IDLE";   // Linux
	symbolicNamesMap[6] = "SCHED_DEADLINE"; // Linux 3.14+
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// rlimit resource  <sys/resource.h>
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForRlimitResource)
	symbolicNamesMap[0]  = "RLIMIT_CPU";
	symbolicNamesMap[1]  = "RLIMIT_FSIZE";
	symbolicNamesMap[2]  = "RLIMIT_DATA";
	symbolicNamesMap[3]  = "RLIMIT_STACK";
	symbolicNamesMap[4]  = "RLIMIT_CORE";
	symbolicNamesMap[5]  = "RLIMIT_RSS";
	symbolicNamesMap[6]  = "RLIMIT_NPROC";
	symbolicNamesMap[7]  = "RLIMIT_NOFILE";
	symbolicNamesMap[8]  = "RLIMIT_MEMLOCK";
	symbolicNamesMap[9]  = "RLIMIT_AS";
	symbolicNamesMap[10] = "RLIMIT_LOCKS";
	symbolicNamesMap[11] = "RLIMIT_SIGPENDING";
	symbolicNamesMap[12] = "RLIMIT_MSGQUEUE";
	symbolicNamesMap[13] = "RLIMIT_NICE";
	symbolicNamesMap[14] = "RLIMIT_RTPRIO";
	symbolicNamesMap[15] = "RLIMIT_RTTIME";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// getpriority / setpriority which
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForPrioWhich)
	symbolicNamesMap[0] = "PRIO_PROCESS";
	symbolicNamesMap[1] = "PRIO_PGRP";
	symbolicNamesMap[2] = "PRIO_USER";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// setitimer / getitimer which
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForItimerWhich)
	symbolicNamesMap[0] = "ITIMER_REAL";
	symbolicNamesMap[1] = "ITIMER_VIRTUAL";
	symbolicNamesMap[2] = "ITIMER_PROF";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// clock_* clockid
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForClockId)
	symbolicNamesMap[0]  = "CLOCK_REALTIME";
	symbolicNamesMap[1]  = "CLOCK_MONOTONIC";
	symbolicNamesMap[2]  = "CLOCK_PROCESS_CPUTIME_ID";
	symbolicNamesMap[3]  = "CLOCK_THREAD_CPUTIME_ID";
	symbolicNamesMap[4]  = "CLOCK_MONOTONIC_RAW";   // Linux
	symbolicNamesMap[5]  = "CLOCK_REALTIME_COARSE"; // Linux
	symbolicNamesMap[6]  = "CLOCK_MONOTONIC_COARSE";// Linux
	symbolicNamesMap[7]  = "CLOCK_BOOTTIME";        // Linux
	symbolicNamesMap[8]  = "CLOCK_REALTIME_ALARM";  // Linux
	symbolicNamesMap[9]  = "CLOCK_BOOTTIME_ALARM";  // Linux
	symbolicNamesMap[10] = "CLOCK_TAI";             // Linux 3.10+
	// macOS
	symbolicNamesMap[6]  = "CLOCK_MONOTONIC_RAW_APPROX"; // macOS (conflicts!)
	symbolicNamesMap[9]  = "CLOCK_UPTIME_RAW";          // macOS
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// timer_settime / clock_nanosleep flags
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForTimerFlags)
	symbolicNamesMap[0x01] = "TIMER_ABSTIME";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// syslog option flags
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForOpenlogOption)
	symbolicNamesMap[0x01] = "LOG_PID";
	symbolicNamesMap[0x02] = "LOG_CONS";
	symbolicNamesMap[0x04] = "LOG_ODELAY";
	symbolicNamesMap[0x08] = "LOG_NDELAY";
	symbolicNamesMap[0x10] = "LOG_NOWAIT";
	symbolicNamesMap[0x20] = "LOG_PERROR";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// syslog facility
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

// syslog priority (level)
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

// dlopen flag
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForDlopenFlag)
	symbolicNamesMap[0x00000] = "RTLD_LOCAL";
	symbolicNamesMap[0x00001] = "RTLD_LAZY";
	symbolicNamesMap[0x00002] = "RTLD_NOW";
	symbolicNamesMap[0x00004] = "RTLD_NOLOAD";
	symbolicNamesMap[0x00008] = "RTLD_DEEPBIND"; // Linux
	symbolicNamesMap[0x00100] = "RTLD_GLOBAL";
	symbolicNamesMap[0x01000] = "RTLD_NODELETE";
	// macOS differences
	symbolicNamesMap[0x00005] = "RTLD_FIRST";  // macOS
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// pthread mutex type
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForMutexType)
	symbolicNamesMap[0] = "PTHREAD_MUTEX_NORMAL";
	symbolicNamesMap[1] = "PTHREAD_MUTEX_RECURSIVE";
	symbolicNamesMap[2] = "PTHREAD_MUTEX_ERRORCHECK";
	symbolicNamesMap[3] = "PTHREAD_MUTEX_DEFAULT";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// pthread detach state
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForDetachState)
	symbolicNamesMap[0] = "PTHREAD_CREATE_JOINABLE";
	symbolicNamesMap[1] = "PTHREAD_CREATE_DETACHED";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// pthread cancel state/type
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForCancelState)
	symbolicNamesMap[0] = "PTHREAD_CANCEL_ENABLE";
	symbolicNamesMap[1] = "PTHREAD_CANCEL_DISABLE";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForCancelType)
	symbolicNamesMap[0] = "PTHREAD_CANCEL_DEFERRED";
	symbolicNamesMap[1] = "PTHREAD_CANCEL_ASYNCHRONOUS";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// posix_fadvise advice
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForFadviseAdvice)
	symbolicNamesMap[0] = "POSIX_FADV_NORMAL";
	symbolicNamesMap[1] = "POSIX_FADV_RANDOM";
	symbolicNamesMap[2] = "POSIX_FADV_SEQUENTIAL";
	symbolicNamesMap[3] = "POSIX_FADV_WILLNEED";
	symbolicNamesMap[4] = "POSIX_FADV_DONTNEED";
	symbolicNamesMap[5] = "POSIX_FADV_NOREUSE";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// tcsetattr optional_actions
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForTcsetattr)
	symbolicNamesMap[0] = "TCSANOW";
	symbolicNamesMap[1] = "TCSADRAIN";
	symbolicNamesMap[2] = "TCSAFLUSH";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// tcflow action
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForTcflowAction)
	symbolicNamesMap[0] = "TCOOFF";
	symbolicNamesMap[1] = "TCOON";
	symbolicNamesMap[2] = "TCIOFF";
	symbolicNamesMap[3] = "TCION";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// tcflush queue_selector
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForTcflushQueue)
	symbolicNamesMap[0] = "TCIFLUSH";
	symbolicNamesMap[1] = "TCOFLUSH";
	symbolicNamesMap[2] = "TCIOFLUSH";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// locale category
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForLocaleCategory)
	symbolicNamesMap[0]  = "LC_CTYPE";
	symbolicNamesMap[1]  = "LC_NUMERIC";
	symbolicNamesMap[2]  = "LC_TIME";
	symbolicNamesMap[3]  = "LC_COLLATE";
	symbolicNamesMap[4]  = "LC_MONETARY";
	symbolicNamesMap[5]  = "LC_MESSAGES";
	symbolicNamesMap[6]  = "LC_ALL";
	symbolicNamesMap[7]  = "LC_PAPER";
	symbolicNamesMap[8]  = "LC_NAME";
	symbolicNamesMap[9]  = "LC_ADDRESS";
	symbolicNamesMap[10] = "LC_TELEPHONE";
	symbolicNamesMap[11] = "LC_MEASUREMENT";
	symbolicNamesMap[12] = "LC_IDENTIFICATION";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// regcomp cflags
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForRegcompFlags)
	symbolicNamesMap[1] = "REG_EXTENDED";
	symbolicNamesMap[2] = "REG_ICASE";
	symbolicNamesMap[4] = "REG_NEWLINE";
	symbolicNamesMap[8] = "REG_NOSUB";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// regexec eflags
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForRegexecFlags)
	symbolicNamesMap[1] = "REG_NOTBOL";
	symbolicNamesMap[2] = "REG_NOTEOL";
	symbolicNamesMap[4] = "REG_STARTEND";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// IPC_* (msgctl / semctl / shmctl cmd)
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForIpcCmd)
	symbolicNamesMap[0] = "IPC_RMID";
	symbolicNamesMap[1] = "IPC_SET";
	symbolicNamesMap[2] = "IPC_STAT";
	symbolicNamesMap[3] = "IPC_INFO";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// at-flag (openat / faccessat / fstatat etc.)
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForAtFlags)
	symbolicNamesMap[0x0100] = "AT_SYMLINK_NOFOLLOW";
	symbolicNamesMap[0x0200] = "AT_REMOVEDIR";
	symbolicNamesMap[0x0400] = "AT_SYMLINK_FOLLOW";
	symbolicNamesMap[0x0800] = "AT_NO_AUTOMOUNT";
	symbolicNamesMap[0x1000] = "AT_EMPTY_PATH";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ─────────────────────────────────────────────────────────────────────────────
// macOS / BSD specific helpers
// ─────────────────────────────────────────────────────────────────────────────

// kqueue kevent filter  <sys/event.h>
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForKqueueFilter)
	symbolicNamesMap[-1]  = "EVFILT_READ";
	symbolicNamesMap[-2]  = "EVFILT_WRITE";
	symbolicNamesMap[-3]  = "EVFILT_AIO";
	symbolicNamesMap[-4]  = "EVFILT_VNODE";
	symbolicNamesMap[-5]  = "EVFILT_PROC";
	symbolicNamesMap[-6]  = "EVFILT_SIGNAL";
	symbolicNamesMap[-7]  = "EVFILT_TIMER";
	symbolicNamesMap[-8]  = "EVFILT_MACHPORT";  // macOS
	symbolicNamesMap[-9]  = "EVFILT_FS";
	symbolicNamesMap[-10] = "EVFILT_USER";
	symbolicNamesMap[-13] = "EVFILT_VM";        // macOS
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// kqueue kevent action flags  <sys/event.h>
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForKqueueFlags)
	symbolicNamesMap[0x0001] = "EV_ADD";
	symbolicNamesMap[0x0002] = "EV_DELETE";
	symbolicNamesMap[0x0004] = "EV_ENABLE";
	symbolicNamesMap[0x0008] = "EV_DISABLE";
	symbolicNamesMap[0x0010] = "EV_ONESHOT";
	symbolicNamesMap[0x0020] = "EV_CLEAR";
	symbolicNamesMap[0x0040] = "EV_RECEIPT";    // macOS 10.6+
	symbolicNamesMap[0x0080] = "EV_DISPATCH";   // macOS
	symbolicNamesMap[0x8000] = "EV_EOF";
	symbolicNamesMap[0x4000] = "EV_ERROR";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// macOS dispatch_queue_create type flags  <dispatch/queue.h>
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForDispatchQueueAttr)
	symbolicNamesMap[0] = "DISPATCH_QUEUE_SERIAL";    // NULL
	symbolicNamesMap[1] = "DISPATCH_QUEUE_CONCURRENT";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// macOS/BSD sysctl MIB top-level names  <sys/sysctl.h>
DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForSysctlMib)
	symbolicNamesMap[1]  = "CTL_KERN";
	symbolicNamesMap[2]  = "CTL_VM";
	symbolicNamesMap[3]  = "CTL_VFS";
	symbolicNamesMap[4]  = "CTL_NET";
	symbolicNamesMap[5]  = "CTL_DEBUG";
	symbolicNamesMap[6]  = "CTL_HW";
	symbolicNamesMap[7]  = "CTL_MACHDEP";
	symbolicNamesMap[8]  = "CTL_USER";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ─────────────────────────────────────────────────────────────────────────────
// Main mapping table
// ─────────────────────────────────────────────────────────────────────────────

const FuncParamsMap &initFuncParamsMap() {
	static FuncParamsMap funcParamsMap;
	ParamSymbolsMap paramSymbolsMap;
	IntStringMap    symbolicNamesMap;

	// ── fseek / lseek / fseeko / lseek64 ─────────────────────────────────
	for (auto &fn : {"fseek", "fseeko", "fseeko64"}) {
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForSeekWhence();
		funcParamsMap[fn]  = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForSeekWhence();
		funcParamsMap["lseek"]    = paramSymbolsMap;
		funcParamsMap["lseek64"]  = paramSymbolsMap;
	}
	{
		// SetFilePointer (Win32) via CRT wrapper _lseek / _lseeki64
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForSeekWhence();
		funcParamsMap["_lseek"]    = paramSymbolsMap;
		funcParamsMap["_lseeki64"] = paramSymbolsMap;
	}

	// ── open / openat (Linux) ─────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForLinuxOpenFlags();
		funcParamsMap["open"]    = paramSymbolsMap;
		funcParamsMap["open64"]  = paramSymbolsMap;
		funcParamsMap["creat"]   = paramSymbolsMap; // creat flags are limited but consistent
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForLinuxOpenFlags();
		funcParamsMap["openat"]   = paramSymbolsMap;
		funcParamsMap["openat64"] = paramSymbolsMap;
	}

	// ── open / openat (macOS/BSD) — alternate keys for BSD targets ────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForBSDOpenFlags();
		funcParamsMap["open_bsd"]   = paramSymbolsMap; // internal alias
	}

	// ── mmap (Linux) ──────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForMmapProt();
		paramSymbolsMap[4] = getSymbolicNamesForLinuxMmapFlags();
		funcParamsMap["mmap"]   = paramSymbolsMap;
		funcParamsMap["mmap64"] = paramSymbolsMap;
	}

	// ── mmap (macOS/BSD) ──────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForMmapProt();
		paramSymbolsMap[4] = getSymbolicNamesForBSDMmapFlags();
		funcParamsMap["mmap_bsd"] = paramSymbolsMap; // internal alias
	}

	// ── mprotect ──────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForMprotectProt();
		funcParamsMap["mprotect"] = paramSymbolsMap;
	}

	// ── madvise ───────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForMadviseAdvice();
		funcParamsMap["madvise"]      = paramSymbolsMap;
		funcParamsMap["posix_madvise"]= paramSymbolsMap;
	}

	// ── mlockall ──────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForMlockallFlags();
		funcParamsMap["mlockall"] = paramSymbolsMap;
	}

	// ── socket / socketpair ───────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForAddressFamilies();
		paramSymbolsMap[2] = getSymbolicNamesForSocketTypes();
		paramSymbolsMap[3] = getSymbolicNamesForIPProtocols();
		funcParamsMap["socket"]     = paramSymbolsMap;
		funcParamsMap["socketpair"] = paramSymbolsMap;
	}

	// ── setsockopt / getsockopt ───────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForSockoptLevel();
		paramSymbolsMap[3] = getSymbolicNamesForSocketOptions();
		funcParamsMap["setsockopt"] = paramSymbolsMap;
		funcParamsMap["getsockopt"] = paramSymbolsMap;
	}

	// ── send / sendto / recv / recvfrom / recvmsg / sendmsg ───────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[4] = getSymbolicNamesForMsgFlags();
		funcParamsMap["send"]    = paramSymbolsMap;
		funcParamsMap["sendto"]  = paramSymbolsMap;
		funcParamsMap["recv"]    = paramSymbolsMap;
		funcParamsMap["recvfrom"]= paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForMsgFlags();
		funcParamsMap["recvmsg"] = paramSymbolsMap;
		funcParamsMap["sendmsg"] = paramSymbolsMap;
	}

	// ── shutdown ──────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForShutdownHow();
		funcParamsMap["shutdown"] = paramSymbolsMap;
	}

	// ── bind / connect / accept (addr family in sockaddr) ─────────────────
	// Not directly applicable — addr family is inside the struct.

	// ── fcntl ─────────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForFcntlCmd();
		funcParamsMap["fcntl"]   = paramSymbolsMap;
		funcParamsMap["fcntl64"] = paramSymbolsMap;
	}

	// ── ioctl ─────────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForIoctlRequest();
		funcParamsMap["ioctl"] = paramSymbolsMap;
	}

	// ── signal / kill / raise / sigaction / pthread_kill ──────────────────
	{
		// Linux
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForLinuxSignals();
		funcParamsMap["raise"]     = paramSymbolsMap;
		funcParamsMap["sigaction"] = paramSymbolsMap;
		funcParamsMap["psignal"]   = paramSymbolsMap;
		funcParamsMap["strsignal"] = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForLinuxSignals();
		funcParamsMap["kill"]         = paramSymbolsMap;
		funcParamsMap["killpg"]       = paramSymbolsMap;
		funcParamsMap["pthread_kill"] = paramSymbolsMap;
		funcParamsMap["sigqueue"]     = paramSymbolsMap;
		funcParamsMap["sigaddset"]    = paramSymbolsMap;
		funcParamsMap["sigdelset"]    = paramSymbolsMap;
		funcParamsMap["sigismember"]  = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForLinuxSignals();
		// handler is param 2 — symbolic 0=SIG_DFL, 1=SIG_IGN
		symbolicNamesMap.clear();
		symbolicNamesMap[0] = "SIG_DFL";
		symbolicNamesMap[1] = "SIG_IGN";
		paramSymbolsMap[2] = symbolicNamesMap;
		funcParamsMap["signal"]   = paramSymbolsMap;
		funcParamsMap["ssignal"]  = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForSigmaskHow();
		funcParamsMap["sigprocmask"]  = paramSymbolsMap;
		funcParamsMap["pthread_sigmask"] = paramSymbolsMap;
	}

	// ── access / faccessat ────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForAccessMode();
		funcParamsMap["access"] = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForAccessMode();
		paramSymbolsMap[4] = getSymbolicNamesForAtFlags();
		funcParamsMap["faccessat"] = paramSymbolsMap;
	}

	// ── chmod / fchmodat ──────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForPermMode();
		funcParamsMap["chmod"]    = paramSymbolsMap;
		funcParamsMap["fchmod"]   = paramSymbolsMap;
		funcParamsMap["lchmod"]   = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForPermMode();
		paramSymbolsMap[4] = getSymbolicNamesForAtFlags();
		funcParamsMap["fchmodat"] = paramSymbolsMap;
	}

	// ── mkdir / mkdirat ───────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForPermMode();
		funcParamsMap["mkdir"]    = paramSymbolsMap;
		funcParamsMap["mkdirat"]  = paramSymbolsMap;
	}

	// ── shm_open / mq_open / sem_open ─────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForLinuxOpenFlags();
		paramSymbolsMap[3] = getSymbolicNamesForPermMode();
		funcParamsMap["shm_open"] = paramSymbolsMap;
		funcParamsMap["mq_open"]  = paramSymbolsMap;
		funcParamsMap["sem_open"] = paramSymbolsMap;
	}

	// ── fstat / fstatat / lstat ───────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[4] = getSymbolicNamesForAtFlags();
		funcParamsMap["fstatat"]  = paramSymbolsMap;
		funcParamsMap["fstatat64"]= paramSymbolsMap;
	}

	// ── unlinkat / linkat / symlinkat ─────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForAtFlags();
		funcParamsMap["unlinkat"]  = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[5] = getSymbolicNamesForAtFlags();
		funcParamsMap["linkat"]    = paramSymbolsMap;
		funcParamsMap["fchownat"]  = paramSymbolsMap;
	}

	// ── mmap open flags (shm_open etc.) ───────────────────────────────────

	// ── waitpid / waitid / wait3 / wait4 ─────────────────────────────────
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
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForWaitOptions();
		funcParamsMap["wait4"] = paramSymbolsMap;
	}

	// ── prctl ─────────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForPrctlOpt();
		funcParamsMap["prctl"] = paramSymbolsMap;
	}

	// ── flock ─────────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForFlockOp();
		funcParamsMap["flock"] = paramSymbolsMap;
	}

	// ── posix_fadvise ─────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[4] = getSymbolicNamesForFadviseAdvice();
		funcParamsMap["posix_fadvise"]   = paramSymbolsMap;
		funcParamsMap["posix_fadvise64"] = paramSymbolsMap;
	}

	// ── sched_setscheduler / sched_setparam ───────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForSchedPolicy();
		funcParamsMap["sched_setscheduler"] = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForSchedPolicy();
		funcParamsMap["sched_get_priority_max"] = paramSymbolsMap;
		funcParamsMap["sched_get_priority_min"] = paramSymbolsMap;
	}

	// ── getrlimit / setrlimit / prlimit ───────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForRlimitResource();
		funcParamsMap["getrlimit"]  = paramSymbolsMap;
		funcParamsMap["setrlimit"]  = paramSymbolsMap;
		funcParamsMap["getrlimit64"]= paramSymbolsMap;
		funcParamsMap["setrlimit64"]= paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForRlimitResource();
		funcParamsMap["prlimit"]    = paramSymbolsMap;
		funcParamsMap["prlimit64"]  = paramSymbolsMap;
	}

	// ── getpriority / setpriority ─────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForPrioWhich();
		funcParamsMap["getpriority"] = paramSymbolsMap;
		funcParamsMap["setpriority"] = paramSymbolsMap;
	}

	// ── setitimer / getitimer ─────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForItimerWhich();
		funcParamsMap["setitimer"] = paramSymbolsMap;
		funcParamsMap["getitimer"] = paramSymbolsMap;
	}

	// ── clock_gettime / clock_settime / clock_getres ──────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForClockId();
		funcParamsMap["clock_gettime"]   = paramSymbolsMap;
		funcParamsMap["clock_settime"]   = paramSymbolsMap;
		funcParamsMap["clock_getres"]    = paramSymbolsMap;
		funcParamsMap["clock_nanosleep"] = paramSymbolsMap;
	}

	// ── timer_settime ─────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForTimerFlags();
		funcParamsMap["timer_settime"] = paramSymbolsMap;
	}

	// ── openlog / syslog / vsyslog ────────────────────────────────────────
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

	// ── pthread mutex type / detach / cancel ─────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForMutexType();
		funcParamsMap["pthread_mutexattr_settype"] = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForDetachState();
		funcParamsMap["pthread_attr_setdetachstate"] = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForCancelState();
		funcParamsMap["pthread_setcancelstate"] = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForCancelType();
		funcParamsMap["pthread_setcanceltype"] = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForSchedPolicy();
		funcParamsMap["pthread_attr_setschedpolicy"] = paramSymbolsMap;
		funcParamsMap["pthread_setschedparam"]       = paramSymbolsMap;
	}

	// ── tcsetattr / tcflow / tcflush ──────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForTcsetattr();
		funcParamsMap["tcsetattr"] = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForTcflowAction();
		funcParamsMap["tcflow"] = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForTcflushQueue();
		funcParamsMap["tcflush"] = paramSymbolsMap;
	}

	// ── setlocale / newlocale ─────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForLocaleCategory();
		funcParamsMap["setlocale"] = paramSymbolsMap;
		funcParamsMap["newlocale"] = paramSymbolsMap;
	}

	// ── regcomp / regexec ─────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForRegcompFlags();
		funcParamsMap["regcomp"] = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[5] = getSymbolicNamesForRegexecFlags();
		funcParamsMap["regexec"] = paramSymbolsMap;
	}

	// ── msgctl / semctl / shmctl ──────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForIpcCmd();
		funcParamsMap["msgctl"] = paramSymbolsMap;
		funcParamsMap["shmctl"] = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForIpcCmd();
		funcParamsMap["semctl"] = paramSymbolsMap;
	}

	// ── inet_ntop / inet_pton ─────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForAddressFamilies();
		funcParamsMap["inet_ntop"]    = paramSymbolsMap;
		funcParamsMap["inet_pton"]    = paramSymbolsMap;
		funcParamsMap["inet_net_pton"]= paramSymbolsMap;
		funcParamsMap["inet_net_ntop"]= paramSymbolsMap;
	}

	// ── macOS kqueue / kevent ─────────────────────────────────────────────
	{
		// kevent(kq, changelist, nchanges, eventlist, nevents, timeout)
		// The filter field inside struct kevent — best effort via param 2 (filter)
		// We document these as reference; actual usage is inside the struct.
		paramSymbolsMap.clear();
		// No direct integer param maps for kevent itself, but
		// kqueue helpers worth documenting:
		// kevent64_s.filter is a short, not a syscall parameter.
		// Leave empty; annotated in struct recovery instead.
	}

	// ── macOS Grand Central Dispatch  dispatch_get_global_queue ───────────
	{
		paramSymbolsMap.clear();
		symbolicNamesMap.clear();
		symbolicNamesMap[0]  = "DISPATCH_QUEUE_PRIORITY_DEFAULT";
		symbolicNamesMap[2]  = "DISPATCH_QUEUE_PRIORITY_HIGH";
		symbolicNamesMap[-2] = "DISPATCH_QUEUE_PRIORITY_LOW";
		symbolicNamesMap[-16]= "DISPATCH_QUEUE_PRIORITY_BACKGROUND";
		// QoS values (newer macOS)
		symbolicNamesMap[0x09] = "QOS_CLASS_USER_INTERACTIVE";
		symbolicNamesMap[0x19] = "QOS_CLASS_USER_INITIATED";
		symbolicNamesMap[0x15] = "QOS_CLASS_DEFAULT";
		symbolicNamesMap[0x11] = "QOS_CLASS_UTILITY";
		symbolicNamesMap[0x00] = "QOS_CLASS_BACKGROUND";
		paramSymbolsMap[1] = symbolicNamesMap;
		funcParamsMap["dispatch_get_global_queue"] = paramSymbolsMap;
		funcParamsMap["dispatch_queue_create_with_target"] = paramSymbolsMap;
	}

	// ── macOS sysctl ──────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForSysctlMib();
		funcParamsMap["sysctl"] = paramSymbolsMap;
	}

	return funcParamsMap;
}

/// Mapping of function names into symbolic names of their parameters.
const FuncParamsMap &FUNC_PARAMS_MAP(initFuncParamsMap());

} // anonymous namespace

/**
* @brief Implements getSymbolicNamesForParam() for LibcSemantics.
*
* See its description for more details.
*/
std::optional<IntStringMap> getSymbolicNamesForParam(const std::string &funcName,
		unsigned paramPos) {
	return getSymbolicNamesForParamFromMap(funcName, paramPos, FUNC_PARAMS_MAP);
}

} // namespace libc
} // namespace semantics
} // namespace llvmir2hll
} // namespace retdec
