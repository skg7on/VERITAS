// Copyright 2026 VERITAS Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "veritas/core/AtomicFile.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace veritas::core {

Status WriteFileAtomically(const std::string& destination,
                           const std::string& bytes) {
  namespace fs = std::filesystem;
  std::error_code ignored;

  std::string pattern = destination + ".tmp.XXXXXX";
  std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
  mutable_pattern.push_back('\0');
  const int reservation = ::mkstemp(mutable_pattern.data());
  if (reservation < 0) {
    return Status::Internal("cannot create the temporary file beside '" +
                            destination + "'");
  }
  const fs::path temporary(mutable_pattern.data());
  if (::close(reservation) != 0 || ::unlink(temporary.c_str()) != 0) {
    fs::remove(temporary, ignored);
    return Status::Internal("cannot prepare the temporary file beside '" +
                            destination + "'");
  }

  // mkstemp reserves an unpredictable name without following a symlink, but
  // fixes its mode at 0600. Re-create that reserved name with O_EXCL so the
  // kernel applies the caller's umask to 0666, matching ordinary file-output
  // semantics. If another process wins the short gap, open fails safely rather
  // than following or replacing what it created.
  const int descriptor = ::open(temporary.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
  if (descriptor < 0) {
    return Status::Internal("cannot create the temporary file beside '" +
                            destination + "'");
  }

  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t count =
        ::write(descriptor, bytes.data() + written, bytes.size() - written);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      (void)::close(descriptor);
      fs::remove(temporary, ignored);
      return Status::Internal("cannot write the temporary file beside '" +
                              destination + "'");
    }
    written += static_cast<std::size_t>(count);
  }
  if (::close(descriptor) != 0) {
    fs::remove(temporary, ignored);
    return Status::Internal("cannot close the temporary file beside '" +
                            destination + "'");
  }

  std::error_code rename_error;
  fs::rename(temporary, fs::path(destination), rename_error);
  if (rename_error) {
    fs::remove(temporary, ignored);
    return Status::Internal("cannot replace '" + destination +
                            "': " + rename_error.message());
  }
  return Status::Ok();
}

}  // namespace veritas::core
