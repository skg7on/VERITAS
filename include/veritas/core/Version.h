// Version.h — VERITAS version identity.
//
// GetVersion() returns the semantic version baked in at configure time
// (PROJECT_VERSION_{MAJOR,MINOR,PATCH}) plus the short git SHA of HEAD at
// the moment CMake configured the build. FormatVersion() renders the
// shared `--version` string used by every VERITAS CLI.

#ifndef VERITAS_CORE_VERSION_H_
#define VERITAS_CORE_VERSION_H_

#include <string>

namespace veritas {

struct Version {
  int major;
  int minor;
  int patch;
  std::string git_revision;
};

Version GetVersion();

// Renders "VERITAS <major>.<minor>.<patch> (<git-revision>)".
std::string FormatVersion(const Version& version);

}  // namespace veritas

#endif  // VERITAS_CORE_VERSION_H_
