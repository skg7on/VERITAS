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

// Status.h — error propagation primitives for VERITAS.
//
// Modelled loosely on absl::Status / absl::StatusOr: a lightweight value type
// carrying either success or a structured error, plus a StatusOr<T> that
// couples a Status with an optional payload. Every VERITAS API that can fail
// should return one of these instead of throwing.

#ifndef VERITAS_CORE_STATUS_H_
#define VERITAS_CORE_STATUS_H_

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace veritas {

enum class StatusCode {
  kOk = 0,
  kInvalidArgument,
  kNotFound,
  kFailedPrecondition,
  kInternal,
  kDeadlineExceeded,
};

class Status {
 public:
  Status() : code_(StatusCode::kOk) {}

  static Status Ok() { return Status(); }
  static Status InvalidArgument(std::string message) {
    return Status(StatusCode::kInvalidArgument, std::move(message));
  }
  static Status NotFound(std::string message) {
    return Status(StatusCode::kNotFound, std::move(message));
  }
  static Status FailedPrecondition(std::string message) {
    return Status(StatusCode::kFailedPrecondition, std::move(message));
  }
  static Status Internal(std::string message) {
    return Status(StatusCode::kInternal, std::move(message));
  }
  static Status DeadlineExceeded(std::string message) {
    return Status(StatusCode::kDeadlineExceeded, std::move(message));
  }

  bool ok() const { return code_ == StatusCode::kOk; }
  StatusCode code() const { return code_; }
  std::string_view message() const { return message_; }

 private:
  Status(StatusCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  StatusCode code_;
  std::string message_;
};

template <typename T>
class StatusOr {
 public:
  StatusOr(T value) : status_(Status::Ok()), value_(std::move(value)) {}
  StatusOr(Status status) : status_(std::move(status)) {
    // An Ok status with no value is a programming error: StatusOr must
    // always resolve to either "value" or "non-ok status".
  }

  bool ok() const { return status_.ok() && value_.has_value(); }
  const Status& status() const { return status_; }

  const T& value() const& { return *value_; }
  T& value() & { return *value_; }
  T&& value() && { return std::move(*value_); }

  const T& operator*() const& { return *value_; }
  T& operator*() & { return *value_; }
  T&& operator*() && { return std::move(*value_); }

  const T* operator->() const { return &*value_; }
  T* operator->() { return &*value_; }

 private:
  Status status_;
  std::optional<T> value_;
};

}  // namespace veritas

#endif  // VERITAS_CORE_STATUS_H_
