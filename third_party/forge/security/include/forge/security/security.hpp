#pragma once
// forge::security: the FORGE Security Standard (docs/security-standard.md, API in docs/security-api.md).
//   errors.hpp    security_error > policy_violation, integrity_error, permission_denied, sandbox_error (FSS-ERR)
//   audit.hpp     audit(): OS facility, JSON-lines file, callbacks, logger (FSS-AUD)
//   path.hpp      safe_join, canonical_within, check_relative (FSS-CAP-2/3)
//   policy.hpp    policy, profiles, parse_policy, load_policy
//   enforce.hpp   enforce / try_enforce, the active policy (FSS-CAP-1)
//   integrity.hpp detached signatures (.sig), verify_artefact, os_code_signature (FSS-INT-1..4)
//   plugin.hpp    open_plugin: verified native plugin loads (FSS-INT-1/2/3)
//   lockdown.hpp  lockdown, check (FSS-SBX, Verification, FSS-BLD-2)
//   secure_buffer.hpp  secure_buffer, secure_bytes<N>, secure_string: locked, non-dumpable key memory (FSS-CRY-2)
//   files.hpp     check_private_file / check_private_dir / require_private_file (FSS-FIO-2)
#include <forge/security/errors.hpp>
#include <forge/security/secure_buffer.hpp>
#include <forge/security/files.hpp>
#include <forge/security/audit.hpp>
#include <forge/security/path.hpp>
#include <forge/security/policy.hpp>
#include <forge/security/integrity.hpp>
#include <forge/security/enforce.hpp>
#include <forge/security/plugin.hpp>
#include <forge/security/lockdown.hpp>
