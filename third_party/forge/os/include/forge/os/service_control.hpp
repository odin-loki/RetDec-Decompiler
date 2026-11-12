#pragma once
// forge::os::service control: define, install, start, stop, query and uninstall a background service.
// service_spec::scope picks a per-user service (default) or a system one (needs root / an elevated process).
//   Linux with systemd: a unit in $XDG_CONFIG_HOME/systemd/user (`systemctl --user`) or /etc/systemd/system.
//   macOS: a plist in ~/Library/LaunchAgents (`launchctl ... gui/<uid>`) or /Library/LaunchDaemons (`system`);
//   query parses `launchctl print` (loaded but not running is stopped, a non-zero last exit failed).
//   Windows user scope: a Task Scheduler task with a logon trigger for the current user, registered from
//   render_task_xml() through the ITaskService COM API (ole32/oleaut32 loaded at run time, no link libraries).
//   Windows system scope: a Service Control Manager service (CreateServiceW, ChangeServiceConfig2W for the
//   description and failure actions, the service key's `Environment` REG_MULTI_SZ value for env).
//   FreeBSD: an rc.d script in /usr/local/etc/rc.d (system scope only) that runs the program under daemon(8),
//   driven by `service <name> onestart/onestop/onestatus` and `sysrc <name>_enable`.
//   Android / iOS: unsupported.
// The service text is produced by render_*() so it can be inspected or tested without touching the system;
// install() rejects control characters (validate()). Escaping: systemd per systemd.syntax, plist / task XML as
// XML text, rc.d values as single-quoted sh words (twice where rc.subr evals them), Windows command lines with
// MSVCRT quoting, PowerShell literals with every single-quote variant doubled.
// set service_spec::dir to write the file somewhere else without registering it (also used by tests).
// Guide: docs/os-wrapper.md.
#include <forge/os/process.hpp>
#include <forge/os/service.hpp>
#include <forge/os/users.hpp>
#include <chrono>
#include <fstream>
#include <thread>
#if defined(FORGE_OS_WINDOWS)
#include <objbase.h>
#include <oleauto.h>
#include <taskschd.h>
#include <tlhelp32.h>
#endif

namespace forge::os::service {
enum class service_scope { user, system };
struct service_spec {
    std::string name, description;
    std::vector<std::string> argv;               // program and arguments
    std::string working_dir;
    std::map<std::string, std::string> env;
    bool restart_on_failure = true, start_at_login = true;   // start_at_login: at boot for system services
    std::string dir;                             // override the unit directory (empty = default for the scope)
    service_scope scope = service_scope::user;
};
enum class service_state { not_installed, stopped, running, failed, unknown };
inline const char* state_name(service_state s) {
    switch (s) { case service_state::not_installed: return "not_installed"; case service_state::stopped: return "stopped"; case service_state::running: return "running"; case service_state::failed: return "failed"; default: return "unknown"; }
}
// query_info(): state plus the main process id (0 when not running or unknown) and the last exit code where the
// manager keeps one (Windows services and tasks, launchd).
struct service_info { service_state state = service_state::unknown; std::int64_t pid = 0; long exit_code = 0; };
inline bool valid_name(const std::string& n) {
    if (n.empty() || n.size() > 128) return false;
    for (char c : n) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '@')) return false;
    return true;
}
// Control characters (newline above all) would start new unit directives or break the plist: rejected by
// validate() (and install()); the renderers also neutralise them so their output is always one directive per line.
inline bool has_ctl_(const std::string& s) { for (unsigned char c : s) if (c < 0x20 || c == 0x7f) return true; return false; }
inline status validate(const service_spec& s) {
    auto bad = [](const std::string& why) { return status{{}, {EINVAL, "service: " + why}}; };
    if (!valid_name(s.name)) return bad("invalid name '" + s.name + "'");
    if (s.argv.empty()) return bad("argv is empty");
    if (s.argv[0].empty() || std::string_view("@-:+!|").find(s.argv[0][0]) != std::string_view::npos) return bad("argv[0] is empty or starts with a systemd prefix character");
    if (has_ctl_(s.description) || has_ctl_(s.working_dir)) return bad("control character in description or working_dir");
    for (auto& a : s.argv) if (has_ctl_(a)) return bad("control character in argv");
    for (auto& [k, v] : s.env) if (k.empty() || k.find('=') != std::string::npos || has_ctl_(k) || has_ctl_(v)) return bad("invalid environment entry '" + k + "'");
    return {};
}
inline void esc_ctl_(std::string& o, char c) { char b[8]; std::snprintf(b, sizeof b, "\\x%02x", unsigned(static_cast<unsigned char>(c))); o += b; }
// ExecStart= word (systemd.service "Command lines"): specifiers (%) and $VAR expansion are escaped by doubling,
// quotes and backslashes are C-escaped inside "...", and a lone ";" (the command separator) becomes "\;".
inline std::string quote_systemd_(const std::string& a) {
    if (a == ";") return "\\;";
    bool plain = !a.empty(); for (char c : a) plain &= !(c == ' ' || c == '"' || c == '\\' || c == '\'' || c == '$' || c == '%' || c == ';' || static_cast<unsigned char>(c) < 0x20 || c == 0x7f);
    if (plain) return a;
    std::string o = "\"";
    for (char c : a) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; } else if (c == '%') o += "%%"; else if (c == '$') o += "$$";
        else if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) esc_ctl_(o, c); else o += c;
    }
    return o + "\"";
}
// Environment= assignment (systemd.exec): quoted and C-unescaped, specifiers expanded, but no $ expansion.
inline std::string quote_systemd_env_(const std::string& kv) {
    std::string o = "\"";
    for (char c : kv) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; } else if (c == '%') o += "%%";
        else if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) esc_ctl_(o, c); else o += c;
    }
    return o + "\"";
}
// Description= / WorkingDirectory=: taken verbatim to the end of the line (no unquoting), specifiers expanded.
inline std::string systemd_verbatim_(const std::string& s) {
    std::string o; for (char c : s) { if (c == '%') o += "%%"; else if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) o += ' '; else o += c; } return o;
}
// XML text / attribute value: the five predefined entities; control characters (not allowed in XML 1.0) -> space.
inline std::string xml_esc_(const std::string& s) {
    std::string o; for (char c : s) { if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '"') o += "&quot;"; else if (c == '\'') o += "&apos;"; else if ((static_cast<unsigned char>(c) < 0x20 && c != '\t') || c == 0x7f) o += ' '; else o += c; } return o;
}
// One POSIX sh word: single quotes, an embedded ' as '\''. Nothing inside '...' is special to sh.
inline std::string sh_quote_(const std::string& s) {
    std::string o = "'"; for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; } return o + "'";
}
// One Windows argument with MSVCRT / CommandLineToArgvW quoting (works on UTF-8 bytes: only ASCII is special).
inline std::string win_quote_(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
    std::string o = "\""; std::size_t bs = 0;
    for (char c : a) { if (c == '\\') { ++bs; continue; } if (c == '"') o.append(bs * 2 + 1, '\\'); else o.append(bs, '\\'); bs = 0; o += c; }
    o.append(bs * 2, '\\'); return o + "\"";
}
inline std::string win_cmdline_(const std::vector<std::string>& av, std::size_t from = 0) {
    std::string o; for (std::size_t i = from; i < av.size(); ++i) { if (i > from) o += ' '; o += win_quote_(av[i]); } return o;
}
// PowerShell single-quoted literal: no expansion inside; the quote characters are ' and U+2018..U+201B, each
// escaped by doubling.
inline std::string ps_quote_(const std::string& s) {
    std::string o = "'";
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\'') { o += "''"; continue; }
        if (i + 2 < s.size() && static_cast<unsigned char>(s[i]) == 0xE2 && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
            static_cast<unsigned char>(s[i + 2]) >= 0x98 && static_cast<unsigned char>(s[i + 2]) <= 0x9B) { o.append(s, i, 3); o.append(s, i, 3); i += 2; continue; }
        o += s[i];
    }
    return o + "'";
}
// UTF-8 -> UTF-16LE bytes -> base64 (PowerShell -EncodedCommand). Bad UTF-8 bytes become U+FFFD.
inline std::string ps_encode_(const std::string& s) {
    std::string b;
    auto put = [&](unsigned u) { b += char(u & 0xff); b += char(u >> 8); };
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]); unsigned cp = 0xFFFD; std::size_t n = 1;
        if (c < 0x80) cp = c;
        else if ((c >> 5) == 6 && i + 1 < s.size()) { cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3F); n = 2; }
        else if ((c >> 4) == 14 && i + 2 < s.size()) { cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3F); n = 3; }
        else if ((c >> 3) == 30 && i + 3 < s.size()) { cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3F); n = 4; }
        i += n;
        if (cp >= 0x10000) { cp -= 0x10000; put(0xD800 + (cp >> 10)); put(0xDC00 + (cp & 0x3FF)); } else put(cp);
    }
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (std::size_t i = 0; i < b.size(); i += 3) {
        unsigned v = unsigned(static_cast<unsigned char>(b[i])) << 16 | (i + 1 < b.size() ? unsigned(static_cast<unsigned char>(b[i + 1])) << 8 : 0) | (i + 2 < b.size() ? static_cast<unsigned char>(b[i + 2]) : 0u);
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += i + 1 < b.size() ? t[(v >> 6) & 63] : '='; o += i + 2 < b.size() ? t[v & 63] : '=';
    }
    return o;
}

