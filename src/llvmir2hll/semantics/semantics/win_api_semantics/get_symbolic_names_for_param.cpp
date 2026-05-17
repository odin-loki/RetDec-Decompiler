/**
* @file src/llvmir2hll/semantics/semantics/win_api_semantics/get_symbolic_names_for_param.cpp
* @brief Implementation of getSymbolicNamesForParam() for Windows API semantics.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*
* Covers Win32 / NT native API symbolic constants for commonly decompiled
* functions. Values sourced from WinNT.h, WinBase.h, WinSock2.h, WinIoCtl.h,
* WinReg.h, WinUser.h, and the Windows SDK headers.
*/

#include "retdec/llvmir2hll/semantics/semantics/impl_support/get_symbolic_names_for_param.h"
#include "retdec/llvmir2hll/semantics/semantics/win_api_semantics/get_symbolic_names_for_param.h"

namespace retdec {
namespace llvmir2hll {
namespace semantics {
namespace win_api {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Reusable symbol sets
// ─────────────────────────────────────────────────────────────────────────────

// ── File access / sharing / creation ─────────────────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForFileAccess)
	// Generic access rights  (WinNT.h)
	symbolicNamesMap[0x00000000] = "0";
	symbolicNamesMap[0x80000000] = "GENERIC_READ";
	symbolicNamesMap[0x40000000] = "GENERIC_WRITE";
	symbolicNamesMap[0x20000000] = "GENERIC_EXECUTE";
	symbolicNamesMap[0x10000000] = "GENERIC_ALL";
	// FILE_* specific access
	symbolicNamesMap[0x00000001] = "FILE_READ_DATA";
	symbolicNamesMap[0x00000002] = "FILE_WRITE_DATA";
	symbolicNamesMap[0x00000004] = "FILE_APPEND_DATA";
	symbolicNamesMap[0x00000008] = "FILE_READ_EA";
	symbolicNamesMap[0x00000010] = "FILE_WRITE_EA";
	symbolicNamesMap[0x00000020] = "FILE_EXECUTE";
	symbolicNamesMap[0x00000040] = "FILE_DELETE_CHILD";
	symbolicNamesMap[0x00000080] = "FILE_READ_ATTRIBUTES";
	symbolicNamesMap[0x00000100] = "FILE_WRITE_ATTRIBUTES";
	// Standard rights
	symbolicNamesMap[0x00010000] = "DELETE";
	symbolicNamesMap[0x00020000] = "READ_CONTROL";
	symbolicNamesMap[0x00040000] = "WRITE_DAC";
	symbolicNamesMap[0x00080000] = "WRITE_OWNER";
	symbolicNamesMap[0x00100000] = "SYNCHRONIZE";
	// Combined
	symbolicNamesMap[0x001f01ff] = "FILE_ALL_ACCESS";
	symbolicNamesMap[0x00120089] = "FILE_GENERIC_READ";
	symbolicNamesMap[0x00120116] = "FILE_GENERIC_WRITE";
	symbolicNamesMap[0x001200a0] = "FILE_GENERIC_EXECUTE";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForFileShareMode)
	symbolicNamesMap[0x00000000] = "0";                   // exclusive
	symbolicNamesMap[0x00000001] = "FILE_SHARE_READ";
	symbolicNamesMap[0x00000002] = "FILE_SHARE_WRITE";
	symbolicNamesMap[0x00000004] = "FILE_SHARE_DELETE";
	symbolicNamesMap[0x00000003] = "FILE_SHARE_READ|FILE_SHARE_WRITE";
	symbolicNamesMap[0x00000007] = "FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForFileCreation)
	symbolicNamesMap[1] = "CREATE_NEW";
	symbolicNamesMap[2] = "CREATE_ALWAYS";
	symbolicNamesMap[3] = "OPEN_EXISTING";
	symbolicNamesMap[4] = "OPEN_ALWAYS";
	symbolicNamesMap[5] = "TRUNCATE_EXISTING";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForFileFlagsAttribs)
	// Attributes
	symbolicNamesMap[0x00000001] = "FILE_ATTRIBUTE_READONLY";
	symbolicNamesMap[0x00000002] = "FILE_ATTRIBUTE_HIDDEN";
	symbolicNamesMap[0x00000004] = "FILE_ATTRIBUTE_SYSTEM";
	symbolicNamesMap[0x00000010] = "FILE_ATTRIBUTE_DIRECTORY";
	symbolicNamesMap[0x00000020] = "FILE_ATTRIBUTE_ARCHIVE";
	symbolicNamesMap[0x00000040] = "FILE_ATTRIBUTE_DEVICE";
	symbolicNamesMap[0x00000080] = "FILE_ATTRIBUTE_NORMAL";
	symbolicNamesMap[0x00000100] = "FILE_ATTRIBUTE_TEMPORARY";
	symbolicNamesMap[0x00000200] = "FILE_ATTRIBUTE_SPARSE_FILE";
	symbolicNamesMap[0x00000400] = "FILE_ATTRIBUTE_REPARSE_POINT";
	symbolicNamesMap[0x00000800] = "FILE_ATTRIBUTE_COMPRESSED";
	symbolicNamesMap[0x00001000] = "FILE_ATTRIBUTE_OFFLINE";
	symbolicNamesMap[0x00002000] = "FILE_ATTRIBUTE_NOT_CONTENT_INDEXED";
	symbolicNamesMap[0x00004000] = "FILE_ATTRIBUTE_ENCRYPTED";
	symbolicNamesMap[0x00008000] = "FILE_ATTRIBUTE_INTEGRITY_STREAM";
	symbolicNamesMap[0x00020000] = "FILE_ATTRIBUTE_NO_SCRUB_DATA";
	// Flags
	symbolicNamesMap[0x00010000] = "FILE_FLAG_OPEN_REQUIRING_OPLOCK";
	symbolicNamesMap[0x00100000] = "FILE_FLAG_OPEN_NO_RECALL";
	symbolicNamesMap[0x00200000] = "FILE_FLAG_OPEN_REPARSE_POINT";
	symbolicNamesMap[0x00400000] = "FILE_FLAG_SESSION_AWARE";
	symbolicNamesMap[0x00800000] = "FILE_FLAG_POSIX_SEMANTICS";
	symbolicNamesMap[0x01000000] = "FILE_FLAG_BACKUP_SEMANTICS";
	symbolicNamesMap[0x02000000] = "FILE_FLAG_DELETE_ON_CLOSE";
	symbolicNamesMap[0x04000000] = "FILE_FLAG_SEQUENTIAL_SCAN";
	symbolicNamesMap[0x08000000] = "FILE_FLAG_RANDOM_ACCESS";
	symbolicNamesMap[0x10000000] = "FILE_FLAG_NO_BUFFERING";
	symbolicNamesMap[0x20000000] = "FILE_FLAG_OVERLAPPED";
	symbolicNamesMap[0x80000000] = "FILE_FLAG_WRITE_THROUGH";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── SetFilePointer / SetFilePointerEx move method ─────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForMoveMethod)
	symbolicNamesMap[0] = "FILE_BEGIN";
	symbolicNamesMap[1] = "FILE_CURRENT";
	symbolicNamesMap[2] = "FILE_END";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── VirtualAlloc / VirtualProtect page access ─────────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForPageProtection)
	symbolicNamesMap[0x01] = "PAGE_NOACCESS";
	symbolicNamesMap[0x02] = "PAGE_READONLY";
	symbolicNamesMap[0x04] = "PAGE_READWRITE";
	symbolicNamesMap[0x08] = "PAGE_WRITECOPY";
	symbolicNamesMap[0x10] = "PAGE_EXECUTE";
	symbolicNamesMap[0x20] = "PAGE_EXECUTE_READ";
	symbolicNamesMap[0x40] = "PAGE_EXECUTE_READWRITE";
	symbolicNamesMap[0x80] = "PAGE_EXECUTE_WRITECOPY";
	// Modifiers (ORed with base protection)
	symbolicNamesMap[0x100] = "PAGE_GUARD";
	symbolicNamesMap[0x200] = "PAGE_NOCACHE";
	symbolicNamesMap[0x400] = "PAGE_WRITECOMBINE";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForAllocationType)
	symbolicNamesMap[0x00001000] = "MEM_COMMIT";
	symbolicNamesMap[0x00002000] = "MEM_RESERVE";
	symbolicNamesMap[0x00004000] = "MEM_DECOMMIT";
	symbolicNamesMap[0x00008000] = "MEM_RELEASE";
	symbolicNamesMap[0x00010000] = "MEM_FREE";
	symbolicNamesMap[0x00020000] = "MEM_PRIVATE";
	symbolicNamesMap[0x00040000] = "MEM_MAPPED";
	symbolicNamesMap[0x00080000] = "MEM_RESET";
	symbolicNamesMap[0x00100000] = "MEM_TOP_DOWN";
	symbolicNamesMap[0x00200000] = "MEM_WRITE_WATCH";
	symbolicNamesMap[0x00400000] = "MEM_PHYSICAL";
	symbolicNamesMap[0x00800000] = "MEM_ROTATE";
	symbolicNamesMap[0x01000000] = "MEM_LARGE_PAGES";
	symbolicNamesMap[0x04000000] = "MEM_4MB_PAGES";
	// Combined common patterns
	symbolicNamesMap[0x00003000] = "MEM_COMMIT|MEM_RESERVE";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForFreeType)
	symbolicNamesMap[0x4000] = "MEM_DECOMMIT";
	symbolicNamesMap[0x8000] = "MEM_RELEASE";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── Process / thread creation flags ──────────────────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForProcessCreationFlags)
	symbolicNamesMap[0x00000000] = "0";
	symbolicNamesMap[0x00000001] = "DEBUG_PROCESS";
	symbolicNamesMap[0x00000002] = "DEBUG_ONLY_THIS_PROCESS";
	symbolicNamesMap[0x00000004] = "CREATE_SUSPENDED";
	symbolicNamesMap[0x00000008] = "DETACHED_PROCESS";
	symbolicNamesMap[0x00000010] = "CREATE_NEW_CONSOLE";
	symbolicNamesMap[0x00000020] = "NORMAL_PRIORITY_CLASS";
	symbolicNamesMap[0x00000040] = "IDLE_PRIORITY_CLASS";
	symbolicNamesMap[0x00000080] = "HIGH_PRIORITY_CLASS";
	symbolicNamesMap[0x00000100] = "REALTIME_PRIORITY_CLASS";
	symbolicNamesMap[0x00000200] = "CREATE_NEW_PROCESS_GROUP";
	symbolicNamesMap[0x00000400] = "CREATE_UNICODE_ENVIRONMENT";
	symbolicNamesMap[0x00000800] = "CREATE_SEPARATE_WOW_VDM";
	symbolicNamesMap[0x00001000] = "CREATE_SHARED_WOW_VDM";
	symbolicNamesMap[0x00002000] = "CREATE_FORCEDOS";
	symbolicNamesMap[0x00004000] = "BELOW_NORMAL_PRIORITY_CLASS";
	symbolicNamesMap[0x00008000] = "ABOVE_NORMAL_PRIORITY_CLASS";
	symbolicNamesMap[0x00010000] = "INHERIT_PARENT_AFFINITY";
	symbolicNamesMap[0x00040000] = "CREATE_PROTECTED_PROCESS";
	symbolicNamesMap[0x00080000] = "EXTENDED_STARTUPINFO_PRESENT";
	symbolicNamesMap[0x00100000] = "PROCESS_MODE_BACKGROUND_BEGIN";
	symbolicNamesMap[0x00200000] = "PROCESS_MODE_BACKGROUND_END";
	symbolicNamesMap[0x00400000] = "CREATE_SECURE_PROCESS";
	symbolicNamesMap[0x04000000] = "CREATE_BREAKAWAY_FROM_JOB";
	symbolicNamesMap[0x08000000] = "CREATE_PRESERVE_CODE_AUTHZ_LEVEL";
	symbolicNamesMap[0x10000000] = "CREATE_DEFAULT_ERROR_MODE";
	symbolicNamesMap[0x20000000] = "CREATE_NO_WINDOW";
	symbolicNamesMap[0x40000000] = "PROFILE_USER";
	symbolicNamesMap[0x80000000] = "PROFILE_KERNEL";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── Thread priority ────────────────────────────────────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForThreadPriority)
	symbolicNamesMap[-15] = "THREAD_PRIORITY_IDLE";
	symbolicNamesMap[-2]  = "THREAD_PRIORITY_LOWEST";
	symbolicNamesMap[-1]  = "THREAD_PRIORITY_BELOW_NORMAL";
	symbolicNamesMap[0]   = "THREAD_PRIORITY_NORMAL";
	symbolicNamesMap[1]   = "THREAD_PRIORITY_ABOVE_NORMAL";
	symbolicNamesMap[2]   = "THREAD_PRIORITY_HIGHEST";
	symbolicNamesMap[15]  = "THREAD_PRIORITY_TIME_CRITICAL";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── WAIT_* / WaitForSingleObject timeout / return ─────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForWaitTimeout)
	symbolicNamesMap[0x00000000] = "0";
	symbolicNamesMap[0x000000FF] = "WAIT_ABANDONED";
	symbolicNamesMap[0xFFFFFFFF] = "INFINITE";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── Registry hives / access ────────────────────────────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForRegistryHive)
	// HKEY predefined handles  (WinReg.h)
	symbolicNamesMap[0x80000000] = "HKEY_CLASSES_ROOT";
	symbolicNamesMap[0x80000001] = "HKEY_CURRENT_USER";
	symbolicNamesMap[0x80000002] = "HKEY_LOCAL_MACHINE";
	symbolicNamesMap[0x80000003] = "HKEY_USERS";
	symbolicNamesMap[0x80000004] = "HKEY_PERFORMANCE_DATA";
	symbolicNamesMap[0x80000005] = "HKEY_CURRENT_CONFIG";
	symbolicNamesMap[0x80000006] = "HKEY_DYN_DATA";
	symbolicNamesMap[0x80000007] = "HKEY_CURRENT_USER_LOCAL_SETTINGS";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForRegistryAccess)
	symbolicNamesMap[0x00000001] = "KEY_QUERY_VALUE";
	symbolicNamesMap[0x00000002] = "KEY_SET_VALUE";
	symbolicNamesMap[0x00000004] = "KEY_CREATE_SUB_KEY";
	symbolicNamesMap[0x00000008] = "KEY_ENUMERATE_SUB_KEYS";
	symbolicNamesMap[0x00000010] = "KEY_NOTIFY";
	symbolicNamesMap[0x00000020] = "KEY_CREATE_LINK";
	symbolicNamesMap[0x00000100] = "KEY_WOW64_64KEY";
	symbolicNamesMap[0x00000200] = "KEY_WOW64_32KEY";
	symbolicNamesMap[0x000f003f] = "KEY_ALL_ACCESS";
	symbolicNamesMap[0x00020019] = "KEY_READ";
	symbolicNamesMap[0x00020006] = "KEY_WRITE";
	symbolicNamesMap[0x00020001] = "KEY_EXECUTE";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForRegistryType)
	symbolicNamesMap[0]  = "REG_NONE";
	symbolicNamesMap[1]  = "REG_SZ";
	symbolicNamesMap[2]  = "REG_EXPAND_SZ";
	symbolicNamesMap[3]  = "REG_BINARY";
	symbolicNamesMap[4]  = "REG_DWORD";
	// symbolicNamesMap[4] = "REG_DWORD_LITTLE_ENDIAN"; // synonym
	symbolicNamesMap[5]  = "REG_DWORD_BIG_ENDIAN";
	symbolicNamesMap[6]  = "REG_LINK";
	symbolicNamesMap[7]  = "REG_MULTI_SZ";
	symbolicNamesMap[8]  = "REG_RESOURCE_LIST";
	symbolicNamesMap[9]  = "REG_FULL_RESOURCE_DESCRIPTOR";
	symbolicNamesMap[10] = "REG_RESOURCE_REQUIREMENTS_LIST";
	symbolicNamesMap[11] = "REG_QWORD";
	// symbolicNamesMap[11]= "REG_QWORD_LITTLE_ENDIAN"; // synonym
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForRegOpenOptions)
	symbolicNamesMap[0x00000000] = "REG_OPTION_NON_VOLATILE";
	symbolicNamesMap[0x00000001] = "REG_OPTION_VOLATILE";
	symbolicNamesMap[0x00000002] = "REG_OPTION_CREATE_LINK";
	symbolicNamesMap[0x00000004] = "REG_OPTION_BACKUP_RESTORE";
	symbolicNamesMap[0x00000008] = "REG_OPTION_OPEN_LINK";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── WinSock2 ──────────────────────────────────────────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForWSAAddressFamily)
	symbolicNamesMap[0]  = "AF_UNSPEC";
	symbolicNamesMap[2]  = "AF_INET";
	symbolicNamesMap[6]  = "AF_IPX";
	symbolicNamesMap[16] = "AF_APPLETALK";
	symbolicNamesMap[17] = "AF_NETBIOS";
	symbolicNamesMap[23] = "AF_INET6";
	symbolicNamesMap[26] = "AF_IRDA";
	symbolicNamesMap[32] = "AF_BTH";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForWSASocketType)
	symbolicNamesMap[1]  = "SOCK_STREAM";
	symbolicNamesMap[2]  = "SOCK_DGRAM";
	symbolicNamesMap[3]  = "SOCK_RAW";
	symbolicNamesMap[4]  = "SOCK_RDM";
	symbolicNamesMap[5]  = "SOCK_SEQPACKET";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForWSAProtocol)
	symbolicNamesMap[0]   = "IPPROTO_HOPOPTS";
	symbolicNamesMap[1]   = "IPPROTO_ICMP";
	symbolicNamesMap[2]   = "IPPROTO_IGMP";
	symbolicNamesMap[3]   = "BTHPROTO_RFCOMM";
	symbolicNamesMap[6]   = "IPPROTO_TCP";
	symbolicNamesMap[17]  = "IPPROTO_UDP";
	symbolicNamesMap[41]  = "IPPROTO_IPV6";
	symbolicNamesMap[43]  = "IPPROTO_ROUTING";
	symbolicNamesMap[44]  = "IPPROTO_FRAGMENT";
	symbolicNamesMap[50]  = "IPPROTO_ESP";
	symbolicNamesMap[51]  = "IPPROTO_AH";
	symbolicNamesMap[58]  = "IPPROTO_ICMPV6";
	symbolicNamesMap[59]  = "IPPROTO_NONE";
	symbolicNamesMap[60]  = "IPPROTO_DSTOPTS";
	symbolicNamesMap[77]  = "IPPROTO_ND";
	symbolicNamesMap[103] = "IPPROTO_PIM";
	symbolicNamesMap[113] = "IPPROTO_PGM";
	symbolicNamesMap[256] = "IPPROTO_RAW";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForWSAFlags)
	symbolicNamesMap[0x01] = "WSA_FLAG_OVERLAPPED";
	symbolicNamesMap[0x02] = "WSA_FLAG_MULTIPOINT_C_ROOT";
	symbolicNamesMap[0x04] = "WSA_FLAG_MULTIPOINT_C_LEAF";
	symbolicNamesMap[0x08] = "WSA_FLAG_MULTIPOINT_D_ROOT";
	symbolicNamesMap[0x10] = "WSA_FLAG_MULTIPOINT_D_LEAF";
	symbolicNamesMap[0x20] = "WSA_FLAG_ACCESS_SYSTEM_SECURITY";
	symbolicNamesMap[0x40] = "WSA_FLAG_NO_HANDLE_INHERIT";
	symbolicNamesMap[0x80] = "WSA_FLAG_REGISTERED_IO";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForWSASendRecvFlags)
	symbolicNamesMap[0x0001] = "MSG_OOB";
	symbolicNamesMap[0x0002] = "MSG_PEEK";
	symbolicNamesMap[0x0004] = "MSG_DONTROUTE";
	symbolicNamesMap[0x0008] = "MSG_WAITALL";
	symbolicNamesMap[0x0010] = "MSG_PUSH_IMMEDIATE";
	symbolicNamesMap[0x0020] = "MSG_PARTIAL";
	symbolicNamesMap[0x0040] = "MSG_INTERRUPT";
	symbolicNamesMap[0x8000] = "MSG_MAXIOVLEN";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForWSAShutdown)
	symbolicNamesMap[0] = "SD_RECEIVE";
	symbolicNamesMap[1] = "SD_SEND";
	symbolicNamesMap[2] = "SD_BOTH";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForWSAIoctlCommand)
	// ioctlsocket / WSAIoctl command codes
	symbolicNamesMap[0x8004667e] = "FIONBIO";
	symbolicNamesMap[0x4004667f] = "FIONREAD";
	symbolicNamesMap[0x80040130] = "FIOASYNC";
	symbolicNamesMap[0x8004667d] = "SIOCATMARK";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForSOLevel)
	symbolicNamesMap[0xffff] = "SOL_SOCKET";
	symbolicNamesMap[0]      = "IPPROTO_IP";
	symbolicNamesMap[6]      = "IPPROTO_TCP";
	symbolicNamesMap[17]     = "IPPROTO_UDP";
	symbolicNamesMap[41]     = "IPPROTO_IPV6";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForSOOption)
	// SOL_SOCKET level options (WinSock2.h)
	symbolicNamesMap[0x0001] = "SO_DEBUG";
	symbolicNamesMap[0x0002] = "SO_ACCEPTCONN";
	symbolicNamesMap[0x0004] = "SO_REUSEADDR";
	symbolicNamesMap[0x0008] = "SO_KEEPALIVE";
	symbolicNamesMap[0x0010] = "SO_DONTROUTE";
	symbolicNamesMap[0x0020] = "SO_BROADCAST";
	symbolicNamesMap[0x0040] = "SO_USELOOPBACK";
	symbolicNamesMap[0x0080] = "SO_LINGER";
	symbolicNamesMap[0x0100] = "SO_OOBINLINE";
	symbolicNamesMap[0x1001] = "SO_SNDBUF";
	symbolicNamesMap[0x1002] = "SO_RCVBUF";
	symbolicNamesMap[0x1003] = "SO_SNDLOWAT";
	symbolicNamesMap[0x1004] = "SO_RCVLOWAT";
	symbolicNamesMap[0x1005] = "SO_SNDTIMEO";
	symbolicNamesMap[0x1006] = "SO_RCVTIMEO";
	symbolicNamesMap[0x1007] = "SO_ERROR";
	symbolicNamesMap[0x1008] = "SO_TYPE";
	symbolicNamesMap[0x1009] = "SO_BSP_STATE";
	symbolicNamesMap[0x3001] = "SO_GROUP_ID";
	symbolicNamesMap[0x3002] = "SO_GROUP_PRIORITY";
	symbolicNamesMap[0x3003] = "SO_MAX_MSG_SIZE";
	symbolicNamesMap[0x700b] = "SO_UPDATE_ACCEPT_CONTEXT";
	symbolicNamesMap[0x700c] = "SO_UPDATE_CONNECT_CONTEXT";
	symbolicNamesMap[0x701a] = "SO_EXCLUSIVEADDRUSE";
	symbolicNamesMap[0x700f] = "SO_REUSEPORT";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── LoadLibrary / GetModuleHandle / LoadLibraryEx flags ───────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForLoadLibraryFlags)
	symbolicNamesMap[0x00000000] = "0";
	symbolicNamesMap[0x00000001] = "DONT_RESOLVE_DLL_REFERENCES";
	symbolicNamesMap[0x00000002] = "LOAD_LIBRARY_AS_DATAFILE";
	symbolicNamesMap[0x00000004] = "LOAD_WITH_ALTERED_SEARCH_PATH";
	symbolicNamesMap[0x00000008] = "LOAD_IGNORE_CODE_AUTHZ_LEVEL";
	symbolicNamesMap[0x00000010] = "LOAD_LIBRARY_AS_IMAGE_RESOURCE";
	symbolicNamesMap[0x00000020] = "LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE";
	symbolicNamesMap[0x00000040] = "LOAD_LIBRARY_REQUIRE_SIGNED_TARGET";
	symbolicNamesMap[0x00000080] = "LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR";
	symbolicNamesMap[0x00000100] = "LOAD_LIBRARY_SEARCH_APPLICATION_DIR";
	symbolicNamesMap[0x00000200] = "LOAD_LIBRARY_SEARCH_USER_DIRS";
	symbolicNamesMap[0x00000400] = "LOAD_LIBRARY_SEARCH_SYSTEM32";
	symbolicNamesMap[0x00000800] = "LOAD_LIBRARY_SEARCH_DEFAULT_DIRS";
	symbolicNamesMap[0x00001000] = "LOAD_LIBRARY_SAFE_CURRENT_DIRS";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── FormatMessage flags ────────────────────────────────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForFormatMessageFlags)
	symbolicNamesMap[0x00000100] = "FORMAT_MESSAGE_ALLOCATE_BUFFER";
	symbolicNamesMap[0x00000200] = "FORMAT_MESSAGE_IGNORE_INSERTS";
	symbolicNamesMap[0x00000400] = "FORMAT_MESSAGE_FROM_STRING";
	symbolicNamesMap[0x00000800] = "FORMAT_MESSAGE_FROM_HMODULE";
	symbolicNamesMap[0x00001000] = "FORMAT_MESSAGE_FROM_SYSTEM";
	symbolicNamesMap[0x00002000] = "FORMAT_MESSAGE_ARGUMENT_ARRAY";
	symbolicNamesMap[0x00004000] = "FORMAT_MESSAGE_MAX_WIDTH_MASK";
	// Common combo
	symbolicNamesMap[0x00001200] = "FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── Window messages (partial — most common) ────────────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForWindowMessage)
	symbolicNamesMap[0x0000] = "WM_NULL";
	symbolicNamesMap[0x0001] = "WM_CREATE";
	symbolicNamesMap[0x0002] = "WM_DESTROY";
	symbolicNamesMap[0x0003] = "WM_MOVE";
	symbolicNamesMap[0x0005] = "WM_SIZE";
	symbolicNamesMap[0x0006] = "WM_ACTIVATE";
	symbolicNamesMap[0x0007] = "WM_SETFOCUS";
	symbolicNamesMap[0x0008] = "WM_KILLFOCUS";
	symbolicNamesMap[0x000A] = "WM_ENABLE";
	symbolicNamesMap[0x000B] = "WM_SETREDRAW";
	symbolicNamesMap[0x000C] = "WM_SETTEXT";
	symbolicNamesMap[0x000D] = "WM_GETTEXT";
	symbolicNamesMap[0x000E] = "WM_GETTEXTLENGTH";
	symbolicNamesMap[0x000F] = "WM_PAINT";
	symbolicNamesMap[0x0010] = "WM_CLOSE";
	symbolicNamesMap[0x0011] = "WM_QUERYENDSESSION";
	symbolicNamesMap[0x0012] = "WM_QUIT";
	symbolicNamesMap[0x0013] = "WM_QUERYOPEN";
	symbolicNamesMap[0x0014] = "WM_ERASEBKGND";
	symbolicNamesMap[0x0015] = "WM_SYSCOLORCHANGE";
	symbolicNamesMap[0x0016] = "WM_ENDSESSION";
	symbolicNamesMap[0x0018] = "WM_SHOWWINDOW";
	symbolicNamesMap[0x001A] = "WM_WININICHANGE";
	symbolicNamesMap[0x001B] = "WM_DEVMODECHANGE";
	symbolicNamesMap[0x001C] = "WM_ACTIVATEAPP";
	symbolicNamesMap[0x001D] = "WM_FONTCHANGE";
	symbolicNamesMap[0x001E] = "WM_TIMECHANGE";
	symbolicNamesMap[0x001F] = "WM_CANCELMODE";
	symbolicNamesMap[0x0020] = "WM_SETCURSOR";
	symbolicNamesMap[0x0021] = "WM_MOUSEACTIVATE";
	symbolicNamesMap[0x0024] = "WM_GETMINMAXINFO";
	symbolicNamesMap[0x0081] = "WM_NCCREATE";
	symbolicNamesMap[0x0082] = "WM_NCDESTROY";
	symbolicNamesMap[0x0083] = "WM_NCCALCSIZE";
	symbolicNamesMap[0x0084] = "WM_NCHITTEST";
	symbolicNamesMap[0x0085] = "WM_NCPAINT";
	symbolicNamesMap[0x0086] = "WM_NCACTIVATE";
	symbolicNamesMap[0x0087] = "WM_GETDLGCODE";
	symbolicNamesMap[0x00A0] = "WM_NCMOUSEMOVE";
	symbolicNamesMap[0x00A1] = "WM_NCLBUTTONDOWN";
	symbolicNamesMap[0x00A2] = "WM_NCLBUTTONUP";
	symbolicNamesMap[0x00A3] = "WM_NCLBUTTONDBLCLK";
	symbolicNamesMap[0x0100] = "WM_KEYDOWN";
	symbolicNamesMap[0x0101] = "WM_KEYUP";
	symbolicNamesMap[0x0102] = "WM_CHAR";
	symbolicNamesMap[0x0103] = "WM_DEADCHAR";
	symbolicNamesMap[0x0104] = "WM_SYSKEYDOWN";
	symbolicNamesMap[0x0105] = "WM_SYSKEYUP";
	symbolicNamesMap[0x0106] = "WM_SYSCHAR";
	symbolicNamesMap[0x0107] = "WM_SYSDEADCHAR";
	symbolicNamesMap[0x0108] = "WM_KEYLAST";
	symbolicNamesMap[0x0110] = "WM_INITDIALOG";
	symbolicNamesMap[0x0111] = "WM_COMMAND";
	symbolicNamesMap[0x0112] = "WM_SYSCOMMAND";
	symbolicNamesMap[0x0113] = "WM_TIMER";
	symbolicNamesMap[0x0114] = "WM_HSCROLL";
	symbolicNamesMap[0x0115] = "WM_VSCROLL";
	symbolicNamesMap[0x0116] = "WM_INITMENU";
	symbolicNamesMap[0x0117] = "WM_INITMENUPOPUP";
	symbolicNamesMap[0x011F] = "WM_MENUSELECT";
	symbolicNamesMap[0x0120] = "WM_MENUCHAR";
	symbolicNamesMap[0x0121] = "WM_ENTERIDLE";
	symbolicNamesMap[0x0200] = "WM_MOUSEMOVE";
	symbolicNamesMap[0x0201] = "WM_LBUTTONDOWN";
	symbolicNamesMap[0x0202] = "WM_LBUTTONUP";
	symbolicNamesMap[0x0203] = "WM_LBUTTONDBLCLK";
	symbolicNamesMap[0x0204] = "WM_RBUTTONDOWN";
	symbolicNamesMap[0x0205] = "WM_RBUTTONUP";
	symbolicNamesMap[0x0206] = "WM_RBUTTONDBLCLK";
	symbolicNamesMap[0x0207] = "WM_MBUTTONDOWN";
	symbolicNamesMap[0x0208] = "WM_MBUTTONUP";
	symbolicNamesMap[0x0209] = "WM_MBUTTONDBLCLK";
	symbolicNamesMap[0x020A] = "WM_MOUSEWHEEL";
	symbolicNamesMap[0x0210] = "WM_PARENTNOTIFY";
	symbolicNamesMap[0x0211] = "WM_ENTERMENULOOP";
	symbolicNamesMap[0x0212] = "WM_EXITMENULOOP";
	symbolicNamesMap[0x0213] = "WM_NEXTMENU";
	symbolicNamesMap[0x0231] = "WM_ENTERSIZEMOVE";
	symbolicNamesMap[0x0232] = "WM_EXITSIZEMOVE";
	symbolicNamesMap[0x0233] = "WM_DROPFILES";
	symbolicNamesMap[0x0281] = "WM_IME_SETCONTEXT";
	symbolicNamesMap[0x0282] = "WM_IME_NOTIFY";
	symbolicNamesMap[0x0300] = "WM_CUT";
	symbolicNamesMap[0x0301] = "WM_COPY";
	symbolicNamesMap[0x0302] = "WM_PASTE";
	symbolicNamesMap[0x0303] = "WM_CLEAR";
	symbolicNamesMap[0x0304] = "WM_UNDO";
	symbolicNamesMap[0x0305] = "WM_RENDERFORMAT";
	symbolicNamesMap[0x0306] = "WM_RENDERALLFORMATS";
	symbolicNamesMap[0x0307] = "WM_DESTROYCLIPBOARD";
	symbolicNamesMap[0x0308] = "WM_DRAWCLIPBOARD";
	symbolicNamesMap[0x0400] = "WM_USER";
	symbolicNamesMap[0x8000] = "WM_APP";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── ShowWindow nCmdShow ────────────────────────────────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForShowWindow)
	symbolicNamesMap[0]  = "SW_HIDE";
	symbolicNamesMap[1]  = "SW_SHOWNORMAL";
	symbolicNamesMap[2]  = "SW_SHOWMINIMIZED";
	symbolicNamesMap[3]  = "SW_SHOWMAXIMIZED";
	symbolicNamesMap[4]  = "SW_SHOWNOACTIVATE";
	symbolicNamesMap[5]  = "SW_SHOW";
	symbolicNamesMap[6]  = "SW_MINIMIZE";
	symbolicNamesMap[7]  = "SW_SHOWMINNOACTIVE";
	symbolicNamesMap[8]  = "SW_SHOWNA";
	symbolicNamesMap[9]  = "SW_RESTORE";
	symbolicNamesMap[10] = "SW_SHOWDEFAULT";
	symbolicNamesMap[11] = "SW_FORCEMINIMIZE";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── MessageBox flags ───────────────────────────────────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForMessageBoxType)
	// Buttons
	symbolicNamesMap[0x00000000] = "MB_OK";
	symbolicNamesMap[0x00000001] = "MB_OKCANCEL";
	symbolicNamesMap[0x00000002] = "MB_ABORTRETRYIGNORE";
	symbolicNamesMap[0x00000003] = "MB_YESNOCANCEL";
	symbolicNamesMap[0x00000004] = "MB_YESNO";
	symbolicNamesMap[0x00000005] = "MB_RETRYCANCEL";
	symbolicNamesMap[0x00000006] = "MB_CANCELTRYCONTINUE";
	// Icons
	symbolicNamesMap[0x00000010] = "MB_ICONHAND";
	symbolicNamesMap[0x00000020] = "MB_ICONQUESTION";
	symbolicNamesMap[0x00000030] = "MB_ICONEXCLAMATION";
	symbolicNamesMap[0x00000040] = "MB_ICONASTERISK";
	// Default button
	symbolicNamesMap[0x00000100] = "MB_DEFBUTTON2";
	symbolicNamesMap[0x00000200] = "MB_DEFBUTTON3";
	symbolicNamesMap[0x00000300] = "MB_DEFBUTTON4";
	// Modality
	symbolicNamesMap[0x00001000] = "MB_APPLMODAL";
	symbolicNamesMap[0x00002000] = "MB_SYSTEMMODAL";
	symbolicNamesMap[0x00004000] = "MB_TASKMODAL";
	// Misc
	symbolicNamesMap[0x00008000] = "MB_HELP";
	symbolicNamesMap[0x00010000] = "MB_NOFOCUS";
	symbolicNamesMap[0x00020000] = "MB_SETFOREGROUND";
	symbolicNamesMap[0x00040000] = "MB_DEFAULT_DESKTOP_ONLY";
	symbolicNamesMap[0x00080000] = "MB_TOPMOST";
	symbolicNamesMap[0x00100000] = "MB_RIGHT";
	symbolicNamesMap[0x00200000] = "MB_RTLREADING";
	symbolicNamesMap[0x00400000] = "MB_SERVICE_NOTIFICATION";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── CreateMutex / CreateEvent / CreateSemaphore initial ownership ─────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForBoolParam)
	symbolicNamesMap[0] = "FALSE";
	symbolicNamesMap[1] = "TRUE";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── GetLastError / error codes (most common) ───────────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForWin32Error)
	symbolicNamesMap[0]     = "ERROR_SUCCESS";
	symbolicNamesMap[1]     = "ERROR_INVALID_FUNCTION";
	symbolicNamesMap[2]     = "ERROR_FILE_NOT_FOUND";
	symbolicNamesMap[3]     = "ERROR_PATH_NOT_FOUND";
	symbolicNamesMap[4]     = "ERROR_TOO_MANY_OPEN_FILES";
	symbolicNamesMap[5]     = "ERROR_ACCESS_DENIED";
	symbolicNamesMap[6]     = "ERROR_INVALID_HANDLE";
	symbolicNamesMap[7]     = "ERROR_ARENA_TRASHED";
	symbolicNamesMap[8]     = "ERROR_NOT_ENOUGH_MEMORY";
	symbolicNamesMap[9]     = "ERROR_INVALID_BLOCK";
	symbolicNamesMap[10]    = "ERROR_BAD_ENVIRONMENT";
	symbolicNamesMap[11]    = "ERROR_BAD_FORMAT";
	symbolicNamesMap[12]    = "ERROR_INVALID_ACCESS";
	symbolicNamesMap[13]    = "ERROR_INVALID_DATA";
	symbolicNamesMap[14]    = "ERROR_OUTOFMEMORY";
	symbolicNamesMap[15]    = "ERROR_INVALID_DRIVE";
	symbolicNamesMap[16]    = "ERROR_CURRENT_DIRECTORY";
	symbolicNamesMap[17]    = "ERROR_NOT_SAME_DEVICE";
	symbolicNamesMap[18]    = "ERROR_NO_MORE_FILES";
	symbolicNamesMap[19]    = "ERROR_WRITE_PROTECT";
	symbolicNamesMap[20]    = "ERROR_BAD_UNIT";
	symbolicNamesMap[21]    = "ERROR_NOT_READY";
	symbolicNamesMap[22]    = "ERROR_BAD_COMMAND";
	symbolicNamesMap[23]    = "ERROR_CRC";
	symbolicNamesMap[24]    = "ERROR_BAD_LENGTH";
	symbolicNamesMap[25]    = "ERROR_SEEK";
	symbolicNamesMap[26]    = "ERROR_NOT_DOS_DISK";
	symbolicNamesMap[27]    = "ERROR_SECTOR_NOT_FOUND";
	symbolicNamesMap[28]    = "ERROR_OUT_OF_PAPER";
	symbolicNamesMap[29]    = "ERROR_WRITE_FAULT";
	symbolicNamesMap[30]    = "ERROR_READ_FAULT";
	symbolicNamesMap[31]    = "ERROR_GEN_FAILURE";
	symbolicNamesMap[32]    = "ERROR_SHARING_VIOLATION";
	symbolicNamesMap[33]    = "ERROR_LOCK_VIOLATION";
	symbolicNamesMap[34]    = "ERROR_WRONG_DISK";
	symbolicNamesMap[36]    = "ERROR_SHARING_BUFFER_EXCEEDED";
	symbolicNamesMap[38]    = "ERROR_HANDLE_EOF";
	symbolicNamesMap[39]    = "ERROR_HANDLE_DISK_FULL";
	symbolicNamesMap[87]    = "ERROR_INVALID_PARAMETER";
	symbolicNamesMap[109]   = "ERROR_BROKEN_PIPE";
	symbolicNamesMap[122]   = "ERROR_INSUFFICIENT_BUFFER";
	symbolicNamesMap[123]   = "ERROR_INVALID_NAME";
	symbolicNamesMap[152]   = "ERROR_INVALID_FLAG_NUMBER";
	symbolicNamesMap[183]   = "ERROR_ALREADY_EXISTS";
	symbolicNamesMap[206]   = "ERROR_FILENAME_EXCED_RANGE";
	symbolicNamesMap[234]   = "ERROR_MORE_DATA";
	symbolicNamesMap[258]   = "WAIT_TIMEOUT";
	symbolicNamesMap[259]   = "ERROR_NO_MORE_ITEMS";
	symbolicNamesMap[288]   = "ERROR_NOT_OWNER";
	symbolicNamesMap[298]   = "ERROR_TOO_MANY_POSTS";
	symbolicNamesMap[299]   = "ERROR_PARTIAL_COPY";
	symbolicNamesMap[317]   = "ERROR_MR_MID_NOT_FOUND";
	symbolicNamesMap[487]   = "ERROR_INVALID_ADDRESS";
	symbolicNamesMap[534]   = "ERROR_ARITHMETIC_OVERFLOW";
	symbolicNamesMap[536]   = "ERROR_PIPE_NOT_CONNECTED";
	symbolicNamesMap[997]   = "ERROR_IO_PENDING";
	symbolicNamesMap[998]   = "ERROR_NOACCESS";
	symbolicNamesMap[1001]  = "ERROR_STACK_OVERFLOW";
	symbolicNamesMap[1005]  = "ERROR_UNRECOGNIZED_VOLUME";
	symbolicNamesMap[1006]  = "ERROR_FILE_INVALID";
	symbolicNamesMap[1008]  = "ERROR_NO_TOKEN";
	symbolicNamesMap[1168]  = "ERROR_NOT_FOUND";
	symbolicNamesMap[1317]  = "ERROR_NONE_MAPPED";
	symbolicNamesMap[1392]  = "ERROR_FILE_CORRUPT";
	symbolicNamesMap[1400]  = "ERROR_INVALID_WINDOW_HANDLE";
	symbolicNamesMap[1460]  = "ERROR_TIMEOUT";
	symbolicNamesMap[1500]  = "ERROR_EVENTLOG_FILE_CORRUPT";
	symbolicNamesMap[1816]  = "ERROR_NOT_ENOUGH_QUOTA";
	symbolicNamesMap[3221225477] = "STATUS_ACCESS_VIOLATION"; // NTSTATUS
	symbolicNamesMap[3221225495] = "STATUS_INTEGER_OVERFLOW";
	symbolicNamesMap[3221225786] = "STATUS_STACK_BUFFER_OVERRUN";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── NTSTATUS codes (NtCreateFile, NtQueryInformationProcess, etc.) ────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForNtStatus)
	symbolicNamesMap[0x00000000] = "STATUS_SUCCESS";
	symbolicNamesMap[0x00000001] = "STATUS_WAIT_1";
	symbolicNamesMap[0x00000080] = "STATUS_ABANDONED_WAIT_0";
	symbolicNamesMap[0x000000C0] = "STATUS_USER_APC";
	symbolicNamesMap[0x00000100] = "STATUS_ALERTED";
	symbolicNamesMap[0x00000101] = "STATUS_TIMEOUT";
	symbolicNamesMap[0x00000102] = "STATUS_PENDING";
	symbolicNamesMap[0x00010002] = "STATUS_REPARSE";
	symbolicNamesMap[0x40000000] = "STATUS_OBJECT_NAME_EXISTS";
	symbolicNamesMap[0x40000016] = "STATUS_THREAD_WAS_SUSPENDED";
	symbolicNamesMap[0x80000001] = "STATUS_GUARD_PAGE_VIOLATION";
	symbolicNamesMap[0x80000002] = "STATUS_DATATYPE_MISALIGNMENT";
	symbolicNamesMap[0x80000003] = "STATUS_BREAKPOINT";
	symbolicNamesMap[0x80000004] = "STATUS_SINGLE_STEP";
	symbolicNamesMap[0x80000005] = "STATUS_BUFFER_OVERFLOW";
	symbolicNamesMap[0x80000006] = "STATUS_NO_MORE_FILES";
	symbolicNamesMap[0xC0000001] = "STATUS_UNSUCCESSFUL";
	symbolicNamesMap[0xC0000002] = "STATUS_NOT_IMPLEMENTED";
	symbolicNamesMap[0xC0000004] = "STATUS_INFO_LENGTH_MISMATCH";
	symbolicNamesMap[0xC0000005] = "STATUS_ACCESS_VIOLATION";
	symbolicNamesMap[0xC0000006] = "STATUS_IN_PAGE_ERROR";
	symbolicNamesMap[0xC0000008] = "STATUS_INVALID_HANDLE";
	symbolicNamesMap[0xC000000D] = "STATUS_INVALID_PARAMETER";
	symbolicNamesMap[0xC000000E] = "STATUS_NO_SUCH_DEVICE";
	symbolicNamesMap[0xC000000F] = "STATUS_NO_SUCH_FILE";
	symbolicNamesMap[0xC0000010] = "STATUS_INVALID_DEVICE_REQUEST";
	symbolicNamesMap[0xC0000011] = "STATUS_END_OF_FILE";
	symbolicNamesMap[0xC0000013] = "STATUS_NO_MEDIA_IN_DEVICE";
	symbolicNamesMap[0xC0000017] = "STATUS_NO_MEMORY";
	symbolicNamesMap[0xC0000018] = "STATUS_CONFLICTING_ADDRESSES";
	symbolicNamesMap[0xC0000022] = "STATUS_ACCESS_DENIED";
	symbolicNamesMap[0xC0000023] = "STATUS_BUFFER_TOO_SMALL";
	symbolicNamesMap[0xC0000024] = "STATUS_OBJECT_TYPE_MISMATCH";
	symbolicNamesMap[0xC0000034] = "STATUS_OBJECT_NAME_NOT_FOUND";
	symbolicNamesMap[0xC0000035] = "STATUS_OBJECT_NAME_COLLISION";
	symbolicNamesMap[0xC000003A] = "STATUS_OBJECT_PATH_NOT_FOUND";
	symbolicNamesMap[0xC000003B] = "STATUS_OBJECT_PATH_SYNTAX_BAD";
	symbolicNamesMap[0xC000003E] = "STATUS_DATA_ERROR";
	symbolicNamesMap[0xC000003F] = "STATUS_CRC_ERROR";
	symbolicNamesMap[0xC0000043] = "STATUS_SHARING_VIOLATION";
	symbolicNamesMap[0xC000004E] = "STATUS_PIPE_NOT_AVAILABLE";
	symbolicNamesMap[0xC000007B] = "STATUS_INVALID_IMAGE_FORMAT";
	symbolicNamesMap[0xC000007E] = "STATUS_RANGE_NOT_LOCKED";
	symbolicNamesMap[0xC000007F] = "STATUS_DISK_FULL";
	symbolicNamesMap[0xC0000091] = "STATUS_FLOAT_OVERFLOW";
	symbolicNamesMap[0xC0000094] = "STATUS_INTEGER_DIVIDE_BY_ZERO";
	symbolicNamesMap[0xC0000095] = "STATUS_INTEGER_OVERFLOW";
	symbolicNamesMap[0xC00000BB] = "STATUS_NOT_SUPPORTED";
	symbolicNamesMap[0xC00000BE] = "STATUS_BAD_NETWORK_PATH";
	symbolicNamesMap[0xC00000C4] = "STATUS_UNEXPECTED_NETWORK_ERROR";
	symbolicNamesMap[0xC00000D0] = "STATUS_PIPE_DISCONNECTED";
	symbolicNamesMap[0xC00000FE] = "STATUS_MAPPED_FILE_SIZE_ZERO";
	symbolicNamesMap[0xC0000101] = "STATUS_DIRECTORY_NOT_EMPTY";
	symbolicNamesMap[0xC0000102] = "STATUS_FILE_CORRUPT_ERROR";
	symbolicNamesMap[0xC0000103] = "STATUS_NOT_A_DIRECTORY";
	symbolicNamesMap[0xC0000120] = "STATUS_CANCELLED";
	symbolicNamesMap[0xC0000121] = "STATUS_CANNOT_DELETE";
	symbolicNamesMap[0xC0000142] = "STATUS_DLL_INIT_FAILED";
	symbolicNamesMap[0xC000014B] = "STATUS_PIPE_BROKEN";
	symbolicNamesMap[0xC0000185] = "STATUS_IO_DEVICE_ERROR";
	symbolicNamesMap[0xC0000194] = "STATUS_POSSIBLE_DEADLOCK";
	symbolicNamesMap[0xC00001AD] = "STATUS_STACK_BUFFER_OVERRUN";
	symbolicNamesMap[0xC0000409] = "STATUS_STACK_BUFFER_OVERRUN";
	symbolicNamesMap[0xC0000906] = "STATUS_FATAL_USER_CALLBACK_EXCEPTION";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── HeapCreate / HeapAlloc flags ──────────────────────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForHeapCreateFlags)
	symbolicNamesMap[0x00000000] = "0";
	symbolicNamesMap[0x00000001] = "HEAP_NO_SERIALIZE";
	symbolicNamesMap[0x00000002] = "HEAP_GROWABLE";
	symbolicNamesMap[0x00000004] = "HEAP_GENERATE_EXCEPTIONS";
	symbolicNamesMap[0x00000008] = "HEAP_ZERO_MEMORY";
	symbolicNamesMap[0x00000010] = "HEAP_REALLOC_IN_PLACE_ONLY";
	symbolicNamesMap[0x00000020] = "HEAP_TAIL_CHECKING_ENABLED";
	symbolicNamesMap[0x00000040] = "HEAP_FREE_CHECKING_ENABLED";
	symbolicNamesMap[0x00000080] = "HEAP_DISABLE_COALESCE_ON_FREE";
	symbolicNamesMap[0x00000400] = "HEAP_CREATE_ALIGN_16";
	symbolicNamesMap[0x00000800] = "HEAP_CREATE_ENABLE_TRACING";
	symbolicNamesMap[0x00001000] = "HEAP_CREATE_ENABLE_EXECUTE";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── CryptAcquireContext provider type ─────────────────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForCryptProvType)
	symbolicNamesMap[1]  = "PROV_RSA_FULL";
	symbolicNamesMap[2]  = "PROV_RSA_SIG";
	symbolicNamesMap[3]  = "PROV_DSS";
	symbolicNamesMap[4]  = "PROV_FORTEZZA";
	symbolicNamesMap[5]  = "PROV_MS_EXCHANGE";
	symbolicNamesMap[6]  = "PROV_SSL";
	symbolicNamesMap[12] = "PROV_RSA_SCHANNEL";
	symbolicNamesMap[13] = "PROV_DSS_DH";
	symbolicNamesMap[14] = "PROV_EC_ECDSA_SIG";
	symbolicNamesMap[15] = "PROV_EC_ECNRA_SIG";
	symbolicNamesMap[16] = "PROV_EC_ECDSA_FULL";
	symbolicNamesMap[17] = "PROV_EC_ECNRA_FULL";
	symbolicNamesMap[18] = "PROV_DH_SCHANNEL";
	symbolicNamesMap[20] = "PROV_SPYRUS_LYNKS";
	symbolicNamesMap[21] = "PROV_RNG";
	symbolicNamesMap[22] = "PROV_INTEL_SEC";
	symbolicNamesMap[23] = "PROV_REPLACE_OWF";
	symbolicNamesMap[24] = "PROV_RSA_AES";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForCryptAcquireFlags)
	symbolicNamesMap[0x00000001] = "CRYPT_VERIFYCONTEXT";
	symbolicNamesMap[0x00000008] = "CRYPT_NEWKEYSET";
	symbolicNamesMap[0x00000010] = "CRYPT_DELETEKEYSET";
	symbolicNamesMap[0x00000020] = "CRYPT_MACHINE_KEYSET";
	symbolicNamesMap[0x00000040] = "CRYPT_SILENT";
	symbolicNamesMap[0x00000080] = "CRYPT_DEFAULT_CONTAINER_OPTIONAL";
	symbolicNamesMap[0xF0000001] = "CRYPT_VERIFYCONTEXT";  // common combo
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForCryptAlgId)
	// Hash algorithms
	symbolicNamesMap[0x8001] = "CALG_MD2";
	symbolicNamesMap[0x8002] = "CALG_MD4";
	symbolicNamesMap[0x8003] = "CALG_MD5";
	symbolicNamesMap[0x8004] = "CALG_SHA";
	// symbolicNamesMap[0x8004] = "CALG_SHA1"; // synonym
	symbolicNamesMap[0x800c] = "CALG_SHA_256";
	symbolicNamesMap[0x800d] = "CALG_SHA_384";
	symbolicNamesMap[0x800e] = "CALG_SHA_512";
	symbolicNamesMap[0x8005] = "CALG_MAC";
	symbolicNamesMap[0x8009] = "CALG_HMAC";
	// Key exchange
	symbolicNamesMap[0xa400] = "CALG_RSA_KEYX";
	symbolicNamesMap[0x2200] = "CALG_DH_SF";
	symbolicNamesMap[0xaa01] = "CALG_ECDH";
	// Signature
	symbolicNamesMap[0x2400] = "CALG_RSA_SIGN";
	symbolicNamesMap[0x2200] = "CALG_DSS_SIGN";
	symbolicNamesMap[0xaa05] = "CALG_ECDSA";
	// Symmetric
	symbolicNamesMap[0x6601] = "CALG_DES";
	symbolicNamesMap[0x6603] = "CALG_3DES";
	symbolicNamesMap[0x6609] = "CALG_AES_128";
	symbolicNamesMap[0x660e] = "CALG_AES_192";
	symbolicNamesMap[0x660f] = "CALG_AES_256";
	symbolicNamesMap[0x6610] = "CALG_AES";
	symbolicNamesMap[0x6602] = "CALG_RC2";
	symbolicNamesMap[0x6801] = "CALG_RC4";
	symbolicNamesMap[0x6802] = "CALG_RC5";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── SHGetFolderPath / SHGetKnownFolderPath CSIDL ─────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForCSIDL)
	symbolicNamesMap[0x0000] = "CSIDL_DESKTOP";
	symbolicNamesMap[0x0002] = "CSIDL_PROGRAMS";
	symbolicNamesMap[0x0005] = "CSIDL_PERSONAL";
	symbolicNamesMap[0x0006] = "CSIDL_FAVORITES";
	symbolicNamesMap[0x0007] = "CSIDL_STARTUP";
	symbolicNamesMap[0x0008] = "CSIDL_RECENT";
	symbolicNamesMap[0x0009] = "CSIDL_SENDTO";
	symbolicNamesMap[0x000a] = "CSIDL_BITBUCKET";
	symbolicNamesMap[0x000b] = "CSIDL_STARTMENU";
	symbolicNamesMap[0x000d] = "CSIDL_MYDOCUMENTS";
	symbolicNamesMap[0x000e] = "CSIDL_MYMUSIC";
	symbolicNamesMap[0x000f] = "CSIDL_MYVIDEO";
	symbolicNamesMap[0x0011] = "CSIDL_DESKTOPDIRECTORY";
	symbolicNamesMap[0x0012] = "CSIDL_DRIVES";
	symbolicNamesMap[0x0013] = "CSIDL_NETWORK";
	symbolicNamesMap[0x0014] = "CSIDL_NETHOOD";
	symbolicNamesMap[0x0015] = "CSIDL_FONTS";
	symbolicNamesMap[0x0016] = "CSIDL_TEMPLATES";
	symbolicNamesMap[0x0017] = "CSIDL_COMMON_STARTMENU";
	symbolicNamesMap[0x0018] = "CSIDL_COMMON_PROGRAMS";
	symbolicNamesMap[0x0019] = "CSIDL_COMMON_STARTUP";
	symbolicNamesMap[0x001a] = "CSIDL_COMMON_DESKTOPDIRECTORY";
	symbolicNamesMap[0x001b] = "CSIDL_APPDATA";
	symbolicNamesMap[0x001c] = "CSIDL_PRINTHOOD";
	symbolicNamesMap[0x001d] = "CSIDL_LOCAL_APPDATA";
	symbolicNamesMap[0x001e] = "CSIDL_ALTSTARTUP";
	symbolicNamesMap[0x001f] = "CSIDL_COMMON_ALTSTARTUP";
	symbolicNamesMap[0x0020] = "CSIDL_COMMON_FAVORITES";
	symbolicNamesMap[0x0021] = "CSIDL_INTERNET_CACHE";
	symbolicNamesMap[0x0022] = "CSIDL_COOKIES";
	symbolicNamesMap[0x0023] = "CSIDL_HISTORY";
	symbolicNamesMap[0x0024] = "CSIDL_COMMON_APPDATA";
	symbolicNamesMap[0x0025] = "CSIDL_WINDOWS";
	symbolicNamesMap[0x0026] = "CSIDL_SYSTEM";
	symbolicNamesMap[0x0027] = "CSIDL_PROGRAM_FILES";
	symbolicNamesMap[0x0028] = "CSIDL_MYPICTURES";
	symbolicNamesMap[0x0029] = "CSIDL_PROFILE";
	symbolicNamesMap[0x002a] = "CSIDL_SYSTEMX86";
	symbolicNamesMap[0x002b] = "CSIDL_PROGRAM_FILESX86";
	symbolicNamesMap[0x002c] = "CSIDL_PROGRAM_FILES_COMMON";
	symbolicNamesMap[0x002d] = "CSIDL_PROGRAM_FILES_COMMONX86";
	symbolicNamesMap[0x002e] = "CSIDL_COMMON_TEMPLATES";
	symbolicNamesMap[0x002f] = "CSIDL_COMMON_DOCUMENTS";
	symbolicNamesMap[0x0030] = "CSIDL_COMMON_ADMINTOOLS";
	symbolicNamesMap[0x0031] = "CSIDL_ADMINTOOLS";
	symbolicNamesMap[0x0035] = "CSIDL_COMMON_MUSIC";
	symbolicNamesMap[0x0036] = "CSIDL_COMMON_PICTURES";
	symbolicNamesMap[0x0037] = "CSIDL_COMMON_VIDEO";
	symbolicNamesMap[0x003a] = "CSIDL_RESOURCES";
	symbolicNamesMap[0x003b] = "CSIDL_RESOURCES_LOCALIZED";
	symbolicNamesMap[0x003e] = "CSIDL_COMMON_OEM_LINKS";
	symbolicNamesMap[0x003f] = "CSIDL_CDBURN_AREA";
	symbolicNamesMap[0x0041] = "CSIDL_COMPUTERSNEARME";
	symbolicNamesMap[0x8000] = "CSIDL_FLAG_CREATE";
	symbolicNamesMap[0x4000] = "CSIDL_FLAG_DONT_VERIFY";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── CreateFile pipe access / CreateNamedPipe ──────────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForPipeAccess)
	symbolicNamesMap[0x00000001] = "FILE_FLAG_FIRST_PIPE_INSTANCE";
	symbolicNamesMap[0x40000000] = "PIPE_ACCESS_INBOUND";
	symbolicNamesMap[0x80000000] = "PIPE_ACCESS_OUTBOUND";
	symbolicNamesMap[0xC0000000] = "PIPE_ACCESS_DUPLEX";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForPipeMode)
	symbolicNamesMap[0x00] = "PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT";
	symbolicNamesMap[0x01] = "PIPE_TYPE_MESSAGE";
	symbolicNamesMap[0x02] = "PIPE_READMODE_MESSAGE";
	symbolicNamesMap[0x04] = "PIPE_NOWAIT";
	symbolicNamesMap[0x08] = "PIPE_ACCEPT_REMOTE_CLIENTS";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── IOCTL device codes (DeviceIoControl) ──────────────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForIoControlCode)
	// Storage / Disk
	symbolicNamesMap[0x00560000] = "FSCTL_GET_COMPRESSION";
	symbolicNamesMap[0x00560014] = "FSCTL_QUERY_ALLOCATED_RANGES";
	symbolicNamesMap[0x00560017] = "FSCTL_SET_SPARSE";
	symbolicNamesMap[0x2d1080]   = "IOCTL_STORAGE_QUERY_PROPERTY";
	symbolicNamesMap[0x2d0c00]   = "IOCTL_STORAGE_GET_DEVICE_NUMBER";
	symbolicNamesMap[0x70000]    = "IOCTL_DISK_GET_DRIVE_GEOMETRY";
	symbolicNamesMap[0x70040]    = "IOCTL_DISK_GET_PARTITION_INFO";
	symbolicNamesMap[0x7c080]    = "IOCTL_DISK_GET_DRIVE_GEOMETRY_EX";
	// Network
	symbolicNamesMap[0x15004]    = "IOCTL_TCP_QUERY_INFORMATION_EX";
	// Keyboard / HID
	symbolicNamesMap[0x0b0041]   = "IOCTL_KEYBOARD_QUERY_ATTRIBUTES";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── Console / GetStdHandle ─────────────────────────────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForStdHandle)
	symbolicNamesMap[0xFFFFFFF6] = "STD_INPUT_HANDLE";
	symbolicNamesMap[0xFFFFFFF5] = "STD_OUTPUT_HANDLE";
	symbolicNamesMap[0xFFFFFFF4] = "STD_ERROR_HANDLE";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ── SECURITY_ATTRIBUTES / SECURITY_INFORMATION ────────────────────────────────

