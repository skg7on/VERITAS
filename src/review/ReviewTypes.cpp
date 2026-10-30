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

#include "veritas/review/ReviewTypes.h"
#include "ReviewSchema.h"
#include "veritas/core/Hash.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <regex>
#include <set>
namespace veritas::review {

Json::Kind Json::kind() const {
  switch (data.index()) {
  case 0:
    return Null;
  case 1:
    return Boolean;
  case 2:
  case 3:
    return Number;
  case 4:
    return String;
  case 5:
    return List;
  default:
    return Map;
  }
}
std::optional<bool> Json::getAsBoolean() const {
  if (auto p = std::get_if<bool>(&data))
    return *p;
  return std::nullopt;
}
std::optional<int64_t> Json::getAsInteger() const {
  if (auto p = std::get_if<int64_t>(&data))
    return *p;
  if (auto p = std::get_if<uint64_t>(&data);
      p && *p <= uint64_t(std::numeric_limits<int64_t>::max()))
    return int64_t(*p);
  return std::nullopt;
}
std::optional<uint64_t> Json::getAsUINT64() const {
  if (auto p = std::get_if<uint64_t>(&data))
    return *p;
  if (auto p = std::get_if<int64_t>(&data); p && *p >= 0)
    return uint64_t(*p);
  return std::nullopt;
}
std::optional<std::string> Json::getAsString() const {
  if (auto p = std::get_if<std::string>(&data))
    return *p;
  return std::nullopt;
}
const Json *JsonObject::get(std::string_view key) const {
  auto it = find(key);
  return it == end() ? nullptr : &it->second;
}
std::optional<std::string> JsonObject::getString(std::string_view key) const {
  auto p = get(key);
  return p ? p->getAsString() : std::nullopt;
}
std::optional<bool> JsonObject::getBoolean(std::string_view key) const {
  auto p = get(key);
  return p ? p->getAsBoolean() : std::nullopt;
}
const JsonObject *JsonObject::getObject(std::string_view key) const {
  auto p = get(key);
  return p ? p->getAsObject() : nullptr;
}
const std::vector<Json> *JsonObject::getArray(std::string_view key) const {
  auto p = get(key);
  return p ? p->getAsArray() : nullptr;
}
namespace {
using V = Json;
using O = Json::Object;
using A = Json::Array;
Status Bad() { return Status::InvalidArgument("invalid closed v2 wire value"); }
class Parser {
public:
  explicit Parser(std::string_view text) : text_(text) {}
  StatusOr<V> Parse(unsigned depth = 0) {
    Space();
    if (depth > 128 || pos_ == text_.size())
      return Bad();
    char c = text_[pos_];
    if (c == '"')
      return String();
    if (c == '{' || c == '[') {
      ++pos_;
      Space();
      bool object = c == '{';
      char end = object ? '}' : ']';
      O obj;
      A arr;
      if (Take(end))
        return object ? V(std::move(obj)) : V(std::move(arr));
      while (true) {
        std::string key;
        if (object) {
          auto parsed = String();
          if (!parsed.ok())
            return parsed.status();
          key = *parsed->getAsString();
          Space();
          if (!Take(':') || obj.get(key))
            return Bad();
        }
        auto child = Parse(depth + 1);
        if (!child.ok())
          return child.status();
        if (object)
          obj[key] = std::move(*child);
        else
          arr.push_back(std::move(*child));
        Space();
        if (Take(end))
          break;
        if (!Take(','))
          return Bad();
        Space();
      }
      return object ? V(std::move(obj)) : V(std::move(arr));
    }
    for (auto literal : {"true", "false", "null"}) {
      std::string_view l = literal;
      if (text_.substr(pos_, l.size()) == l) {
        pos_ += l.size();
        if (l == "null")
          return V(nullptr);
        return V(l == "true");
      }
    }
    size_t start = pos_;
    if (c == '-')
      ++pos_;
    if (pos_ == text_.size() || text_[pos_] < '0' || text_[pos_] > '9')
      return Bad();
    if (text_[pos_] == '0')
      ++pos_;
    else
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9')
        ++pos_;
    auto token = text_.substr(start, pos_ - start);
    if (token.front() == '-') {
      int64_t n;
      auto result =
          std::from_chars(token.data(), token.data() + token.size(), n);
      if (result.ec != std::errc{})
        return Bad();
      return V(n);
    }
    uint64_t n;
    auto result = std::from_chars(token.data(), token.data() + token.size(), n);
    if (result.ec != std::errc{})
      return Bad();
    return V(n);
  }
  bool Done() {
    Space();
    return pos_ == text_.size();
  }

private:
  void Space() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' ||
                                   text_[pos_] == '\r' || text_[pos_] == '\t'))
      ++pos_;
  }
  bool Take(char c) {
    if (pos_ < text_.size() && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }
  StatusOr<V> String() {
    if (!Take('"'))
      return Bad();
    size_t start = pos_ - 1;
    while (pos_ < text_.size()) {
      char c = text_[pos_++];
      if (c == '\\') {
        if (pos_ == text_.size())
          return Bad();
        if (text_[pos_++] == 'u') {
          auto unit = [&]() -> int {
            if (text_.size() - pos_ < 4)
              return -1;
            int n = 0;
            for (int i = 0; i < 4; ++i) {
              char h = text_[pos_++];
              int d = h >= '0' && h <= '9'   ? h - '0'
                      : h >= 'a' && h <= 'f' ? h - 'a' + 10
                      : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                             : -1;
              if (d < 0)
                return -1;
              n = n * 16 + d;
            }
            return n;
          };
          int first = unit();
          if (first < 0)
            return Bad();
          if (first >= 0xd800 && first <= 0xdbff) {
            if (!Take('\\') || !Take('u'))
              return Bad();
            int low = unit();
            if (low < 0xdc00 || low > 0xdfff)
              return Bad();
          } else if (first >= 0xdc00 && first <= 0xdfff)
            return Bad();
        }
      } else if (c == '"') {
        auto parsed = llvm::json::parse(
            llvm::StringRef(text_.data() + start, pos_ - start));
        if (!parsed) {
          llvm::consumeError(parsed.takeError());
          return Bad();
        }
        return V(parsed->getAsString()->str());
      }
    }
    return Bad();
  }
  std::string_view text_;
  size_t pos_ = 0;
};
void Quote(std::string_view s, std::string &out) {
  out += '"';
  const char *hex = "0123456789abcdef";
  for (unsigned char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 32) {
        out += "\\u00";
        out += hex[c >> 4];
        out += hex[c & 15];
      } else
        out += static_cast<char>(c);
    }
  }
  out += '"';
}
Status Encode(const V &v, std::string &out, unsigned depth = 0) {
  if (depth > 128)
    return Bad();
  if (v.kind() == V::Null)
    out += "null";
  else if (auto b = v.getAsBoolean())
    out += *b ? "true" : "false";
  else if (auto n = v.getAsUINT64())
    out += std::to_string(*n);
  else if (auto n = v.getAsInteger())
    out += std::to_string(*n);
  else if (auto s = v.getAsString()) {
    if (!llvm::json::isUTF8(*s))
      return Bad();
    Quote(*s, out);
  } else if (auto a = v.getAsArray()) {
    out += '[';
    bool first = true;
    for (const auto &x : *a) {
      if (!first)
        out += ',';
      first = false;
      auto status = Encode(x, out, depth + 1);
      if (!status.ok())
        return status;
    }
    out += ']';
  } else if (auto o = v.getAsObject()) {
    std::map<std::string, const V *> sorted;
    for (const auto &x : *o)
      sorted.emplace(x.first, &x.second);
    out += '{';
    bool first = true;
    for (const auto &[k, x] : sorted) {
      if (!llvm::json::isUTF8(k))
        return Bad();
      if (!first)
        out += ',';
      first = false;
      Quote(k, out);
      out += ':';
      auto status = Encode(*x, out, depth + 1);
      if (!status.ok())
        return status;
    }
    out += '}';
  } else
    return Bad();
  return Status::Ok();
}
const V &Schema() {
  static const V schema = [] {
    auto s = ParseStrictJson(kReviewSchema);
    return std::move(*s);
  }();
  return schema;
}
std::string Text(const O &o, std::string_view key) {
  auto s = o.getString(key);
  return s ? *s : "";
}
std::optional<uint64_t> UInt(const O &o, std::string_view key) {
  auto v = o.get(key);
  return v ? v->getAsUINT64() : std::nullopt;
}
Status Validate(const V &v, const V &rule, unsigned depth = 0) {
  if (depth > 160)
    return Bad();
  const auto &s = *rule.getAsObject();
  if (auto r = s.getString("$ref"))
    return Validate(
        v, *Schema().getAsObject()->getObject("$defs")->get(r->substr(8)),
        depth + 1);
  if (auto choices = s.getArray("oneOf")) {
    size_t count = 0;
    for (const auto &c : *choices)
      if (Validate(v, c, depth + 1).ok())
        ++count;
    return count == 1 ? Status::Ok() : Bad();
  }
  if (auto c = s.get("const"); c && *c != v)
    return Bad();
  if (auto e = s.getArray("enum");
      e && std::find(e->begin(), e->end(), v) == e->end())
    return Bad();
  auto t = s.getString("type");
  if (!t)
    return Status::Ok();
  if (*t == "null")
    return v.kind() == V::Null ? Status::Ok() : Bad();
  if (*t == "boolean")
    return v.getAsBoolean().has_value() ? Status::Ok() : Bad();
  if (*t == "integer") {
    auto n = v.getAsUINT64();
    if (!n)
      return Bad();
    auto lo = UInt(s, "minimum"), hi = UInt(s, "maximum");
    return (lo && *n < *lo) || (hi && *n > *hi) ? Bad() : Status::Ok();
  }
  if (*t == "string") {
    auto text = v.getAsString();
    if (!text)
      return Bad();
    if (auto min = UInt(s, "minLength"); min && text->size() < *min)
      return Bad();
    if (auto p = s.getString("pattern");
        p && !std::regex_search(*text, std::regex(*p)))
      return Bad();
    return Status::Ok();
  }
  if (*t == "array") {
    auto a = v.getAsArray();
    if (!a)
      return Bad();
    if (auto min = UInt(s, "minItems"); min && a->size() < *min)
      return Bad();
    std::set<std::string> seen;
    for (const auto &x : *a) {
      auto status = Validate(x, *s.get("items"), depth + 1);
      if (!status.ok())
        return status;
      if (s.getBoolean("uniqueItems").value_or(false)) {
        auto bytes = CanonicalJson(x);
        if (!bytes.ok() || !seen.insert(*bytes).second)
          return Bad();
      }
    }
    return Status::Ok();
  }
  auto o = v.getAsObject();
  if (!o)
    return Bad();
  auto props = s.getObject("properties");
  if (!props || o->size() != props->size())
    return Bad();
  for (const auto &p : *props) {
    auto x = o->get(p.first);
    if (!x)
      return Bad();
    auto st = Validate(*x, p.second, depth + 1);
    if (!st.ok())
      return st;
  }
  auto name = Text(s, "title");
  if (name == "ReviewBinding") {
    std::vector<ProviderVersion> versions;
    std::set<std::string> names;
    for (const auto &p : *o->getArray("provider_versions")) {
      auto &pv = *p.getAsObject();
      versions.push_back(
          {Text(pv, "name"), Text(pv, "version"), Text(pv, "config_digest")});
      if (!names.insert(versions.back().name).second)
        return Bad();
    }
    if (!std::is_sorted(versions.begin(), versions.end()))
      return Bad();
  }
  if (name == "LocalRequest") {
    std::string previous;
    for (const auto &slot : *o->getArray("requested_slots")) {
      auto cur = *slot.getAsString();
      if (cur < previous)
        return Bad();
      previous = cur;
    }
  }
  if (name == "Region" || name == "AnchorRef" || name == "EntityBinding" ||
      name == "ContextArtifact") {
    if (*UInt(*o, "start") > *UInt(*o, "end"))
      return Bad();
  }
  if (name == "ContextArtifact") {
    auto start = *UInt(*o, "start"), end = *UInt(*o, "end");
    if (end - start != o->getString("excerpt")->size())
      return Bad();
    for (const auto &x : *o->getArray("entity_bindings")) {
      auto b = x.getAsObject();
      if (*UInt(*b, "start") < start || *UInt(*b, "end") > end)
        return Bad();
    }
  }
  if (name == "ProgramScope" &&
      Text(*o, "membership_source") == "OUTPUT_HINT" &&
      Text(*o, "coverage") == "DECLARED_CLOSED")
    return Bad();
  return Status::Ok();
}
A Strings(const std::vector<std::string> &strings) {
  A a;
  for (const auto &s : strings)
    a.push_back(s);
  return a;
}
} // namespace
bool IsDigest(std::string_view text) {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
std::string HashBytes(std::string_view text) {
  return core::DigestToHex(
      core::ComputeSHA256(std::as_bytes(std::span(text.data(), text.size()))));
}
StatusOr<V> ParseStrictJson(std::string_view text) {
  if (text.size() > 1024 * 1024 ||
      !llvm::json::isUTF8(llvm::StringRef(text.data(), text.size())))
    return Bad();
  Parser parser(text);
  auto value = parser.Parse();
  if (!value.ok())
    return value.status();
  if (!parser.Done())
    return Bad();
  return std::move(*value);
}
StatusOr<std::string> CanonicalJson(const V &value) {
  std::string out;
  auto status = Encode(value, out);
  if (!status.ok())
    return status;
  return out;
}
Status ValidateWireType(const V &value, std::string_view name) {
  auto rule = Schema().getAsObject()->getObject("$defs")->get(name);
  if (!rule)
    return Bad();
  return Validate(value, *rule);
}
V ToJson(const ReviewBinding &b) {
  A providers;
  for (const auto &p : b.provider_versions)
    providers.push_back(O{{"name", p.name},
                          {"version", p.version},
                          {"config_digest", p.config_digest}});
  V native = nullptr;
  if (b.native_binding)
    native = O{{"analysis_run_id", b.native_binding->analysis_run_id},
               {"projection_id", b.native_binding->projection_id},
               {"fact_snapshot_id", b.native_binding->fact_snapshot_id}};
  return O{
      {"snapshot_id", b.snapshot_id ? V(b.snapshot_id->value()) : V(nullptr)},
      {"repository_id", b.repository_id},
      {"base_revision_id", b.base_revision_id},
      {"working_tree_digest", b.working_tree_digest},
      {"program_scope_id",
       b.program_scope_id ? V(b.program_scope_id->value()) : V(nullptr)},
      {"build_context_digest", b.build_context_digest},
      {"native_binding", std::move(native)},
      {"provider_versions", std::move(providers)}};
}
V ToJson(const ProgramScope &s) {
  return O{
      {"member_tu_digests", Strings(s.member_tu_digests)},
      {"dependency_artifact_digests", Strings(s.dependency_artifact_digests)},
      {"target_digest", s.target_digest},
      {"membership_source", s.membership_source},
      {"coverage", s.coverage},
      {"assumptions", Strings(s.assumptions)},
      {"frontier", Strings(s.frontier)}};
}
StatusOr<ReviewBinding> DecodeReviewBinding(const V &value) {
  auto status = ValidateWireType(value, "ReviewBinding");
  if (!status.ok())
    return status;
  const auto &o = *value.getAsObject();
  ReviewBinding b;
  b.snapshot_id = *ParseArtifactId<SnapshotTag>(Text(o, "snapshot_id"));
  b.program_scope_id =
      *ParseArtifactId<ProgramScopeTag>(Text(o, "program_scope_id"));
  b.repository_id = Text(o, "repository_id");
  b.base_revision_id = Text(o, "base_revision_id");
  b.working_tree_digest = Text(o, "working_tree_digest");
  b.build_context_digest = Text(o, "build_context_digest");
  if (auto n = o.getObject("native_binding"))
    b.native_binding =
        NativeBinding{Text(*n, "analysis_run_id"), Text(*n, "projection_id"),
                      Text(*n, "fact_snapshot_id")};
  for (const auto &p : *o.getArray("provider_versions")) {
    auto &x = *p.getAsObject();
    b.provider_versions.push_back(
        {Text(x, "name"), Text(x, "version"), Text(x, "config_digest")});
  }
  return b;
}
StatusOr<BackendEnvelope> DecodeV2(std::string_view text,
                                   std::string_view expected_id,
                                   std::string_view expected_op) {
  auto value = ParseStrictJson(text);
  if (!value.ok())
    return value.status();
  auto st = Validate(*value, Schema());
  if (!st.ok())
    return st;
  const auto &o = *value->getAsObject();
  BackendEnvelope result{Text(o, "request_id"), Text(o, "operation"),
                         o.getBoolean("ok"), std::nullopt, std::move(*value)};
  if (!expected_id.empty() && result.request_id != expected_id)
    return Bad();
  auto &doc = *result.document.getAsObject();
  auto payload = doc.getObject("parameters");
  if (!payload)
    payload = doc.getObject("result");
  if (!expected_op.empty()) {
    if (!result.operation.empty() && result.operation != expected_op)
      return Bad();
    if (result.ok.value_or(false)) {
      static const std::map<std::string_view, std::string_view> names = {
          {"handshake", "HandshakeResult"},
          {"capture", "CaptureResult"},
          {"context", "ContextResult"},
          {"seed-candidates", "SeedResult"},
          {"local-evidence", "LocalEvidence"},
          {"native-evidence", "NativeResult"},
          {"verify", "VerifyResult"},
          {"release", "ReleaseResult"}};
      auto it = names.find(expected_op);
      if (it == names.end() ||
          !ValidateWireType(*doc.get("result"), it->second).ok())
        return Bad();
    }
  }
  if (payload && payload->get("binding")) {
    auto b = DecodeReviewBinding(*payload->get("binding"));
    if (!b.ok())
      return b.status();
    result.binding = std::move(*b);
  }
  return result;
}
} // namespace veritas::review