inline std::string render_systemd_unit(const service_spec& s) {
    std::string u = "[Unit]\nDescription=" + systemd_verbatim_(s.description.empty() ? s.name : s.description) + "\n\n[Service]\nType=simple\nExecStart=";
    for (std::size_t i = 0; i < s.argv.size(); ++i) u += (i ? " " : "") + quote_systemd_(s.argv[i]);
    u += "\n";
    if (!s.working_dir.empty()) u += "WorkingDirectory=" + systemd_verbatim_(s.working_dir) + "\n";
    for (auto& [k, v] : s.env) u += "Environment=" + quote_systemd_env_(k + "=" + v) + "\n";
    u += std::string("Restart=") + (s.restart_on_failure ? "on-failure" : "no") + "\n";
    u += std::string("\n[Install]\nWantedBy=") + (s.scope == service_scope::system ? "multi-user.target" : "default.target") + "\n";
    return u;
}
inline std::string launchctl_field_(const std::string& out, const char* k) {
    std::string key = std::string("\n\t") + k + " = "; auto t = "\n" + out; auto p = t.find(key); if (p == std::string::npos) return {};
    p += key.size(); auto e = t.find('\n', p); return t.substr(p, e == std::string::npos ? std::string::npos : e - p);
}
// `launchctl print <domain>/<label>` output -> state: top-level (one-tab) "state = running" is running; otherwise a
// non-zero "last exit code" is failed, anything else stopped.
inline service_state parse_launchctl_print(const std::string& out) {
    auto st = launchctl_field_(out, "state"); if (st == "running") return service_state::running;
    auto lx = launchctl_field_(out, "last exit code"); if (!lx.empty() && lx[0] != '(' && std::strtol(lx.c_str(), nullptr, 10) != 0) return service_state::failed;
    return service_state::stopped;
}
inline std::string render_launchd_plist(const service_spec& s) {
    std::string p = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n";
    p += "  <key>Label</key><string>" + xml_esc_(s.name) + "</string>\n  <key>ProgramArguments</key>\n  <array>\n";
    for (auto& a : s.argv) p += "    <string>" + xml_esc_(a) + "</string>\n";
    p += "  </array>\n";
    if (!s.working_dir.empty()) p += "  <key>WorkingDirectory</key><string>" + xml_esc_(s.working_dir) + "</string>\n";
    if (!s.env.empty()) { p += "  <key>EnvironmentVariables</key>\n  <dict>\n"; for (auto& [k, v] : s.env) p += "    <key>" + xml_esc_(k) + "</key><string>" + xml_esc_(v) + "</string>\n"; p += "  </dict>\n"; }
    p += std::string("  <key>RunAtLoad</key><") + (s.start_at_login ? "true" : "false") + "/>\n";
    p += std::string("  <key>KeepAlive</key>") + (s.restart_on_failure ? "<dict><key>SuccessfulExit</key><false/></dict>" : "<false/>") + "\n";
    p += "</dict>\n</plist>\n";
    return p;
}