DEFINE_GET_SYMBOLIC_NAMES_FUNC_BEGIN(getSymbolicNamesForSecurityInfo)
	symbolicNamesMap[0x00000001] = "OWNER_SECURITY_INFORMATION";
	symbolicNamesMap[0x00000002] = "GROUP_SECURITY_INFORMATION";
	symbolicNamesMap[0x00000004] = "DACL_SECURITY_INFORMATION";
	symbolicNamesMap[0x00000008] = "SACL_SECURITY_INFORMATION";
	symbolicNamesMap[0x00000010] = "LABEL_SECURITY_INFORMATION";
	symbolicNamesMap[0x00000020] = "ATTRIBUTE_SECURITY_INFORMATION";
	symbolicNamesMap[0x00000040] = "SCOPE_SECURITY_INFORMATION";
	symbolicNamesMap[0x00000100] = "PROCESS_TRUST_LABEL_SECURITY_INFORMATION";
	symbolicNamesMap[0x00001000] = "BACKUP_SECURITY_INFORMATION";
	symbolicNamesMap[0x80000000] = "PROTECTED_DACL_SECURITY_INFORMATION";
	symbolicNamesMap[0x40000000] = "PROTECTED_SACL_SECURITY_INFORMATION";
	symbolicNamesMap[0x20000000] = "UNPROTECTED_DACL_SECURITY_INFORMATION";
	symbolicNamesMap[0x10000000] = "UNPROTECTED_SACL_SECURITY_INFORMATION";
