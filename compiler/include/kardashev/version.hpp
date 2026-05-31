// The single source of truth for the kardashev toolchain version.
//
// Versioning policy (SemVer): the project is pre-1.0, so each completed
// ROADMAP is a MINOR bump (Roadmap v15 -> 0.15.0, v16 -> 0.16.0); bug-fix
// releases bump PATCH. 1.0 is reserved for a language-surface stability
// commitment, after which the language evolves via opt-in editions rather than
// MAJOR bumps (the Rust model). Keep this in sync with `MODULE.bazel`'s
// `version` and `CHANGELOG.md`. Reported by `kardc --version`.

#pragma once

namespace kardashev {

inline constexpr const char* kVersion = "0.20.0";

} // namespace kardashev