// ---- Windows system scope: Service Control Manager parameters ----
// A service has no working directory of its own (it starts in System32): working_dir travels as the
// FORGE_SERVICE_DIR variable in the service's environment, and run_as_service() (service.hpp) changes to it
// before running the body. Programs not built on run_as_service must read it themselves.
inline constexpr const char* service_dir_env = "FORGE_SERVICE_DIR";
struct scm_config {
    std::string command_line;                // lpBinaryPathName: MSVCRT-quoted argv (argv[0] quoted when it has a space)
    std::string display_name, description;
    bool auto_start = true, restart = true;  // SERVICE_AUTO_START / DEMAND_START; restart: 3 x SC_ACTION_RESTART after 5 s
    std::vector<std::string> environment;    // "K=V" entries of the `Environment` REG_MULTI_SZ value
};
inline scm_config render_scm(const service_spec& s) {
    scm_config c; c.command_line = win_cmdline_(s.argv); c.display_name = s.description.empty() ? s.name : s.description;
    c.description = s.description; c.auto_start = s.start_at_login; c.restart = s.restart_on_failure;
    bool has_dir = false;
    for (auto& [k, v] : s.env) { c.environment.push_back(k + "=" + v); if (k == service_dir_env) has_dir = true; }
    if (!s.working_dir.empty() && !has_dir) c.environment.push_back(std::string(service_dir_env) + "=" + s.working_dir);
    return c;
}
// ---- Windows user scope: Task Scheduler task definition ----
// The task runs argv with the user's interactive token at logon (start_at_login), never times out, ignores a
// second start while running, and is restarted up to 999 times a minute apart when the scheduler sees it fail
// (restart_on_failure; the scheduler's notion of failure is a launch failure, not every non-zero exit code).
// Tasks have no environment block: with env set, the action is Windows PowerShell with -EncodedCommand (base64
// UTF-16, so no quoting reaches a command line) setting the variables and starting argv through
// ProcessStartInfo with the MSVCRT-quoted argument string, then exiting with its exit code.
// user_id is the account (SID string or DOMAIN\name) for the trigger and principal.
inline std::string task_powershell_script_(const service_spec& s) {
    std::string p = "$ErrorActionPreference='Stop'; ";
    for (auto& [k, v] : s.env) p += "[Environment]::SetEnvironmentVariable(" + ps_quote_(k) + "," + ps_quote_(v) + "); ";
    p += "$i=New-Object System.Diagnostics.ProcessStartInfo; $i.FileName=" + ps_quote_(s.argv[0]) + "; $i.Arguments=" + ps_quote_(win_cmdline_(s.argv, 1)) +
         "; $i.UseShellExecute=$false; $p=[System.Diagnostics.Process]::Start($i); $p.WaitForExit(); exit $p.ExitCode";
    return p;
}
inline std::string render_task_xml(const service_spec& s, const std::string& user_id) {
    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n";
    x += "  <RegistrationInfo>\n    <Description>" + xml_esc_(s.description.empty() ? s.name : s.description) + "</Description>\n  </RegistrationInfo>\n";
    x += "  <Triggers>\n";
    if (s.start_at_login) x += "    <LogonTrigger>\n      <Enabled>true</Enabled>\n      <UserId>" + xml_esc_(user_id) + "</UserId>\n    </LogonTrigger>\n";
    x += "  </Triggers>\n  <Principals>\n    <Principal id=\"Author\">\n      <UserId>" + xml_esc_(user_id) + "</UserId>\n"
         "      <LogonType>InteractiveToken</LogonType>\n      <RunLevel>LeastPrivilege</RunLevel>\n    </Principal>\n  </Principals>\n";
    x += "  <Settings>\n    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
         "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n    <AllowHardTerminate>true</AllowHardTerminate>\n    <StartWhenAvailable>false</StartWhenAvailable>\n"
         "    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\n    <IdleSettings>\n      <StopOnIdleEnd>false</StopOnIdleEnd>\n      <RestartOnIdle>false</RestartOnIdle>\n    </IdleSettings>\n"
         "    <AllowStartOnDemand>true</AllowStartOnDemand>\n    <Enabled>true</Enabled>\n    <Hidden>false</Hidden>\n    <RunOnlyIfIdle>false</RunOnlyIfIdle>\n"
         "    <WakeToRun>false</WakeToRun>\n    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n    <Priority>5</Priority>\n";
    if (s.restart_on_failure) x += "    <RestartOnFailure>\n      <Interval>PT1M</Interval>\n      <Count>999</Count>\n    </RestartOnFailure>\n";
    x += "  </Settings>\n  <Actions Context=\"Author\">\n    <Exec>\n";
    if (s.env.empty()) {
        x += "      <Command>" + xml_esc_(win_quote_(s.argv[0])) + "</Command>\n";
        if (s.argv.size() > 1) x += "      <Arguments>" + xml_esc_(win_cmdline_(s.argv, 1)) + "</Arguments>\n";
    } else {
        x += "      <Command>powershell.exe</Command>\n      <Arguments>-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -EncodedCommand " + ps_encode_(task_powershell_script_(s)) + "</Arguments>\n";
    }
    if (!s.working_dir.empty()) x += "      <WorkingDirectory>" + xml_esc_(s.working_dir) + "</WorkingDirectory>\n";
    x += "    </Exec>\n  </Actions>\n</Task>\n";
    return x;
}
// ---- FreeBSD: rc.d script ----
// rc.subr variable names must be shell identifiers: other characters of the service name become '_'.
inline std::string rc_name(const std::string& n) {
    std::string o; for (char c : n) o += std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_';
    if (o.empty() || std::isdigit(static_cast<unsigned char>(o[0]))) o = "svc_" + o;
    return o;
}
inline bool sh_identifier_(const std::string& k) {
    if (k.empty() || std::isdigit(static_cast<unsigned char>(k[0]))) return false;
    for (char c : k) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}