DEFINE_GET_SYMBOLIC_NAMES_FUNC_END()

// ─────────────────────────────────────────────────────────────────────────────
// Main mapping table
// ─────────────────────────────────────────────────────────────────────────────

const FuncParamsMap &initFuncParamsMap() {
	static FuncParamsMap funcParamsMap;
	ParamSymbolsMap paramSymbolsMap;
	IntStringMap    symbolicNamesMap;

	// ── CreateFile / CreateFileA / CreateFileW ────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForFileAccess();
		paramSymbolsMap[3] = getSymbolicNamesForFileShareMode();
		paramSymbolsMap[5] = getSymbolicNamesForFileCreation();
		paramSymbolsMap[6] = getSymbolicNamesForFileFlagsAttribs();
		funcParamsMap["CreateFileA"] = paramSymbolsMap;
		funcParamsMap["CreateFileW"] = paramSymbolsMap;
		funcParamsMap["CreateFile"]  = paramSymbolsMap;
	}

	// ── SetFilePointer / SetFilePointerEx ─────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[4] = getSymbolicNamesForMoveMethod();
		funcParamsMap["SetFilePointer"]  = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForMoveMethod();
		funcParamsMap["SetFilePointerEx"] = paramSymbolsMap;
	}

	// ── VirtualAlloc / VirtualAllocEx ─────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForAllocationType();
		paramSymbolsMap[4] = getSymbolicNamesForPageProtection();
		funcParamsMap["VirtualAlloc"]   = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[4] = getSymbolicNamesForAllocationType();
		paramSymbolsMap[5] = getSymbolicNamesForPageProtection();
		funcParamsMap["VirtualAllocEx"]  = paramSymbolsMap;
	}

	// ── VirtualFree / VirtualFreeEx ───────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForFreeType();
		funcParamsMap["VirtualFree"]    = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[4] = getSymbolicNamesForFreeType();
		funcParamsMap["VirtualFreeEx"]  = paramSymbolsMap;
	}

	// ── VirtualProtect / VirtualProtectEx ─────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForPageProtection();
		funcParamsMap["VirtualProtect"]  = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[4] = getSymbolicNamesForPageProtection();
		funcParamsMap["VirtualProtectEx"] = paramSymbolsMap;
	}

	// ── CreateProcess / CreateProcessA / CreateProcessW ───────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[7]  = getSymbolicNamesForBoolParam();  // bInheritHandles
		paramSymbolsMap[8]  = getSymbolicNamesForProcessCreationFlags();
		funcParamsMap["CreateProcessA"] = paramSymbolsMap;
		funcParamsMap["CreateProcessW"] = paramSymbolsMap;
		funcParamsMap["CreateProcess"]  = paramSymbolsMap;
	}

	// ── SetThreadPriority / GetThreadPriority ─────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForThreadPriority();
		funcParamsMap["SetThreadPriority"] = paramSymbolsMap;
	}

	// ── WaitForSingleObject / WaitForSingleObjectEx ───────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForWaitTimeout();
		funcParamsMap["WaitForSingleObject"]   = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForWaitTimeout();
		paramSymbolsMap[3] = getSymbolicNamesForBoolParam();  // bAlertable
		funcParamsMap["WaitForSingleObjectEx"] = paramSymbolsMap;
	}

	// ── WaitForMultipleObjects / WaitForMultipleObjectsEx ─────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForBoolParam();  // bWaitAll
		paramSymbolsMap[4] = getSymbolicNamesForWaitTimeout();
		funcParamsMap["WaitForMultipleObjects"] = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForBoolParam();  // bWaitAll
		paramSymbolsMap[4] = getSymbolicNamesForWaitTimeout();
		paramSymbolsMap[5] = getSymbolicNamesForBoolParam();  // bAlertable
		funcParamsMap["WaitForMultipleObjectsEx"] = paramSymbolsMap;
	}

	// ── Registry ──────────────────────────────────────────────────────────
	{
		// RegOpenKeyEx / RegOpenKeyExA / RegOpenKeyExW
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForRegistryHive();
		paramSymbolsMap[4] = getSymbolicNamesForRegistryAccess();
		funcParamsMap["RegOpenKeyExA"] = paramSymbolsMap;
		funcParamsMap["RegOpenKeyExW"] = paramSymbolsMap;
		funcParamsMap["RegOpenKeyEx"]  = paramSymbolsMap;
	}
	{
		// RegCreateKeyEx
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForRegistryHive();
		paramSymbolsMap[5] = getSymbolicNamesForRegOpenOptions();
		paramSymbolsMap[6] = getSymbolicNamesForRegistryAccess();
		funcParamsMap["RegCreateKeyExA"] = paramSymbolsMap;
		funcParamsMap["RegCreateKeyExW"] = paramSymbolsMap;
		funcParamsMap["RegCreateKeyEx"]  = paramSymbolsMap;
	}
	{
		// RegSetValueEx / RegQueryValueEx (type param)
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForRegistryHive();
		funcParamsMap["RegDeleteKeyA"] = paramSymbolsMap;
		funcParamsMap["RegDeleteKeyW"] = paramSymbolsMap;
	}
	{
		// RegSetValueEx — type is param 4
		paramSymbolsMap.clear();
		paramSymbolsMap[4] = getSymbolicNamesForRegistryType();
		funcParamsMap["RegSetValueExA"] = paramSymbolsMap;
		funcParamsMap["RegSetValueExW"] = paramSymbolsMap;
		funcParamsMap["RegSetValueEx"]  = paramSymbolsMap;
	}
	{
		// RegQueryValueEx — type is output via param 4 pointer, not useful
		// RegEnumValue — type is param 7 (out pointer)
		// RegCloseKey — no symbolic args
	}
	{
		// RegConnectRegistry
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForRegistryHive();
		funcParamsMap["RegConnectRegistryA"] = paramSymbolsMap;
		funcParamsMap["RegConnectRegistryW"] = paramSymbolsMap;
	}

	// ── WinSock2 — socket / WSASocket ─────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForWSAAddressFamily();
		paramSymbolsMap[2] = getSymbolicNamesForWSASocketType();
		paramSymbolsMap[3] = getSymbolicNamesForWSAProtocol();
		funcParamsMap["socket"]    = paramSymbolsMap;
		funcParamsMap["WSASocket"]  = paramSymbolsMap;
		funcParamsMap["WSASocketA"] = paramSymbolsMap;
		funcParamsMap["WSASocketW"] = paramSymbolsMap;
	}
	{
		// WSASocket has an extra flags param
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForWSAAddressFamily();
		paramSymbolsMap[2] = getSymbolicNamesForWSASocketType();
		paramSymbolsMap[3] = getSymbolicNamesForWSAProtocol();
		paramSymbolsMap[5] = getSymbolicNamesForWSAFlags();  // dwFlags
		funcParamsMap["WSASocketW_ext"] = paramSymbolsMap;   // internal alias
	}

	// ── send / recv / sendto / recvfrom ───────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[4] = getSymbolicNamesForWSASendRecvFlags();
		funcParamsMap["send"]    = paramSymbolsMap;
		funcParamsMap["sendto"]  = paramSymbolsMap;
		funcParamsMap["recv"]    = paramSymbolsMap;
		funcParamsMap["recvfrom"]= paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForWSASendRecvFlags();
		funcParamsMap["WSASend"]    = paramSymbolsMap;
		funcParamsMap["WSARecv"]    = paramSymbolsMap;
		funcParamsMap["WSASendTo"]  = paramSymbolsMap;
		funcParamsMap["WSARecvFrom"]= paramSymbolsMap;
	}

	// ── shutdown ──────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForWSAShutdown();
		funcParamsMap["shutdown"] = paramSymbolsMap;
	}

	// ── ioctlsocket / WSAIoctl ────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForWSAIoctlCommand();
		funcParamsMap["ioctlsocket"] = paramSymbolsMap;
		funcParamsMap["WSAIoctl"]    = paramSymbolsMap;
	}

	// ── setsockopt / getsockopt ───────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForSOLevel();
		paramSymbolsMap[3] = getSymbolicNamesForSOOption();
		funcParamsMap["setsockopt"] = paramSymbolsMap;
		funcParamsMap["getsockopt"] = paramSymbolsMap;
	}

	// ── LoadLibrary / LoadLibraryEx ───────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForLoadLibraryFlags();
		funcParamsMap["LoadLibraryExA"] = paramSymbolsMap;
		funcParamsMap["LoadLibraryExW"] = paramSymbolsMap;
		funcParamsMap["LoadLibraryEx"]  = paramSymbolsMap;
	}

	// ── FormatMessage ─────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForFormatMessageFlags();
		funcParamsMap["FormatMessageA"] = paramSymbolsMap;
		funcParamsMap["FormatMessageW"] = paramSymbolsMap;
		funcParamsMap["FormatMessage"]  = paramSymbolsMap;
	}

	// ── MessageBox / MessageBoxEx ─────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[4] = getSymbolicNamesForMessageBoxType();
		funcParamsMap["MessageBoxA"]   = paramSymbolsMap;
		funcParamsMap["MessageBoxW"]   = paramSymbolsMap;
		funcParamsMap["MessageBox"]    = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[4] = getSymbolicNamesForMessageBoxType();
		funcParamsMap["MessageBoxExA"] = paramSymbolsMap;
		funcParamsMap["MessageBoxExW"] = paramSymbolsMap;
	}

	// ── ShowWindow ────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForShowWindow();
		funcParamsMap["ShowWindow"] = paramSymbolsMap;
	}

	// ── SendMessage / PostMessage ──────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForWindowMessage();
		funcParamsMap["SendMessageA"]   = paramSymbolsMap;
		funcParamsMap["SendMessageW"]   = paramSymbolsMap;
		funcParamsMap["SendMessage"]    = paramSymbolsMap;
		funcParamsMap["PostMessageA"]   = paramSymbolsMap;
		funcParamsMap["PostMessageW"]   = paramSymbolsMap;
		funcParamsMap["PostMessage"]    = paramSymbolsMap;
		funcParamsMap["PostThreadMessageA"] = paramSymbolsMap;
		funcParamsMap["PostThreadMessageW"] = paramSymbolsMap;
	}

	// ── GetStdHandle ──────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForStdHandle();
		funcParamsMap["GetStdHandle"] = paramSymbolsMap;
		funcParamsMap["SetStdHandle"] = paramSymbolsMap;
	}

	// ── HeapCreate / HeapAlloc / HeapFree / HeapReAlloc ───────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForHeapCreateFlags();
		funcParamsMap["HeapCreate"] = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForHeapCreateFlags();
		funcParamsMap["HeapAlloc"]   = paramSymbolsMap;
		funcParamsMap["HeapFree"]    = paramSymbolsMap;
		funcParamsMap["HeapReAlloc"] = paramSymbolsMap;
	}

	// ── CryptAcquireContext ────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[4] = getSymbolicNamesForCryptProvType();
		paramSymbolsMap[5] = getSymbolicNamesForCryptAcquireFlags();
		funcParamsMap["CryptAcquireContextA"] = paramSymbolsMap;
		funcParamsMap["CryptAcquireContextW"] = paramSymbolsMap;
		funcParamsMap["CryptAcquireContext"]  = paramSymbolsMap;
	}

	// ── CryptCreateHash ───────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForCryptAlgId();
		funcParamsMap["CryptCreateHash"] = paramSymbolsMap;
	}

	// ── CryptEncrypt / CryptDecrypt ───────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForBoolParam();  // Final
		funcParamsMap["CryptEncrypt"] = paramSymbolsMap;
		funcParamsMap["CryptDecrypt"] = paramSymbolsMap;
	}

	// ── SHGetFolderPath / SHGetSpecialFolderPath ──────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForCSIDL();
		funcParamsMap["SHGetFolderPathA"]         = paramSymbolsMap;
		funcParamsMap["SHGetFolderPathW"]         = paramSymbolsMap;
		funcParamsMap["SHGetSpecialFolderPathA"]  = paramSymbolsMap;
		funcParamsMap["SHGetSpecialFolderPathW"]  = paramSymbolsMap;
	}

	// ── CreateNamedPipe ────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForPipeAccess();
		paramSymbolsMap[3] = getSymbolicNamesForPipeMode();
		funcParamsMap["CreateNamedPipeA"] = paramSymbolsMap;
		funcParamsMap["CreateNamedPipeW"] = paramSymbolsMap;
		funcParamsMap["CreateNamedPipe"]  = paramSymbolsMap;
	}

	// ── DeviceIoControl ────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForIoControlCode();
		funcParamsMap["DeviceIoControl"] = paramSymbolsMap;
	}

	// ── SetSecurityInfo / GetSecurityInfo ──────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForSecurityInfo();
		funcParamsMap["SetSecurityInfo"]         = paramSymbolsMap;
		funcParamsMap["GetSecurityInfo"]         = paramSymbolsMap;
		funcParamsMap["SetNamedSecurityInfoA"]   = paramSymbolsMap;
		funcParamsMap["SetNamedSecurityInfoW"]   = paramSymbolsMap;
		funcParamsMap["GetNamedSecurityInfoA"]   = paramSymbolsMap;
		funcParamsMap["GetNamedSecurityInfoW"]   = paramSymbolsMap;
	}

	// ── NT Native API ──────────────────────────────────────────────────────
	{
		// NtCreateFile / ZwCreateFile
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForFileAccess();
		paramSymbolsMap[6] = getSymbolicNamesForFileShareMode();
		paramSymbolsMap[8] = getSymbolicNamesForFileCreation();
		funcParamsMap["NtCreateFile"]  = paramSymbolsMap;
		funcParamsMap["ZwCreateFile"]  = paramSymbolsMap;
		funcParamsMap["NtOpenFile"]    = paramSymbolsMap;
		funcParamsMap["ZwOpenFile"]    = paramSymbolsMap;
	}
	{
		// NtAllocateVirtualMemory
		paramSymbolsMap.clear();
		paramSymbolsMap[5] = getSymbolicNamesForAllocationType();
		paramSymbolsMap[6] = getSymbolicNamesForPageProtection();
		funcParamsMap["NtAllocateVirtualMemory"] = paramSymbolsMap;
		funcParamsMap["ZwAllocateVirtualMemory"] = paramSymbolsMap;
	}
	{
		// NtProtectVirtualMemory
		paramSymbolsMap.clear();
		paramSymbolsMap[4] = getSymbolicNamesForPageProtection();
		funcParamsMap["NtProtectVirtualMemory"] = paramSymbolsMap;
		funcParamsMap["ZwProtectVirtualMemory"] = paramSymbolsMap;
	}
	{
		// RtlNtStatusToDosError / NtQuerySystemInformation status returns
		paramSymbolsMap.clear();
		paramSymbolsMap[1] = getSymbolicNamesForNtStatus();
		funcParamsMap["RtlNtStatusToDosError"] = paramSymbolsMap;
	}

	// ── MapViewOfFile / MapViewOfFileEx ────────────────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[2] = getSymbolicNamesForFileAccess();
		funcParamsMap["MapViewOfFile"]   = paramSymbolsMap;
		funcParamsMap["MapViewOfFileEx"] = paramSymbolsMap;
	}

	// ── CreateFileMapping / CreateFileMappingA / W ────────────────────────
	{
		paramSymbolsMap.clear();
		paramSymbolsMap[3] = getSymbolicNamesForPageProtection();
		funcParamsMap["CreateFileMappingA"] = paramSymbolsMap;
		funcParamsMap["CreateFileMappingW"] = paramSymbolsMap;
		funcParamsMap["CreateFileMapping"]  = paramSymbolsMap;
	}

	// ── SetErrorMode ───────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		symbolicNamesMap.clear();
		symbolicNamesMap[0x0000] = "0";
		symbolicNamesMap[0x0001] = "SEM_FAILCRITICALERRORS";
		symbolicNamesMap[0x0002] = "SEM_NOALIGNMENTFAULTEXCEPT";
		symbolicNamesMap[0x0004] = "SEM_NOGPFAULTERRORBOX";
		symbolicNamesMap[0x8000] = "SEM_NOOPENFILEERRORBOX";
		paramSymbolsMap[1] = symbolicNamesMap;
		funcParamsMap["SetErrorMode"]    = paramSymbolsMap;
		funcParamsMap["SetThreadErrorMode"] = paramSymbolsMap;
	}

	// ── OpenProcess ────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		symbolicNamesMap.clear();
		// Process access rights (WinNT.h)
		symbolicNamesMap[0x0001] = "PROCESS_TERMINATE";
		symbolicNamesMap[0x0002] = "PROCESS_CREATE_THREAD";
		symbolicNamesMap[0x0008] = "PROCESS_VM_OPERATION";
		symbolicNamesMap[0x0010] = "PROCESS_VM_READ";
		symbolicNamesMap[0x0020] = "PROCESS_VM_WRITE";
		symbolicNamesMap[0x0040] = "PROCESS_DUP_HANDLE";
		symbolicNamesMap[0x0080] = "PROCESS_CREATE_PROCESS";
		symbolicNamesMap[0x0100] = "PROCESS_SET_QUOTA";
		symbolicNamesMap[0x0200] = "PROCESS_SET_INFORMATION";
		symbolicNamesMap[0x0400] = "PROCESS_QUERY_INFORMATION";
		symbolicNamesMap[0x0800] = "PROCESS_SUSPEND_RESUME";
		symbolicNamesMap[0x1000] = "PROCESS_QUERY_LIMITED_INFORMATION";
		symbolicNamesMap[0x1fffff] = "PROCESS_ALL_ACCESS";
		paramSymbolsMap[1] = symbolicNamesMap;
		paramSymbolsMap[2] = getSymbolicNamesForBoolParam();  // bInheritHandle
		funcParamsMap["OpenProcess"] = paramSymbolsMap;
	}

	// ── OpenThread ─────────────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		symbolicNamesMap.clear();
		symbolicNamesMap[0x0001] = "THREAD_TERMINATE";
		symbolicNamesMap[0x0002] = "THREAD_SUSPEND_RESUME";
		symbolicNamesMap[0x0008] = "THREAD_GET_CONTEXT";
		symbolicNamesMap[0x0010] = "THREAD_SET_CONTEXT";
		symbolicNamesMap[0x0020] = "THREAD_SET_INFORMATION";
		symbolicNamesMap[0x0040] = "THREAD_QUERY_INFORMATION";
		symbolicNamesMap[0x0080] = "THREAD_SET_THREAD_TOKEN";
		symbolicNamesMap[0x0100] = "THREAD_IMPERSONATE";
		symbolicNamesMap[0x0200] = "THREAD_DIRECT_IMPERSONATION";
		symbolicNamesMap[0x1fffff] = "THREAD_ALL_ACCESS";
		paramSymbolsMap[1] = symbolicNamesMap;
		paramSymbolsMap[2] = getSymbolicNamesForBoolParam();
		funcParamsMap["OpenThread"] = paramSymbolsMap;
	}

	// ── InternetOpen / InternetConnect (WinInet) ──────────────────────────
	{
		paramSymbolsMap.clear();
		symbolicNamesMap.clear();
		symbolicNamesMap[0]  = "INTERNET_OPEN_TYPE_PRECONFIG";
		symbolicNamesMap[1]  = "INTERNET_OPEN_TYPE_DIRECT";
		symbolicNamesMap[3]  = "INTERNET_OPEN_TYPE_PROXY";
		symbolicNamesMap[4]  = "INTERNET_OPEN_TYPE_PRECONFIG_WITH_NO_AUTOPROXY";
		paramSymbolsMap[2]   = symbolicNamesMap;
		funcParamsMap["InternetOpenA"] = paramSymbolsMap;
		funcParamsMap["InternetOpenW"] = paramSymbolsMap;
		funcParamsMap["InternetOpen"]  = paramSymbolsMap;
	}
	{
		paramSymbolsMap.clear();
		symbolicNamesMap.clear();
		symbolicNamesMap[1]  = "INTERNET_SERVICE_FTP";
		symbolicNamesMap[2]  = "INTERNET_SERVICE_GOPHER";
		symbolicNamesMap[3]  = "INTERNET_SERVICE_HTTP";
		paramSymbolsMap[3]   = symbolicNamesMap;
		funcParamsMap["InternetConnectA"] = paramSymbolsMap;
		funcParamsMap["InternetConnectW"] = paramSymbolsMap;
	}

	// ── HttpOpenRequest ───────────────────────────────────────────────────
	{
		paramSymbolsMap.clear();
		symbolicNamesMap.clear();
		symbolicNamesMap[0x00000000] = "0";
		symbolicNamesMap[0x00000001] = "INTERNET_FLAG_RELOAD";
		symbolicNamesMap[0x00000080] = "INTERNET_FLAG_EXISTING_CONNECT";
		symbolicNamesMap[0x00000400] = "INTERNET_FLAG_SECURE";
		symbolicNamesMap[0x00000800] = "INTERNET_FLAG_KEEP_CONNECTION";
		symbolicNamesMap[0x00001000] = "INTERNET_FLAG_NO_AUTO_REDIRECT";
		symbolicNamesMap[0x00002000] = "INTERNET_FLAG_READ_PREFETCH";
		symbolicNamesMap[0x00004000] = "INTERNET_FLAG_NO_COOKIES";
		symbolicNamesMap[0x00008000] = "INTERNET_FLAG_NO_AUTH";
		symbolicNamesMap[0x00200000] = "INTERNET_FLAG_HYPERLINK";
		symbolicNamesMap[0x00400000] = "INTERNET_FLAG_NO_UI";
		symbolicNamesMap[0x00800000] = "INTERNET_FLAG_PRAGMA_NOCACHE";
		symbolicNamesMap[0x01000000] = "INTERNET_FLAG_CACHE_ASYNC";
		symbolicNamesMap[0x04000000] = "INTERNET_FLAG_IGNORE_REDIRECT_TO_HTTP";
		symbolicNamesMap[0x08000000] = "INTERNET_FLAG_IGNORE_REDIRECT_TO_HTTPS";
		symbolicNamesMap[0x10000000] = "INTERNET_FLAG_IGNORE_CERT_DATE_INVALID";
		symbolicNamesMap[0x20000000] = "INTERNET_FLAG_IGNORE_CERT_CN_INVALID";
		symbolicNamesMap[0x80000000] = "INTERNET_FLAG_DONT_CACHE";
		paramSymbolsMap[6] = symbolicNamesMap;
		funcParamsMap["HttpOpenRequestA"] = paramSymbolsMap;
		funcParamsMap["HttpOpenRequestW"] = paramSymbolsMap;
		funcParamsMap["HttpOpenRequest"]  = paramSymbolsMap;
	}

	return funcParamsMap;
}

/// Mapping of function names into symbolic names of their parameters.
const FuncParamsMap &FUNC_PARAMS_MAP(initFuncParamsMap());

} // anonymous namespace

/**
* @brief Implements getSymbolicNamesForParam() for WinAPISemantics.
*
* See its description for more details.
*/
std::optional<IntStringMap> getSymbolicNamesForParam(const std::string &funcName,
		unsigned paramPos) {
	return getSymbolicNamesForParamFromMap(funcName, paramPos, FUNC_PARAMS_MAP);
}

} // namespace win_api
} // namespace semantics
} // namespace llvmir2hll
} // namespace retdec