// daemon(8) supervises argv (-P: its own pid in the pidfile, so stop ends supervisor and child; -r -R 5: restart
// 5 s after any exit when restart_on_failure; -S -T: output to syslog under the rc name). rc.subr evals
// command_args, so each argv word is sh-quoted and the whole string is quoted again for the assignment. The
// working directory and environment are applied by a start_precmd in the rc shell, which daemon inherits.
// Environment names that are not shell identifiers are left out (install() refuses them).
inline std::string render_rcd_script(const service_spec& s) {
    const std::string n = rc_name(s.name);
    std::string args = "-P ${pidfile}"; if (s.restart_on_failure) args += " -r -R 5";
    args += " -S -T " + n + " --"; for (auto& a : s.argv) args += " " + sh_quote_(a);
    std::string r = "#!/bin/sh\n#\n# PROVIDE: " + n + "\n# REQUIRE: LOGIN\n# KEYWORD: shutdown\n#\n# Generated by forge::os::service.\n\n. /etc/rc.subr\n\n";
    r += "name=" + n + "\nrcvar=" + n + "_enable\ndesc=" + sh_quote_(s.description.empty() ? s.name : s.description) + "\n\nload_rc_config $name\n";
    r += ": ${" + n + "_enable:=NO}\n\npidfile=\"/var/run/${name}.pid\"\ncommand=\"/usr/sbin/daemon\"\ncommand_args=" + sh_quote_(args) + "\n";
    r += "start_precmd=\"" + n + "_prestart\"\n\n" + n + "_prestart()\n{\n";
    if (!s.working_dir.empty()) r += "\tcd -- " + sh_quote_(s.working_dir) + " || return 1\n";
    for (auto& [k, v] : s.env) if (sh_identifier_(k)) r += "\texport " + k + "=" + sh_quote_(v) + "\n";
    r += "\treturn 0\n}\n\nrun_rc_command \"$1\"\n";
    for (char& c : r) if ((static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t') || c == 0x7f) c = ' ';   // validate() refuses these anyway
    return r;
}

inline std::string unit_dir(const service_spec& s) {
    if (!s.dir.empty()) return s.dir;
    const bool sys = s.scope == service_scope::system;
#if defined(__APPLE__)
    return sys ? std::string("/Library/LaunchDaemons") : home_dir() + "/Library/LaunchAgents";
#elif defined(FORGE_OS_FREEBSD)
    (void)sys; return "/usr/local/etc/rc.d";
#elif defined(FORGE_OS_WINDOWS)
    (void)sys; return {};   // nothing on disk: the SCM database / Task Scheduler store
#else
    return sys ? std::string("/etc/systemd/system") : config_dir() + "/systemd/user";
#endif
}
inline std::string unit_path(const service_spec& s) {
#if defined(__APPLE__)
    return unit_dir(s) + "/" + s.name + ".plist";
#elif defined(FORGE_OS_FREEBSD)
    return unit_dir(s) + "/" + s.name;
#elif defined(FORGE_OS_WINDOWS)
    return s.dir.empty() ? std::string() : s.dir + "\\" + s.name + ".xml";   // the task XML, written only with dir set
#else
    return unit_dir(s) + "/" + s.name + ".service";
#endif
}
inline result<proc_result> ctl_(std::vector<std::string> argv) { spawn_spec sp; sp.argv = std::move(argv); sp.timeout_ms = 30000; return spawn_wait(sp); }
inline status ctl_ok_(std::vector<std::string> argv) {
    auto what = argv.empty() ? std::string() : argv[0] + (argv.size() > 1 ? " " + argv[1] : "") + (argv.size() > 2 ? " " + argv[2] : "");
    auto r = ctl_(std::move(argv)); if (!r) return {{}, r.err};
    if (r->exit_code != 0) return {{}, {r->exit_code, what + ": " + (r->err.empty() ? r->out : r->err)}};
    return {};
}
#if defined(__APPLE__)
inline std::string launchd_domain_(const service_spec& s) { return s.scope == service_scope::system ? std::string("system") : "gui/" + std::to_string(::getuid()); }
inline std::string launchd_target_(const service_spec& s) { return launchd_domain_(s) + "/" + s.name; }
#endif
#if !defined(__APPLE__) && !defined(FORGE_OS_FREEBSD) && !defined(FORGE_OS_WINDOWS)
inline bool systemd_user_available_() { return !find_executable("systemctl").empty() && (!getenv("XDG_RUNTIME_DIR").empty() || !getenv("DBUS_SESSION_BUS_ADDRESS").empty()); }
inline bool systemd_available_(const service_spec& s) {
    return s.scope == service_scope::system ? !find_executable("systemctl").empty() && os::stat("/run/systemd/system").exists : systemd_user_available_();
}
inline std::vector<std::string> systemctl_(const service_spec& s, std::initializer_list<std::string> rest) {
    std::vector<std::string> v{"systemctl"}; if (s.scope == service_scope::user) v.push_back("--user");
    v.insert(v.end(), rest); return v;
}
#endif

#if defined(FORGE_OS_WINDOWS)
// ---- Windows backends ----
inline error win_err_(const std::string& what, DWORD code) { return {int(code), "service: " + what + ": " + error_text(int(code))}; }
inline std::wstring multi_sz_(const std::vector<std::string>& v) { std::wstring w; for (auto& e : v) { w += widen(e); w += L'\0'; } w += L'\0'; return w; }
// argv[0] resolved through PATH (services and tasks do not search it); batch files are refused: the SCM cannot
// run one and the Task Scheduler would hand its arguments to cmd.exe unescaped.
inline status win_resolve_(service_spec& s) {
    if (auto x = find_executable(s.argv[0]); !x.empty()) s.argv[0] = x;
    if (detail_proc::is_batch(s.argv[0])) return {{}, {EINVAL, "service: argv[0] is a batch file; run it through cmd.exe /c explicitly"}};
    return {};
}
struct sc_handle_ { SC_HANDLE h = nullptr; sc_handle_() = default; explicit sc_handle_(SC_HANDLE x) : h(x) {} ~sc_handle_() { if (h) CloseServiceHandle(h); }
                    sc_handle_(const sc_handle_&) = delete; sc_handle_& operator=(const sc_handle_&) = delete; };
inline status scm_stop_(SC_HANDLE svc) {
    SERVICE_STATUS st{};
    if (!ControlService(svc, SERVICE_CONTROL_STOP, &st)) { DWORD e = GetLastError(); if (e != ERROR_SERVICE_NOT_ACTIVE) return {{}, win_err_("ControlService(stop)", e)}; return {}; }
    for (int i = 0; i < 300; ++i) {   // up to 30 s for SERVICE_STOPPED
        SERVICE_STATUS_PROCESS sp{}; DWORD n = 0;
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&sp), sizeof sp, &n)) return {{}, win_err_("QueryServiceStatusEx", GetLastError())};
        if (sp.dwCurrentState == SERVICE_STOPPED) return {};
        Sleep(100);
    }
    return {{}, {ETIMEDOUT, "service: did not stop within 30 s"}};
}
inline status scm_install_(const service_spec& s) {
    const auto c = render_scm(s); const auto wn = widen(s.name), wcmd = widen(c.command_line), wdisp = widen(c.display_name);
    sc_handle_ m(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    if (!m.h) return {{}, win_err_("OpenSCManager (system services need an elevated process)", GetLastError())};
    const DWORD start = c.auto_start ? SERVICE_AUTO_START : SERVICE_DEMAND_START;
    bool fresh = true;
    sc_handle_ svc(CreateServiceW(m.h, wn.c_str(), wdisp.c_str(), SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, start, SERVICE_ERROR_NORMAL, wcmd.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
    if (!svc.h) {
        DWORD e = GetLastError(); if (e != ERROR_SERVICE_EXISTS) return {{}, win_err_("CreateService", e)};
        fresh = false; svc.h = OpenServiceW(m.h, wn.c_str(), SERVICE_ALL_ACCESS); if (!svc.h) return {{}, win_err_("OpenService", GetLastError())};
        if (!ChangeServiceConfigW(svc.h, SERVICE_WIN32_OWN_PROCESS, start, SERVICE_ERROR_NORMAL, wcmd.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, wdisp.c_str()))
            return {{}, win_err_("ChangeServiceConfig", GetLastError())};
    }
    auto fail = [&](const std::string& what, DWORD e) { if (fresh) DeleteService(svc.h); return status{{}, win_err_(what, e)}; };
    std::wstring wdesc = widen(c.description); SERVICE_DESCRIPTIONW d{wdesc.empty() ? nullptr : wdesc.data()};
    if (!ChangeServiceConfig2W(svc.h, SERVICE_CONFIG_DESCRIPTION, &d)) return fail("ChangeServiceConfig2(description)", GetLastError());
    SC_ACTION acts[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 5000}};
    SERVICE_FAILURE_ACTIONSW fa{}; fa.dwResetPeriod = 86400; fa.cActions = c.restart ? 3 : 0; fa.lpsaActions = acts;   // 0 actions + non-null array clears them
    if (!ChangeServiceConfig2W(svc.h, SERVICE_CONFIG_FAILURE_ACTIONS, &fa)) return fail("ChangeServiceConfig2(failure actions)", GetLastError());
    SERVICE_FAILURE_ACTIONS_FLAG ff{c.restart ? TRUE : FALSE};   // a non-zero exit code (not only a crash) counts as failure
    ChangeServiceConfig2W(svc.h, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &ff);
    const std::wstring key = L"SYSTEM\\CurrentControlSet\\Services\\" + wn;
    if (c.environment.empty()) RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"Environment");
    else {
        auto ms = multi_sz_(c.environment);
        if (LSTATUS e = RegSetKeyValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"Environment", REG_MULTI_SZ, ms.data(), DWORD(ms.size() * sizeof(wchar_t))); e != ERROR_SUCCESS)
            return fail("set Environment", DWORD(e));
    }
    return {};
}
inline status scm_open_(const service_spec& s, DWORD access, sc_handle_& m, sc_handle_& svc, bool& missing) {
    missing = false;
    m.h = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT); if (!m.h) return {{}, win_err_("OpenSCManager", GetLastError())};
    svc.h = OpenServiceW(m.h, widen(s.name).c_str(), access);
    if (!svc.h) { DWORD e = GetLastError(); missing = e == ERROR_SERVICE_DOES_NOT_EXIST; return {{}, win_err_("OpenService " + s.name, e)}; }
    return {};
}
inline service_info scm_query_(const service_spec& s) {
    service_info i; sc_handle_ m, svc; bool missing = false;
    if (!scm_open_(s, SERVICE_QUERY_STATUS, m, svc, missing)) { i.state = missing ? service_state::not_installed : service_state::unknown; return i; }
    SERVICE_STATUS_PROCESS sp{}; DWORD n = 0;
    if (!QueryServiceStatusEx(svc.h, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&sp), sizeof sp, &n)) return i;
    i.pid = sp.dwProcessId;
    i.exit_code = long(sp.dwWin32ExitCode == ERROR_SERVICE_SPECIFIC_ERROR ? sp.dwServiceSpecificExitCode : sp.dwWin32ExitCode);
    switch (sp.dwCurrentState) {
    case SERVICE_RUNNING: case SERVICE_START_PENDING: case SERVICE_CONTINUE_PENDING: i.state = service_state::running; break;
    case SERVICE_STOPPED: i.state = sp.dwWin32ExitCode != NO_ERROR && sp.dwWin32ExitCode != ERROR_SERVICE_NEVER_STARTED ? service_state::failed : service_state::stopped; break;
    default: i.state = service_state::stopped;
    }
    return i;
}
// Task Scheduler through COM, with ole32/oleaut32 loaded at run time. Each call initialises COM on the calling
// thread for its duration (a thread already in another apartment mode is used as it is).
struct ts_api_ {
    HRESULT(WINAPI* init)(LPVOID, DWORD) = nullptr; void(WINAPI* uninit)() = nullptr;
    HRESULT(WINAPI* create)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*) = nullptr;
    BSTR(WINAPI* alloc)(const OLECHAR*) = nullptr; void(WINAPI* free)(BSTR) = nullptr;
    bool ok() const { return init && uninit && create && alloc && free; }
};
inline const ts_api_& ts_api_get_() {
    static ts_api_ a = [] {
        ts_api_ x; HMODULE o = LoadLibraryExW(L"ole32.dll", nullptr, 0x800 /*LOAD_LIBRARY_SEARCH_SYSTEM32*/), a2 = LoadLibraryExW(L"oleaut32.dll", nullptr, 0x800);
        if (!o || !a2) return x;
        auto f = [](HMODULE h, const char* n) { return reinterpret_cast<void*>(GetProcAddress(h, n)); };
        x.init = reinterpret_cast<decltype(x.init)>(f(o, "CoInitializeEx")); x.uninit = reinterpret_cast<decltype(x.uninit)>(f(o, "CoUninitialize"));
        x.create = reinterpret_cast<decltype(x.create)>(f(o, "CoCreateInstance"));
        x.alloc = reinterpret_cast<decltype(x.alloc)>(f(a2, "SysAllocString")); x.free = reinterpret_cast<decltype(x.free)>(f(a2, "SysFreeString"));
        return x;
    }();
    return a;
}
struct bstr_ { BSTR b = nullptr; explicit bstr_(const std::wstring& s) : b(ts_api_get_().alloc(s.c_str())) {} ~bstr_() { if (b) ts_api_get_().free(b); }
               bstr_(const bstr_&) = delete; bstr_& operator=(const bstr_&) = delete; operator BSTR() const { return b; } };
template <class T> struct com_ { T* p = nullptr; ~com_() { if (p) p->Release(); } T* operator->() const { return p; } com_() = default; com_(const com_&) = delete; com_& operator=(const com_&) = delete; };
inline error hr_err_(const std::string& what, HRESULT hr) {
    int code = (hr & 0xFFFF0000) == 0x80070000 ? int(hr & 0xFFFF) : int(hr);
    char b[16]; std::snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(hr));
    return {code, "Task Scheduler: " + what + ": " + b + ((hr & 0xFFFF0000) == 0x80070000 ? " " + error_text(code) : std::string())};
}
inline constexpr HRESULT hr_not_found_ = HRESULT(0x80070002);   // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
// Runs fn(root folder) with a connected ITaskService.
template <class F> status with_task_folder_(F&& fn) {
    const auto& a = ts_api_get_(); if (!a.ok()) return {{}, unsupported("Task Scheduler (ole32/oleaut32)")};
    HRESULT ih = a.init(nullptr, COINIT_MULTITHREADED); const bool un = SUCCEEDED(ih);
    struct guard_ { bool u; ~guard_() { if (u) ts_api_get_().uninit(); } } g{un};
    static const GUID clsid = {0x0f87369f, 0xa4e5, 0x4cfc, {0xbd, 0x3e, 0x73, 0xe6, 0x15, 0x45, 0x72, 0xdd}};   // CLSID_TaskScheduler
    static const GUID iid = {0x2faba4c7, 0x4da9, 0x4013, {0x96, 0x97, 0x20, 0xcc, 0x3f, 0xd4, 0x0f, 0x85}};     // IID_ITaskService
    com_<ITaskService> svc;
    if (HRESULT hr = a.create(clsid, nullptr, CLSCTX_INPROC_SERVER, iid, reinterpret_cast<void**>(&svc.p)); FAILED(hr)) return {{}, hr_err_("CoCreateInstance", hr)};
    VARIANT e{};   // VT_EMPTY: local machine, current user
    if (HRESULT hr = svc->Connect(e, e, e, e); FAILED(hr)) return {{}, hr_err_("Connect", hr)};
    com_<ITaskFolder> root; bstr_ rp(L"\\");
    if (HRESULT hr = svc->GetFolder(rp, &root.p); FAILED(hr)) return {{}, hr_err_("GetFolder", hr)};
    return fn(root.p);
}
inline status task_install_(const service_spec& s) {
    auto u = current_user(); if (u.sid.empty()) return {{}, {EINVAL, "service: cannot determine the current user's SID"}};
    const std::wstring xml = widen(render_task_xml(s, u.sid));
    return with_task_folder_([&](ITaskFolder* f) -> status {
        bstr_ name(widen(s.name)), text(xml); VARIANT e{}; com_<IRegisteredTask> t;
        if (HRESULT hr = f->RegisterTask(name, text, TASK_CREATE_OR_UPDATE, e, e, TASK_LOGON_INTERACTIVE_TOKEN, e, &t.p); FAILED(hr)) return {{}, hr_err_("RegisterTask " + s.name, hr)};
        return {};
    });
}
template <class F> status with_task_(const service_spec& s, F&& fn) {
    return with_task_folder_([&](ITaskFolder* f) -> status {
        bstr_ name(widen(s.name)); com_<IRegisteredTask> t;
        if (HRESULT hr = f->GetTask(name, &t.p); FAILED(hr)) return {{}, hr_err_("GetTask " + s.name, hr)};
        return fn(f, t.p);
    });
}
inline void task_fill_(IRegisteredTask* t, service_info& i) {
    TASK_STATE st = TASK_STATE_UNKNOWN; LONG last = 0;
    if (FAILED(t->get_State(&st))) { i.state = service_state::unknown; return; }
    t->get_LastTaskResult(&last); i.exit_code = long(last);
    com_<IRunningTaskCollection> rc;
    if (SUCCEEDED(t->GetInstances(0, &rc.p)) && rc.p) {
        LONG n = 0; rc->get_Count(&n);
        if (n > 0) { VARIANT idx{}; idx.vt = VT_I4; idx.lVal = 1; com_<IRunningTask> r; DWORD pid = 0; if (SUCCEEDED(rc->get_Item(idx, &r.p)) && r.p && SUCCEEDED(r->get_EnginePID(&pid))) i.pid = pid; }
    }
    // Results that are not failures: SCHED_S_TASK_READY / RUNNING / HAS_NOT_RUN / TERMINATED, 0, and the codes a
    // process ended by Stop() or Ctrl+C reports (DBG_TERMINATE_PROCESS, STATUS_CONTROL_C_EXIT).
    const unsigned long r = static_cast<unsigned long>(last);
    const bool benign = r == 0 || r == 0x41300 || r == 0x41301 || r == 0x41303 || r == 0x41306 || r == 0x40010004 || r == 0xC000013A;
    switch (st) {
    case TASK_STATE_RUNNING: i.state = service_state::running; break;
    case TASK_STATE_READY: case TASK_STATE_QUEUED: case TASK_STATE_DISABLED: i.state = benign ? service_state::stopped : service_state::failed; break;
    default: i.state = service_state::unknown;
    }
}
inline service_info task_query_(const service_spec& s) {
    service_info i;
    auto r = with_task_(s, [&](ITaskFolder*, IRegisteredTask* t) -> status { task_fill_(t, i); return {}; });
    if (!r) i.state = r.err.code == 2 ? service_state::not_installed : service_state::unknown;
    return i;
}
// IRegisteredTask::Stop ends only the action's own process; children (the program behind the PowerShell env
// wrapper above all) would survive. Their handles are opened before Stop (descendants of each running
// instance, created after it, so a reused parent pid cannot match) and terminated after it.
inline std::vector<HANDLE> open_descendants_(DWORD root) {
    std::vector<HANDLE> hs;
    auto created = [](HANDLE h) { FILETIME c{}, x{}, k{}, u{}; return GetProcessTimes(h, &c, &x, &k, &u) ? (ULONGLONG(c.dwHighDateTime) << 32 | c.dwLowDateTime) : 0ull; };
    HANDLE rh = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, root); if (!rh) return hs;
    const ULONGLONG rt = created(rh); CloseHandle(rh);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); if (snap == INVALID_HANDLE_VALUE) return hs;
    std::vector<std::pair<DWORD, DWORD>> all; PROCESSENTRY32W e{}; e.dwSize = sizeof e;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) all.emplace_back(e.th32ProcessID, e.th32ParentProcessID);
    CloseHandle(snap);
    std::vector<DWORD> tree{root};
    for (std::size_t i = 0; i < tree.size(); ++i)
        for (auto& [p, pp] : all)
            if (pp == tree[i] && p != root && std::find(tree.begin(), tree.end(), p) == tree.end())
                if (HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, p)) {
                    if (created(h) >= rt) { tree.push_back(p); hs.push_back(h); } else CloseHandle(h);
                }
    return hs;
}
inline HRESULT task_stop_tree_(IRegisteredTask* t) {
    std::vector<HANDLE> kids;
    com_<IRunningTaskCollection> rc;
    if (SUCCEEDED(t->GetInstances(0, &rc.p)) && rc.p) {
        LONG n = 0; rc->get_Count(&n);
        for (LONG k = 1; k <= n; ++k) {
            VARIANT idx{}; idx.vt = VT_I4; idx.lVal = k; com_<IRunningTask> r; DWORD pid = 0;
            if (SUCCEEDED(rc->get_Item(idx, &r.p)) && r.p && SUCCEEDED(r->get_EnginePID(&pid)) && pid) { auto d = open_descendants_(pid); kids.insert(kids.end(), d.begin(), d.end()); }
        }
    }
    HRESULT hr = t->Stop(0);
    for (HANDLE h : kids) { TerminateProcess(h, 1); CloseHandle(h); }
    return hr;
}
inline status task_stop_(const service_spec& s) {
    return with_task_(s, [&](ITaskFolder*, IRegisteredTask* t) -> status {
        if (HRESULT hr = task_stop_tree_(t); FAILED(hr)) return {{}, hr_err_("Stop " + s.name, hr)};
        for (int k = 0; k < 100; ++k) { TASK_STATE st{}; if (FAILED(t->get_State(&st)) || st != TASK_STATE_RUNNING) return {}; Sleep(50); }
        return {{}, {ETIMEDOUT, "service: task still running 5 s after Stop"}};
    });
}
#endif

#if defined(FORGE_OS_FREEBSD)
inline std::int64_t pidfile_pid_(const std::string& path) { std::ifstream f(path); long long p = 0; return f >> p ? std::int64_t(p) : 0; }
#endif

// Write the unit (and register it with the manager unless spec.dir overrides the location).
inline status install(const service_spec& s) {
    if (auto v = validate(s); !v) return v;
#if defined(FORGE_OS_ANDROID) || defined(FORGE_OS_IOS)
    return {{}, unsupported("service install")};
#elif defined(FORGE_OS_WINDOWS)
    auto r = s; if (auto st = win_resolve_(r); !st) return st;
    if (!s.dir.empty()) {   // the definition only, not registered
        if (s.scope == service_scope::system) return {{}, {EINVAL, "service: spec.dir applies to user (Task Scheduler) services only; see render_scm()"}};
        auto u = current_user();
        std::error_code ec; std::filesystem::create_directories(fs::u8path(s.dir), ec); if (ec) return {{}, {ec.value(), "service: mkdir " + s.dir + ": " + ec.message()}};
        auto w = widen(render_task_xml(r, u.sid.empty() ? u.name : u.sid));   // UTF-16LE with BOM, as declared (schtasks /XML reads it)
        std::string bytes = "\xFF\xFE"; bytes.append(reinterpret_cast<const char*>(w.data()), w.size() * sizeof(wchar_t));
        return fs::write_file_atomic(unit_path(s), bytes);
    }
    return s.scope == service_scope::system ? scm_install_(r) : task_install_(r);
#else
#if defined(FORGE_OS_FREEBSD)
    for (auto& [k, v] : s.env) if (!sh_identifier_(k)) return {{}, {EINVAL, "service: rc.d environment name '" + k + "' is not a shell identifier"}};
    if (s.dir.empty() && s.scope == service_scope::user) return {{}, unsupported("service install (FreeBSD has no per-user service manager; use service_scope::system)")};
#endif
    std::error_code ec; std::filesystem::create_directories(fs::u8path(unit_dir(s)), ec);
    if (ec) return {{}, {ec.value(), "service: mkdir " + unit_dir(s) + ": " + ec.message()}};
#if defined(__APPLE__)
    auto text = render_launchd_plist(s); unsigned mode = 0644;
#elif defined(FORGE_OS_FREEBSD)
    auto text = render_rcd_script(s); unsigned mode = 0755;
#else
    auto text = render_systemd_unit(s); unsigned mode = 0644;
#endif
    if (auto w = fs::write_file_atomic(unit_path(s), text, mode); !w) return w;
    if (!s.dir.empty()) return {};
#if defined(__APPLE__)
    return {};
#elif defined(FORGE_OS_FREEBSD)
    return ctl_ok_({"sysrc", rc_name(s.name) + "_enable=" + (s.start_at_login ? "YES" : "NO")});
#else
    if (!systemd_available_(s)) return {};   // file written; the manager picks it up at next login / boot
    if (auto st = ctl_ok_(systemctl_(s, {"daemon-reload"})); !st) return st;
    if (s.start_at_login) return ctl_ok_(systemctl_(s, {"enable", s.name + ".service"}));
    return {};
#endif
#endif
}
inline status start(const service_spec& s) {
#if defined(FORGE_OS_WINDOWS)
    if (s.scope == service_scope::user)
        return with_task_(s, [&](ITaskFolder*, IRegisteredTask* t) -> status {
            VARIANT e{}; com_<IRunningTask> r;
            if (HRESULT hr = t->Run(e, &r.p); FAILED(hr)) return {{}, hr_err_("Run " + s.name, hr)};
            return {};
        });
    sc_handle_ m, svc; bool missing = false;
    if (auto st = scm_open_(s, SERVICE_START, m, svc, missing); !st) return st;
    if (!StartServiceW(svc.h, 0, nullptr)) { DWORD e = GetLastError(); if (e != ERROR_SERVICE_ALREADY_RUNNING) return {{}, win_err_("StartService", e)}; }
    return {};
#elif defined(__APPLE__) && !defined(FORGE_OS_IOS)
    ctl_({"launchctl", "enable", launchd_target_(s)});   // clear a disabled override (what load -w did)
    if (auto r = ctl_({"launchctl", "print", launchd_target_(s)}); !r || r->exit_code != 0)
        if (auto b = ctl_ok_({"launchctl", "bootstrap", launchd_domain_(s), unit_path(s)}); !b) return b;
    return ctl_ok_({"launchctl", "kickstart", launchd_target_(s)});
#elif defined(FORGE_OS_ANDROID) || defined(FORGE_OS_IOS)
    (void)s; return {{}, unsupported("service start")};
#elif defined(FORGE_OS_FREEBSD)
    if (!fs::exists(unit_path(s))) return {{}, {ENOENT, "service: " + unit_path(s) + " not installed"}};
    return ctl_ok_({"service", s.name, "onestart"});
#else
    if (!systemd_available_(s)) return {{}, unsupported("service start (no systemd session)")};
    return ctl_ok_(systemctl_(s, {"start", s.name + ".service"}));
#endif
}
inline status stop(const service_spec& s) {
#if defined(FORGE_OS_WINDOWS)
    if (s.scope == service_scope::user) return task_stop_(s);
    sc_handle_ m, svc; bool missing = false;
    if (auto st = scm_open_(s, SERVICE_STOP | SERVICE_QUERY_STATUS, m, svc, missing); !st) return st;
    return scm_stop_(svc.h);
#elif defined(__APPLE__) && !defined(FORGE_OS_IOS)
    return ctl_ok_({"launchctl", "bootout", launchd_target_(s)});
#elif defined(FORGE_OS_ANDROID) || defined(FORGE_OS_IOS)
    (void)s; return {{}, unsupported("service stop")};
#elif defined(FORGE_OS_FREEBSD)
    if (!fs::exists(unit_path(s))) return {{}, {ENOENT, "service: " + unit_path(s) + " not installed"}};
    return ctl_ok_({"service", s.name, "onestop"});
#else
    if (!systemd_available_(s)) return {{}, unsupported("service stop (no systemd session)")};
    return ctl_ok_(systemctl_(s, {"stop", s.name + ".service"}));
#endif
}
inline service_info query_info(const service_spec& s) {
    service_info i;
#if defined(FORGE_OS_WINDOWS)
    if (!s.dir.empty()) { i.state = fs::exists(unit_path(s)) ? service_state::stopped : service_state::not_installed; return i; }
    return s.scope == service_scope::system ? scm_query_(s) : task_query_(s);
#elif defined(FORGE_OS_ANDROID) || defined(FORGE_OS_IOS)
    (void)s; return i;
#else
    if (!fs::exists(unit_path(s))) { i.state = service_state::not_installed; return i; }
#if defined(__APPLE__)
    auto r = ctl_({"launchctl", "print", launchd_target_(s)}); if (!r) return i;
    if (r->exit_code != 0) { i.state = service_state::stopped; return i; }   // not bootstrapped = stopped
    i.state = parse_launchctl_print(r->out); i.pid = std::strtoll(launchctl_field_(r->out, "pid").c_str(), nullptr, 10);
    if (auto lx = launchctl_field_(r->out, "last exit code"); !lx.empty() && lx[0] != '(') i.exit_code = std::strtol(lx.c_str(), nullptr, 10);
    return i;
#elif defined(FORGE_OS_FREEBSD)
    if (!s.dir.empty()) { i.state = service_state::stopped; return i; }
    auto r = ctl_({"service", s.name, "onestatus"}); if (!r) return i;
    i.state = r->exit_code == 0 ? service_state::running : service_state::stopped;
    if (i.state == service_state::running) i.pid = pidfile_pid_("/var/run/" + rc_name(s.name) + ".pid");
    return i;
#else
    if (!systemd_available_(s) || !s.dir.empty()) { i.state = service_state::stopped; return i; }
    auto r = ctl_(systemctl_(s, {"is-active", s.name + ".service"})); if (!r) return i;
    auto o = r->out; while (!o.empty() && (o.back() == '\n' || o.back() == ' ')) o.pop_back();
    i.state = o == "active" || o == "activating" || o == "reloading" ? service_state::running : o == "failed" ? service_state::failed : service_state::stopped;
    if (i.state == service_state::running)
        if (auto p = ctl_(systemctl_(s, {"show", "-p", "MainPID", "--value", s.name + ".service"})); p && p->exit_code == 0) i.pid = std::strtoll(p->out.c_str(), nullptr, 10);
    return i;
#endif
#endif
}
inline service_state query(const service_spec& s) { return query_info(s).state; }
inline status uninstall(const service_spec& s) {
#if defined(FORGE_OS_WINDOWS)
    if (!s.dir.empty()) { std::error_code ec; std::filesystem::remove(fs::u8path(unit_path(s)), ec); if (ec) return {{}, {ec.value(), "service: remove " + unit_path(s) + ": " + ec.message()}}; return {}; }
    if (s.scope == service_scope::user) {
        return with_task_folder_([&](ITaskFolder* f) -> status {
            bstr_ name(widen(s.name)); com_<IRegisteredTask> t;
            if (HRESULT hr = f->GetTask(name, &t.p); FAILED(hr)) return hr == hr_not_found_ ? status{} : status{{}, hr_err_("GetTask " + s.name, hr)};
            task_stop_tree_(t.p);
            if (HRESULT hr = f->DeleteTask(name, 0); FAILED(hr) && hr != hr_not_found_) return {{}, hr_err_("DeleteTask " + s.name, hr)};
            return {};
        });
    }
    sc_handle_ m, svc; bool missing = false;
    if (auto st = scm_open_(s, DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS, m, svc, missing); !st) return missing ? status{} : st;
    scm_stop_(svc.h);
    if (!DeleteService(svc.h)) { DWORD e = GetLastError(); if (e != ERROR_SERVICE_MARKED_FOR_DELETE) return {{}, win_err_("DeleteService", e)}; }
    return {};
#elif defined(FORGE_OS_ANDROID) || defined(FORGE_OS_IOS)
    (void)s; return {{}, unsupported("service uninstall")};
#else
    if (s.dir.empty()) {
#if defined(__APPLE__)
        ctl_({"launchctl", "bootout", launchd_target_(s)});
#elif defined(FORGE_OS_FREEBSD)
        if (fs::exists(unit_path(s))) ctl_({"service", s.name, "onestop"});
        ctl_({"sysrc", "-x", rc_name(s.name) + "_enable"});
#else
        if (systemd_available_(s)) { ctl_(systemctl_(s, {"disable", "--now", s.name + ".service"})); }
#endif
    }
    std::error_code ec; std::filesystem::remove(fs::u8path(unit_path(s)), ec);
    if (ec) return {{}, {ec.value(), "service: remove " + unit_path(s) + ": " + ec.message()}};
#if !defined(__APPLE__) && !defined(FORGE_OS_FREEBSD)
    if (s.dir.empty() && systemd_available_(s)) ctl_(systemctl_(s, {"daemon-reload"}));
#endif
    return {};
#endif
}
} // namespace forge::os::service
