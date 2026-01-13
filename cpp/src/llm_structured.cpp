#include "llm_structured.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <set>
#include <regex>
#include <sstream>

namespace llm_structured {

// ---------------- Json helpers ----------------

bool Json::is_null() const { return std::holds_alternative<std::nullptr_t>(value); }
bool Json::is_bool() const { return std::holds_alternative<bool>(value); }
bool Json::is_number() const { return std::holds_alternative<double>(value); }
bool Json::is_string() const { return std::holds_alternative<std::string>(value); }
bool Json::is_array() const { return std::holds_alternative<JsonArray>(value); }
bool Json::is_object() const { return std::holds_alternative<JsonObject>(value); }

const bool& Json::as_bool() const { return std::get<bool>(value); }
const double& Json::as_number() const { return std::get<double>(value); }
const std::string& Json::as_string() const { return std::get<std::string>(value); }
const JsonArray& Json::as_array() const { return std::get<JsonArray>(value); }
const JsonObject& Json::as_object() const { return std::get<JsonObject>(value); }

JsonArray& Json::as_array() { return std::get<JsonArray>(value); }
JsonObject& Json::as_object() { return std::get<JsonObject>(value); }

static std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

static std::string ltrim_copy(std::string s) {
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
  return s.substr(i);
}

static std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::string cur;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '\r') continue;
    if (c == '\n') {
      lines.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  lines.push_back(cur);
  return lines;
}

static std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          std::ostringstream oss;
          oss << "\\u";
          oss.setf(std::ios::hex, std::ios::basefield);
          oss.width(4);
          oss.fill('0');
          oss << (static_cast<int>(static_cast<unsigned char>(c)));
          out += oss.str();
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

static std::string json_pointer_escape(const std::string& seg) {
  std::string out;
  out.reserve(seg.size());
  for (char c : seg) {
    if (c == '~') {
      out += "~0";
    } else if (c == '/') {
      out += "~1";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string json_pointer_from_path(const std::string& json_path) {
  // Best-effort JSONPath-ish -> RFC6901 pointer.
  if (json_path.empty() || json_path[0] != '$') return "";

  std::vector<std::string> segs;
  size_t i = 1;
  while (i < json_path.size()) {
    char c = json_path[i];
    if (c == '.') {
      ++i;
      size_t start = i;
      while (i < json_path.size()) {
        char cc = json_path[i];
        if (cc == '.' || cc == '[') break;
        ++i;
      }
      if (i > start) segs.push_back(json_path.substr(start, i - start));
      continue;
    }
    if (c == '[') {
      ++i;
      size_t start = i;
      while (i < json_path.size() && json_path[i] != ']') ++i;
      if (i > start) {
        std::string inner = json_path.substr(start, i - start);
        if (inner.size() >= 2 && ((inner.front() == '"' && inner.back() == '"') || (inner.front() == '\'' && inner.back() == '\''))) {
          inner = inner.substr(1, inner.size() - 2);
        }
        segs.push_back(inner);
      }
      if (i < json_path.size() && json_path[i] == ']') ++i;
      continue;
    }
    ++i;
  }

  std::string out;
  for (const auto& seg : segs) {
    out.push_back('/');
    out += json_pointer_escape(seg);
  }
  return out;
}

std::string dumps_json(const Json& value) {
  if (value.is_null()) return "null";
  if (value.is_bool()) return value.as_bool() ? "true" : "false";
  if (value.is_number()) {
    double n = value.as_number();
    if (std::isfinite(n)) {
      // Prefer integer formatting when exact.
      double intpart;
      if (std::modf(n, &intpart) == 0.0) {
        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss.precision(0);
        oss << n;
        return oss.str();
      }
      std::ostringstream oss;
      oss.precision(15);
      oss << n;
      return oss.str();
    }
    return "null";
  }
  if (value.is_string()) return "\"" + json_escape(value.as_string()) + "\"";
  if (value.is_array()) {
    std::string out = "[";
    const auto& arr = value.as_array();
    for (size_t i = 0; i < arr.size(); ++i) {
      if (i) out += ",";
      out += dumps_json(arr[i]);
    }
    out += "]";
    return out;
  }
  const auto& obj = value.as_object();
  std::string out = "{";
  bool first = true;
  for (const auto& kv : obj) {
    if (!first) out += ",";
    first = false;
    out += "\"" + json_escape(kv.first) + "\":" + dumps_json(kv.second);
  }
  out += "}";
  return out;
}

// ---------------- JSON parser (tolerant pre-fix + strict-ish parse) ----------------

static std::string fix_smart_quotes(std::string s) {
  // Replace common Unicode “ ” ‘ ’ with ASCII quotes.
  auto rep = [&](const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
      s.replace(pos, from.size(), to);
      pos += to.size();
    }
  };
  rep("\xE2\x80\x9C", "\"");
  rep("\xE2\x80\x9D", "\"");
  rep("\xE2\x80\x98", "'");
  rep("\xE2\x80\x99", "'");
  return s;
}

static std::string strip_json_comments(const std::string& s) {
  // Removes //... and /*...*/ outside string literals.
  std::string out;
  out.reserve(s.size());

  bool in_str = false;
  char quote = 0;
  bool escape = false;
  bool in_line = false;
  bool in_block = false;

  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    char n = (i + 1 < s.size()) ? s[i + 1] : '\0';

    if (in_line) {
      if (c == '\n') {
        in_line = false;
        out.push_back(c);
      }
      continue;
    }
    if (in_block) {
      if (c == '*' && n == '/') {
        in_block = false;
        ++i;
      }
      continue;
    }

    if (in_str) {
      out.push_back(c);
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == quote) {
        in_str = false;
        quote = 0;
      }
      continue;
    }

    if (c == '"' || c == '\'') {
      in_str = true;
      quote = c;
      out.push_back(c);
      continue;
    }

    if (c == '/' && n == '/') {
      in_line = true;
      ++i;
      continue;
    }
    if (c == '/' && n == '*') {
      in_block = true;
      ++i;
      continue;
    }

    out.push_back(c);
  }
  return out;
}

static std::string replace_python_literals(const std::string& s) {
  // Best-effort: True/False/None -> true/false/null outside strings.
  std::string out;
  out.reserve(s.size());
  bool in_str = false;
  char quote = 0;
  bool escape = false;

  auto is_ident_char = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  };

  for (size_t i = 0; i < s.size();) {
    char c = s[i];
    if (in_str) {
      out.push_back(c);
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == quote) {
        in_str = false;
        quote = 0;
      }
      ++i;
      continue;
    }

    if (c == '"' || c == '\'') {
      in_str = true;
      quote = c;
      out.push_back(c);
      ++i;
      continue;
    }

    if ((c == 'T' || c == 'F' || c == 'N') && (i == 0 || !is_ident_char(s[i - 1]))) {
      if (s.compare(i, 4, "True") == 0 && (i + 4 >= s.size() || !is_ident_char(s[i + 4]))) {
        out += "true";
        i += 4;
        continue;
      }
      if (s.compare(i, 5, "False") == 0 && (i + 5 >= s.size() || !is_ident_char(s[i + 5]))) {
        out += "false";
        i += 5;
        continue;
      }
      if (s.compare(i, 4, "None") == 0 && (i + 4 >= s.size() || !is_ident_char(s[i + 4]))) {
        out += "null";
        i += 4;
        continue;
      }
    }

    out.push_back(c);
    ++i;
  }
  return out;
}

static std::string quote_unquoted_keys(const std::string& s) {
  // Best-effort: { foo: 1 } -> {"foo": 1} (outside strings)
  std::string out;
  out.reserve(s.size() + 8);

  bool in_str = false;
  char quote = 0;
  bool escape = false;

  auto is_ident_start = [](char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
  };
  auto is_ident = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  };

  for (size_t i = 0; i < s.size();) {
    char c = s[i];
    if (in_str) {
      out.push_back(c);
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == quote) {
        in_str = false;
        quote = 0;
      }
      ++i;
      continue;
    }

    if (c == '"' || c == '\'') {
      in_str = true;
      quote = c;
      out.push_back(c);
      ++i;
      continue;
    }

    if (is_ident_start(c)) {
      size_t start = i;
      size_t j = i;
      while (j < s.size() && is_ident(s[j])) ++j;
      size_t k = j;
      while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
      if (k < s.size() && s[k] == ':') {
        out.push_back('"');
        out.append(s, start, j - start);
        out.push_back('"');
        i = j;
        continue;
      }
    }

    out.push_back(c);
    ++i;
  }
  return out;
}

static std::optional<std::string> try_kv_object_to_json(const std::string& s) {
  // If the candidate looks like key=value lines (and not like JSON), convert to JSON object.
  if (s.find('{') != std::string::npos || s.find('[') != std::string::npos) return std::nullopt;
  if (s.find('=') == std::string::npos) return std::nullopt;

  JsonObject obj;
  auto lines = split_lines(s);
  std::regex kv_re(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*?)\s*$)");
  bool any = false;
  for (const auto& line : lines) {
    std::string t = ltrim_copy(line);
    if (t.empty() || t[0] == '#') continue;
    std::smatch m;
    if (!std::regex_match(line, m, kv_re)) return std::nullopt;
    any = true;
    std::string k = m[1].str();
    std::string v = m[2].str();

    std::string vv = v;
    if (vv.size() >= 2 && ((vv.front() == '"' && vv.back() == '"') || (vv.front() == '\'' && vv.back() == '\''))) {
      vv = vv.substr(1, vv.size() - 2);
      obj[k] = Json(vv);
      continue;
    }

    if (vv == "true") {
      obj[k] = Json(true);
    } else if (vv == "false") {
      obj[k] = Json(false);
    } else if (vv == "null") {
      obj[k] = Json(nullptr);
    } else {
      char* endp = nullptr;
      double n = std::strtod(vv.c_str(), &endp);
      if (endp && *endp == '\0') {
        obj[k] = Json(n);
      } else {
        obj[k] = Json(vv);
      }
    }
  }

  if (!any) return std::nullopt;
  return dumps_json(Json(obj));
}

static std::string drop_trailing_commas(const std::string& s) {
  // Remove commas immediately before } or ] (ignoring whitespace), while skipping string literals.
  std::string out;
  out.reserve(s.size());
  bool in_str = false;
  char quote = 0;
  bool escape = false;

  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (in_str) {
      out.push_back(c);
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == quote) {
        in_str = false;
        quote = 0;
      }
      continue;
    }

    if (c == '"' || c == '\'') {
      in_str = true;
      quote = c;
      out.push_back(c);
      continue;
    }

    if (c == ',') {
      size_t j = i + 1;
      while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) ++j;
      if (j < s.size() && (s[j] == '}' || s[j] == ']')) {
        continue;  // drop
      }
    }

    out.push_back(c);
  }
  return out;
}

struct Parser {
  const std::string& s;
  size_t i{0};
  bool allow_single_quotes{true};
  RepairConfig::DuplicateKeyPolicy duplicate_key_policy{RepairConfig::DuplicateKeyPolicy::FirstWins};
  int* duplicate_key_count{nullptr};

  explicit Parser(const std::string& in,
                  bool allow_single_quotes_,
                  RepairConfig::DuplicateKeyPolicy duplicate_key_policy_,
                  int* duplicate_key_count_)
      : s(in),
        allow_single_quotes(allow_single_quotes_),
        duplicate_key_policy(duplicate_key_policy_),
        duplicate_key_count(duplicate_key_count_) {}

  void skip_ws() {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  }

  [[noreturn]] void fail(const std::string& msg) const { throw std::runtime_error("JSON parse error: " + msg); }

  struct DuplicateKeyError {
    std::string key;
  };

  bool consume(char c) {
    skip_ws();
    if (i < s.size() && s[i] == c) {
      ++i;
      return true;
    }
    return false;
  }

  Json parse_value() {
    skip_ws();
    if (i >= s.size()) fail("unexpected end");
    char c = s[i];
    if (c == '{') return parse_object();
    if (c == '[') return parse_array();
    if (c == '"') return Json(parse_string());
    if (c == '\'') {
      if (!allow_single_quotes) fail("single-quoted strings are forbidden");
      return Json(parse_string());
    }
    if (c == 't') return parse_true();
    if (c == 'f') return parse_false();
    if (c == 'n') return parse_null();
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return Json(parse_number());
    fail(std::string("unexpected char '") + c + "'");
    return Json();
  }

  Json parse_object() {
    if (!consume('{')) fail("expected {");
    JsonObject obj;
    skip_ws();
    if (consume('}')) return Json(obj);
    while (true) {
      skip_ws();
      if (i >= s.size()) fail("unterminated object");
      if (!(s[i] == '"' || s[i] == '\'')) fail("expected string key");
      if (s[i] == '\'' && !allow_single_quotes) fail("single-quoted strings are forbidden");
      std::string key = parse_string();
      skip_ws();
      if (!consume(':')) fail("expected :");
      Json val = parse_value();

      auto it = obj.find(key);
      if (it != obj.end()) {
        if (duplicate_key_count) (*duplicate_key_count)++;
        if (duplicate_key_policy == RepairConfig::DuplicateKeyPolicy::Error) {
          throw DuplicateKeyError{key};
        }
        if (duplicate_key_policy == RepairConfig::DuplicateKeyPolicy::LastWins) {
          it->second = std::move(val);
        }
        // FirstWins: keep existing value.
      } else {
        obj.emplace(std::move(key), std::move(val));
      }
      skip_ws();
      if (consume('}')) break;
      if (!consume(',')) fail("expected , or }");
    }
    return Json(obj);
  }

  Json parse_array() {
    if (!consume('[')) fail("expected [");
    JsonArray arr;
    skip_ws();
    if (consume(']')) return Json(arr);
    while (true) {
      Json v = parse_value();
      arr.push_back(std::move(v));
      skip_ws();
      if (consume(']')) break;
      if (!consume(',')) fail("expected , or ]");
    }
    return Json(arr);
  }

  std::string parse_string() {
    skip_ws();
    if (i >= s.size()) fail("expected string");
    char q = s[i];
    if (q != '"' && q != '\'') fail("expected quote");
    if (q == '\'' && !allow_single_quotes) fail("single-quoted strings are forbidden");
    ++i;
    std::string out;
    while (i < s.size()) {
      char c = s[i++];
      if (c == q) return out;
      if (c == '\\') {
        if (i >= s.size()) fail("bad escape");
        char e = s[i++];
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          default:
            // Minimal: keep unknown escapes as-is.
            out.push_back(e);
        }
      } else {
        out.push_back(c);
      }
    }
    fail("unterminated string");
    return out;
  }

  double parse_number() {
    skip_ws();
    size_t start = i;
    if (s[i] == '-') ++i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    if (i < s.size() && s[i] == '.') {
      ++i;
      while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
      ++i;
      if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
      while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    }
    std::string num = s.substr(start, i - start);
    char* endp = nullptr;
    double v = std::strtod(num.c_str(), &endp);
    (void)endp;
    return v;
  }

  Json parse_true() {
    if (s.compare(i, 4, "true") == 0) {
      i += 4;
      return Json(true);
    }
    fail("expected true");
    return Json();
  }

  Json parse_false() {
    if (s.compare(i, 5, "false") == 0) {
      i += 5;
      return Json(false);
    }
    fail("expected false");
    return Json();
  }

  Json parse_null() {
    if (s.compare(i, 4, "null") == 0) {
      i += 4;
      return Json(nullptr);
    }
    fail("expected null");
    return Json();
  }
};

static Json parse_json_strictish(const std::string& fixed,
                                 bool allow_single_quotes,
                                 RepairConfig::DuplicateKeyPolicy duplicate_key_policy,
                                 int* duplicate_key_count) {
  Parser p(fixed, allow_single_quotes, duplicate_key_policy, duplicate_key_count);
  Json v = p.parse_value();
  p.skip_ws();
  if (p.i != fixed.size()) {
    throw std::runtime_error("JSON parse error: trailing data");
  }
  return v;
}

static std::optional<std::string> try_extract_json_candidate(const std::string& text) {
  // 1) fenced block ```json ... ```
  {
    auto lines = split_lines(text);
    bool in = false;
    std::ostringstream body;
    for (size_t idx = 0; idx < lines.size(); ++idx) {
      std::string line = ltrim_copy(lines[idx]);
      std::string low = to_lower(line);
      if (!in) {
        if (low.rfind("```json", 0) == 0) {
          in = true;
          body.str("");
          body.clear();
        }
      } else {
        if (low.rfind("```", 0) == 0) {
          std::string out = body.str();
          if (!out.empty() && out.back() == '\n') out.pop_back();
          return out;
        }
        body << lines[idx] << "\n";
      }
    }
    if (in) return std::nullopt;  // fence started but not closed yet
  }

  // 2) first balanced {...} or [...]
  auto scan_balanced = [&](char open, char close) -> std::optional<std::string> {
    bool in_str = false;
    char quote = 0;
    bool escape = false;
    int depth = 0;
    size_t start = std::string::npos;

    for (size_t idx = 0; idx < text.size(); ++idx) {
      char c = text[idx];
      if (in_str) {
        if (escape) {
          escape = false;
        } else if (c == '\\') {
          escape = true;
        } else if (c == quote) {
          in_str = false;
          quote = 0;
        }
        continue;
      }

      if (c == '"' || c == '\'') {
        in_str = true;
        quote = c;
        continue;
      }

      if (c == open) {
        if (depth == 0) start = idx;
        depth++;
      } else if (c == close && depth > 0) {
        depth--;
        if (depth == 0 && start != std::string::npos) {
          return text.substr(start, idx - start + 1);
        }
      }
    }

    if (start != std::string::npos) return std::nullopt;  // started but not closed yet
    return std::nullopt;
  };

  if (auto obj = scan_balanced('{', '}')) return obj;
  if (auto arr = scan_balanced('[', ']')) return arr;

  return std::nullopt;
}

struct JsonCandidateSpan {
  std::string candidate;
  size_t consume_end{0};  // number of bytes to consume from start of buffer
};

static std::optional<size_t> find_ci(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return 0;
  if (haystack.size() < needle.size()) return std::nullopt;
  for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    bool ok = true;
    for (size_t j = 0; j < needle.size(); ++j) {
      char a = static_cast<char>(std::tolower(static_cast<unsigned char>(haystack[i + j])));
      char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
      if (a != b) {
        ok = false;
        break;
      }
    }
    if (ok) return i;
  }
  return std::nullopt;
}

static std::optional<JsonCandidateSpan> try_extract_next_json_candidate_span(const std::string& text) {
  // 1) fenced ```json ... ``` (best-effort, case-insensitive)
  {
    auto start = find_ci(text, "```json");
    if (start) {
      size_t start_pos = *start;
      size_t body_start = text.find('\n', start_pos);
      if (body_start == std::string::npos) return std::nullopt;
      body_start += 1;
      size_t end_pos = text.find("```", body_start);
      if (end_pos == std::string::npos) return std::nullopt;
      std::string body = text.substr(body_start, end_pos - body_start);
      // trim one trailing newline to match extract_json_candidate behavior
      if (!body.empty() && body.back() == '\n') body.pop_back();
      return JsonCandidateSpan{body, end_pos + 3};
    }
  }

  // 2) first balanced {...} or [...] (choose earliest)
  auto scan_balanced_span = [&](char open, char close) -> std::optional<std::pair<size_t, size_t>> {
    bool in_str = false;
    char quote = 0;
    bool escape = false;
    int depth = 0;
    size_t start = std::string::npos;

    for (size_t idx = 0; idx < text.size(); ++idx) {
      char c = text[idx];
      if (in_str) {
        if (escape) {
          escape = false;
        } else if (c == '\\') {
          escape = true;
        } else if (c == quote) {
          in_str = false;
          quote = 0;
        }
        continue;
      }

      if (c == '"' || c == '\'') {
        in_str = true;
        quote = c;
        continue;
      }

      if (c == open) {
        if (depth == 0) start = idx;
        depth++;
      } else if (c == close && depth > 0) {
        depth--;
        if (depth == 0 && start != std::string::npos) {
          return std::make_pair(start, idx);
        }
      }
    }

    if (start != std::string::npos) return std::nullopt;  // started but not closed yet
    return std::nullopt;
  };

  std::optional<std::pair<size_t, size_t>> obj = scan_balanced_span('{', '}');
  std::optional<std::pair<size_t, size_t>> arr = scan_balanced_span('[', ']');

  std::optional<std::pair<size_t, size_t>> best;
  if (obj) best = obj;
  if (arr) {
    if (!best || arr->first < best->first) best = arr;
  }
  if (best) {
    size_t start = best->first;
    size_t end = best->second;
    return JsonCandidateSpan{text.substr(start, end - start + 1), end + 1};
  }
  return std::nullopt;
}

static std::optional<std::string> pop_next_json_candidate(std::string& buf) {
  auto span = try_extract_next_json_candidate_span(buf);
  if (!span) return std::nullopt;
  std::string cand = span->candidate;
  buf.erase(0, span->consume_end);
  return cand;
}

std::string extract_json_candidate(const std::string& text) {
  // 1) fenced block ```json ... ``` (scan lines; MSVC std::regex doesn't support (?is) flags)
  {
    auto lines = split_lines(text);
    bool in = false;
    std::ostringstream body;
    for (size_t idx = 0; idx < lines.size(); ++idx) {
      std::string line = ltrim_copy(lines[idx]);
      std::string low = to_lower(line);
      if (!in) {
        if (low.rfind("```json", 0) == 0) {
          in = true;
          body.str("");
          body.clear();
        }
      } else {
        if (low.rfind("```", 0) == 0) {
          std::string out = body.str();
          if (!out.empty() && out.back() == '\n') out.pop_back();
          return out;
        }
        body << lines[idx] << "\n";
      }
    }
  }

  // 2) first balanced {...} or [...]
  auto scan_balanced = [&](char open, char close) -> std::optional<std::string> {
    bool in_str = false;
    char quote = 0;
    bool escape = false;
    int depth = 0;
    size_t start = std::string::npos;

    for (size_t idx = 0; idx < text.size(); ++idx) {
      char c = text[idx];
      if (in_str) {
        if (escape) {
          escape = false;
        } else if (c == '\\') {
          escape = true;
        } else if (c == quote) {
          in_str = false;
          quote = 0;
        }
        continue;
      }

      if (c == '"' || c == '\'') {
        in_str = true;
        quote = c;
        continue;
      }

      if (c == open) {
        if (depth == 0) start = idx;
        depth++;
      } else if (c == close && depth > 0) {
        depth--;
        if (depth == 0 && start != std::string::npos) {
          return text.substr(start, idx - start + 1);
        }
      }
    }
    return std::nullopt;
  };

  if (auto obj = scan_balanced('{', '}')) return *obj;
  if (auto arr = scan_balanced('[', ']')) return *arr;

  throw std::runtime_error("no JSON found");
}

static std::pair<std::string, bool> extract_json_candidate_with_meta(const std::string& text) {
  // 1) fenced block ```json ... ```
  {
    auto lines = split_lines(text);
    bool in = false;
    std::ostringstream body;
    for (size_t idx = 0; idx < lines.size(); ++idx) {
      std::string line = ltrim_copy(lines[idx]);
      std::string low = to_lower(line);
      if (!in) {
        if (low.rfind("```json", 0) == 0) {
          in = true;
          body.str("");
          body.clear();
        }
      } else {
        if (low.rfind("```", 0) == 0) {
          std::string out = body.str();
          if (!out.empty() && out.back() == '\n') out.pop_back();
          return {out, true};
        }
        body << lines[idx] << "\n";
      }
    }
  }

  // 2) first balanced {...} or [...]
  auto scan_balanced = [&](char open, char close) -> std::optional<std::string> {
    bool in_str = false;
    char quote = 0;
    bool escape = false;
    int depth = 0;
    size_t start = std::string::npos;

    for (size_t idx = 0; idx < text.size(); ++idx) {
      char c = text[idx];
      if (in_str) {
        if (escape) {
          escape = false;
        } else if (c == '\\') {
          escape = true;
        } else if (c == quote) {
          in_str = false;
          quote = 0;
        }
        continue;
      }

      if (c == '"' || c == '\'') {
        in_str = true;
        quote = c;
        continue;
      }

      if (c == open) {
        if (depth == 0) start = idx;
        depth++;
      } else if (c == close && depth > 0) {
        depth--;
        if (depth == 0 && start != std::string::npos) {
          return text.substr(start, idx - start + 1);
        }
      }
    }
    return std::nullopt;
  };

  if (auto obj = scan_balanced('{', '}')) return {*obj, false};
  if (auto arr = scan_balanced('[', ']')) return {*arr, false};

  // 3) Fallback for top-level JSON primitives or incomplete JSON.
  // If the input starts with a JSON token (after whitespace), treat the remainder as the candidate.
  // This allows e.g. '"a@b.com"' or an incomplete '{"a": 1' to report a parse error rather than "no JSON found".
  {
    std::string trimmed = ltrim_copy(text);
    if (!trimmed.empty()) {
      const char c0 = trimmed[0];
      const bool looks_like_json_value =
          (c0 == '{' || c0 == '[' || c0 == '"' || c0 == '\'' || c0 == '-' || std::isdigit(static_cast<unsigned char>(c0)) ||
           c0 == 't' || c0 == 'f' || c0 == 'n');
      if (looks_like_json_value) return {trimmed, false};
    }
  }

  throw std::runtime_error("no JSON found");
}

struct TextRange {
  size_t start{0};
  size_t end{0};  // exclusive
};

struct CandidateWithMeta {
  size_t start{0};
  std::string text;
  bool from_fence{false};
};

static bool RangeContains(const TextRange& r, size_t idx) { return idx >= r.start && idx < r.end; }

static std::vector<CandidateWithMeta> extract_json_candidates_with_meta_all(const std::string& text) {
  std::vector<CandidateWithMeta> out;
  std::vector<TextRange> fenced_ranges;

  // 1) all fenced blocks ```json ... ```
  {
    bool in = false;
    size_t fence_start = 0;
    size_t body_start = 0;

    size_t pos = 0;
    while (pos <= text.size()) {
      const size_t line_start = pos;
      size_t line_end = text.find('\n', pos);
      if (line_end == std::string::npos) line_end = text.size();
      const std::string line_raw = text.substr(line_start, line_end - line_start);
      const std::string line = ltrim_copy(line_raw);
      const std::string low = to_lower(line);

      if (!in) {
        if (low.rfind("```json", 0) == 0) {
          in = true;
          fence_start = line_start;
          body_start = (line_end < text.size()) ? (line_end + 1) : text.size();
        }
      } else {
        if (low.rfind("```", 0) == 0) {
          const size_t body_end = line_start;
          std::string body = text.substr(body_start, body_end - body_start);
          // match extract_json_candidate behavior: trim one trailing newline
          if (!body.empty() && body.back() == '\n') body.pop_back();
          out.push_back(CandidateWithMeta{body_start, std::move(body), true});

          const size_t fence_end = (line_end < text.size()) ? (line_end + 1) : line_end;
          fenced_ranges.push_back(TextRange{fence_start, fence_end});
          in = false;
        }
      }

      if (line_end >= text.size()) break;
      pos = line_end + 1;
    }
  }

  // sort ranges so we can skip them efficiently
  std::sort(fenced_ranges.begin(), fenced_ranges.end(), [](const TextRange& a, const TextRange& b) { return a.start < b.start; });

  auto skip_fenced = [&](size_t& idx) -> bool {
    // linear scan is fine (few fences typical)
    for (const auto& r : fenced_ranges) {
      if (RangeContains(r, idx)) {
        idx = (r.end > 0) ? (r.end - 1) : idx;
        return true;
      }
    }
    return false;
  };

  auto extract_balanced_at = [&](size_t start, std::string& cand, size_t& end_out) -> bool {
    if (start >= text.size()) return false;
    const char open = text[start];
    const char close = (open == '{') ? '}' : ']';

    bool in_str = false;
    char quote = 0;
    bool escape = false;
    int depth = 0;

    for (size_t i = start; i < text.size(); ++i) {
      char c = text[i];
      if (in_str) {
        if (escape) {
          escape = false;
        } else if (c == '\\') {
          escape = true;
        } else if (c == quote) {
          in_str = false;
          quote = 0;
        }
        continue;
      }

      if (c == '"' || c == '\'') {
        in_str = true;
        quote = c;
        continue;
      }

      if (c == open) {
        depth++;
      } else if (c == close) {
        if (depth > 0) depth--;
        if (depth == 0) {
          end_out = i + 1;
          cand = text.substr(start, end_out - start);
          return true;
        }
      }
    }
    return false;
  };

  // 2) all balanced {...} / [...] outside fenced regions
  {
    bool in_str = false;
    char quote = 0;
    bool escape = false;

    for (size_t idx = 0; idx < text.size(); ++idx) {
      if (skip_fenced(idx)) continue;

      char c = text[idx];
      if (in_str) {
        if (escape) {
          escape = false;
        } else if (c == '\\') {
          escape = true;
        } else if (c == quote) {
          in_str = false;
          quote = 0;
        }
        continue;
      }

      if (c == '"' || c == '\'') {
        in_str = true;
        quote = c;
        continue;
      }

      if (c == '{' || c == '[') {
        std::string cand;
        size_t end = 0;
        if (extract_balanced_at(idx, cand, end)) {
          out.push_back(CandidateWithMeta{idx, std::move(cand), false});
          idx = end - 1;
        }
      }
    }
  }

  if (out.empty()) throw std::runtime_error("no JSON found");

  std::sort(out.begin(), out.end(), [](const CandidateWithMeta& a, const CandidateWithMeta& b) {
    if (a.start != b.start) return a.start < b.start;
    // stable-ish tie-breaker
    if (a.from_fence != b.from_fence) return a.from_fence;
    return a.text.size() < b.text.size();
  });
  return out;
}

std::vector<std::string> extract_json_candidates(const std::string& text) {
  auto all = extract_json_candidates_with_meta_all(text);
  std::vector<std::string> out;
  out.reserve(all.size());
  for (auto& it : all) out.push_back(std::move(it.text));
  return out;
}

static JsonishParseResult loads_jsonish_candidate_ex(const std::string& candidate, bool from_fence, const RepairConfig& repair) {
  RepairMetadata meta;
  meta.extracted_from_fence = from_fence;

  std::string fixed = candidate;

  if (repair.fix_smart_quotes) {
    std::string before = fixed;
    fixed = fix_smart_quotes(fixed);
    meta.fixed_smart_quotes = (fixed != before);
  }

  if (repair.strip_json_comments) {
    std::string before = fixed;
    fixed = strip_json_comments(fixed);
    meta.stripped_comments = (fixed != before);
  }

  if (repair.replace_python_literals) {
    std::string before = fixed;
    fixed = replace_python_literals(fixed);
    meta.replaced_python_literals = (fixed != before);
  }

  if (repair.convert_kv_object_to_json) {
    if (auto converted = try_kv_object_to_json(fixed)) {
      meta.converted_kv_object = true;
      fixed = *converted;
    }
  }

  if (repair.quote_unquoted_keys) {
    std::string before = fixed;
    fixed = quote_unquoted_keys(fixed);
    meta.quoted_unquoted_keys = (fixed != before);
  }

  if (repair.drop_trailing_commas) {
    std::string before = fixed;
    fixed = drop_trailing_commas(fixed);
    meta.dropped_trailing_commas = (fixed != before);
  }

  try {
    int dup_count = 0;
    Json v = parse_json_strictish(fixed, repair.allow_single_quotes, repair.duplicate_key_policy, &dup_count);
    meta.duplicateKeyCount = dup_count;
    meta.duplicateKeyPolicy = repair.duplicate_key_policy;
    return JsonishParseResult{std::move(v), std::move(fixed), meta};
  } catch (const Parser::DuplicateKeyError& e) {
    meta.duplicateKeyCount = std::max(meta.duplicateKeyCount, 1);
    meta.duplicateKeyPolicy = repair.duplicate_key_policy;
    throw ValidationError("duplicate key", "$." + e.key, "parse");
  } catch (const std::exception& e) {
    throw ValidationError(e.what(), "$", "parse");
  }
}

JsonishParseResult loads_jsonish_ex(const std::string& text, const RepairConfig& repair) {
  auto [candidate, from_fence] = extract_json_candidate_with_meta(text);
  return loads_jsonish_candidate_ex(candidate, from_fence, repair);
}

Json loads_jsonish(const std::string& text) {
  return loads_jsonish_ex(text, RepairConfig{}).value;
}

JsonArray loads_jsonish_all(const std::string& text) {
  return loads_jsonish_all_ex(text, RepairConfig{}).values;
}

JsonishParseAllResult loads_jsonish_all_ex(const std::string& text, const RepairConfig& repair) {
  auto all = extract_json_candidates_with_meta_all(text);
  JsonishParseAllResult out;
  out.values.reserve(all.size());
  out.fixed.reserve(all.size());
  out.metadata.reserve(all.size());

  for (const auto& it : all) {
    auto r = loads_jsonish_candidate_ex(it.text, it.from_fence, repair);
    out.values.push_back(std::move(r.value));
    out.fixed.push_back(std::move(r.fixed));
    out.metadata.push_back(std::move(r.metadata));
  }
  return out;
}

// ---------------- JSON schema validation (subset) ----------------

static std::optional<std::string> get_string_field(const JsonObject& obj, const std::string& key) {
  auto it = obj.find(key);
  if (it == obj.end()) return std::nullopt;
  if (!it->second.is_string()) return std::nullopt;
  return it->second.as_string();
}

static std::optional<double> get_number_field(const JsonObject& obj, const std::string& key) {
  auto it = obj.find(key);
  if (it == obj.end()) return std::nullopt;
  if (!it->second.is_number()) return std::nullopt;
  return it->second.as_number();
}

static const JsonObject& require_object_schema(const Json& schema, const std::string& path) {
  if (!schema.is_object()) throw ValidationError("schema must be object", path);
  return schema.as_object();
}

static bool json_equals(const Json& a, const Json& b) {
  // For our use (enum), compare dumps.
  return dumps_json(a) == dumps_json(b);
}

struct ValidateOptions {
  bool collect_all{false};
  std::vector<ValidationError>* errors{nullptr};
};

static void validate_impl(const Json& value, const Json& schema, const std::string& path, const ValidateOptions& opt);

static bool report_or_throw(
    const ValidateOptions& opt, const std::string& message, const std::string& path, const std::string& kind = "schema") {
  if (opt.collect_all && opt.errors) {
    opt.errors->emplace_back(message, path, kind);
    return false;
  }
  throw ValidationError(message, path, kind);
}

static bool schema_passes(const Json& value, const Json& schema, const std::string& path) {
  try {
    ValidateOptions opt;
    validate_impl(value, schema, path, opt);
    return true;
  } catch (const ValidationError&) {
    return false;
  }
}

static void validate_impl(const Json& value, const Json& schema, const std::string& path, const ValidateOptions& opt) {
  const auto& sch = require_object_schema(schema, path);

  // allOf / anyOf / oneOf
  {
    auto it_all = sch.find("allOf");
    if (it_all != sch.end() && it_all->second.is_array()) {
      for (const auto& sub : it_all->second.as_array()) {
        if (!sub.is_object()) continue;
        validate_impl(value, sub, path, opt);
      }
    }

    auto it_any = sch.find("anyOf");
    if (it_any != sch.end() && it_any->second.is_array()) {
      bool ok = false;
      for (const auto& sub : it_any->second.as_array()) {
        if (!sub.is_object()) continue;
        if (schema_passes(value, sub, path)) {
          ok = true;
          break;
        }
      }
      if (!ok) {
        if (!report_or_throw(opt, "does not match anyOf", path)) return;
      }
    }

    auto it_one = sch.find("oneOf");
    if (it_one != sch.end() && it_one->second.is_array()) {
      int ok_count = 0;
      for (const auto& sub : it_one->second.as_array()) {
        if (!sub.is_object()) continue;
        if (schema_passes(value, sub, path)) ok_count++;
      }
      if (ok_count != 1) {
        if (!report_or_throw(opt, "does not match oneOf", path)) return;
      }
    }
  }

  // const
  {
    auto it = sch.find("const");
    if (it != sch.end()) {
      if (!json_equals(value, it->second)) {
        if (!report_or_throw(opt, "value does not match const", path)) return;
      }
    }
  }

  // enum
  {
    auto it = sch.find("enum");
    if (it != sch.end() && it->second.is_array()) {
      const auto& arr = it->second.as_array();
      bool ok = false;
      for (const auto& v : arr) {
        if (json_equals(value, v)) {
          ok = true;
          break;
        }
      }
      if (!ok) {
        if (!report_or_throw(opt, "value not in enum", path)) return;
      }
    }
  }

  // type
  std::optional<std::string> ty;
  if (auto t = get_string_field(sch, "type")) ty = to_lower(*t);

  auto type_mismatch = [&](const std::string& expected) {
    report_or_throw(opt, "expected " + expected, path, "type");
  };

  if (ty) {
    if (*ty == "null" && !value.is_null()) type_mismatch("null");
    if (*ty == "boolean" && !value.is_bool()) type_mismatch("boolean");
    if ((*ty == "number" || *ty == "integer") && !value.is_number()) type_mismatch("number");
    if (*ty == "string" && !value.is_string()) type_mismatch("string");
    if (*ty == "array" && !value.is_array()) type_mismatch("array");
    if (*ty == "object" && !value.is_object()) type_mismatch("object");
    if (*ty == "integer" && value.is_number()) {
      double n = value.as_number();
      if (!std::isfinite(n)) type_mismatch("integer");
      double ip;
      double frac = std::modf(n, &ip);
      if (std::fabs(frac) > 1e-12) type_mismatch("integer");
    }
  }

  // numeric constraints
  if (value.is_number()) {
    if (auto mn = get_number_field(sch, "minimum")) {
      if (value.as_number() < *mn) {
        if (!report_or_throw(opt, "number < minimum", path)) return;
      }
    }
    if (auto mx = get_number_field(sch, "maximum")) {
      if (value.as_number() > *mx) {
        if (!report_or_throw(opt, "number > maximum", path)) return;
      }
    }

    if (auto mul = get_number_field(sch, "multipleOf")) {
      double m = *mul;
      if (m > 0.0) {
        double n = value.as_number();
        double q = n / m;
        double rq = std::round(q);
        if (!std::isfinite(q) || std::fabs(q - rq) > 1e-9) {
          if (!report_or_throw(opt, "number is not a multipleOf", path)) return;
        }
      }
    }
  }

  // string constraints (+ pattern)
  if (value.is_string()) {
    const auto& s = value.as_string();
    if (auto mn = get_number_field(sch, "minLength")) {
      if (static_cast<double>(s.size()) < *mn) {
        if (!report_or_throw(opt, "string shorter than minLength", path)) return;
      }
    }
    if (auto mx = get_number_field(sch, "maxLength")) {
      if (static_cast<double>(s.size()) > *mx) {
        if (!report_or_throw(opt, "string longer than maxLength", path)) return;
      }
    }

    auto it_pat = sch.find("pattern");
    if (it_pat != sch.end() && it_pat->second.is_string()) {
      try {
        std::regex r(it_pat->second.as_string(), std::regex::ECMAScript);
        if (!std::regex_search(s, r)) {
          if (!report_or_throw(opt, "string does not match pattern", path)) return;
        }
      } catch (const std::regex_error&) {
        if (!report_or_throw(opt, "invalid pattern regex", path)) return;
      }
    }

    // format (email | uuid | date-time)
    auto it_fmt = sch.find("format");
    if (it_fmt != sch.end() && it_fmt->second.is_string()) {
      const std::string fmt = to_lower(it_fmt->second.as_string());
      try {
        if (fmt == "email") {
          std::regex r(R"(^[^\s@]+@[^\s@]+\.[^\s@]+$)", std::regex::ECMAScript);
          if (!std::regex_match(s, r)) {
            if (!report_or_throw(opt, "string does not match email format", path)) return;
          }
        } else if (fmt == "uuid") {
          std::regex r(R"(^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$)",
                       std::regex::ECMAScript);
          if (!std::regex_match(s, r)) {
            if (!report_or_throw(opt, "string does not match uuid format", path)) return;
          }
        } else if (fmt == "date-time") {
          std::regex r(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?(Z|[+-]\d{2}:\d{2})$)",
                       std::regex::ECMAScript);
          if (!std::regex_match(s, r)) {
            if (!report_or_throw(opt, "string does not match date-time format", path)) return;
          }
        }
      } catch (const std::regex_error&) {
        if (!report_or_throw(opt, "invalid format regex", path)) return;
      }
    }
  }

  // array constraints
  if (value.is_array()) {
    const auto& arr = value.as_array();
    if (auto mn = get_number_field(sch, "minItems")) {
      if (static_cast<double>(arr.size()) < *mn) {
        if (!report_or_throw(opt, "array shorter than minItems", path)) return;
      }
    }
    if (auto mx = get_number_field(sch, "maxItems")) {
      if (static_cast<double>(arr.size()) > *mx) {
        if (!report_or_throw(opt, "array longer than maxItems", path)) return;
      }
    }

    auto it_items = sch.find("items");
    if (it_items != sch.end() && it_items->second.is_object()) {
      for (size_t idx = 0; idx < arr.size(); ++idx) {
        validate_impl(arr[idx], it_items->second, path + "[" + std::to_string(idx) + "]", opt);
      }
    }

    // contains (+ minContains/maxContains)
    auto it_contains = sch.find("contains");
    if (it_contains != sch.end() && it_contains->second.is_object()) {
      size_t count = 0;
      for (size_t idx = 0; idx < arr.size(); ++idx) {
        if (schema_passes(arr[idx], it_contains->second, path + "[" + std::to_string(idx) + "]")) {
          ++count;
        }
      }
      size_t minC = 1;
      if (auto mn = get_number_field(sch, "minContains")) {
        if (*mn >= 0.0) minC = static_cast<size_t>(*mn);
      }
      std::optional<size_t> maxC;
      if (auto mx = get_number_field(sch, "maxContains")) {
        if (*mx >= 0.0) maxC = static_cast<size_t>(*mx);
      }
      if (count < minC) {
        if (!report_or_throw(opt, "array does not satisfy contains/minContains", path)) return;
      }
      if (maxC && count > *maxC) {
        if (!report_or_throw(opt, "array exceeds maxContains", path)) return;
      }
    }
  }

  // object constraints
  if (value.is_object()) {
    const auto& obj = value.as_object();

    if (auto mn = get_number_field(sch, "minProperties")) {
      if (static_cast<double>(obj.size()) < *mn) {
        if (!report_or_throw(opt, "object has fewer properties than minProperties", path)) return;
      }
    }
    if (auto mx = get_number_field(sch, "maxProperties")) {
      if (static_cast<double>(obj.size()) > *mx) {
        if (!report_or_throw(opt, "object has more properties than maxProperties", path)) return;
      }
    }

    // required
    auto it_req = sch.find("required");
    if (it_req != sch.end() && it_req->second.is_array()) {
      for (const auto& k : it_req->second.as_array()) {
        if (!k.is_string()) continue;
        if (obj.find(k.as_string()) == obj.end()) {
          report_or_throw(opt, "missing required property: " + k.as_string(), path + "." + k.as_string());
        }
      }
    }

    // dependentRequired
    {
      auto it_dep = sch.find("dependentRequired");
      if (it_dep != sch.end() && it_dep->second.is_object()) {
        const auto& deps = it_dep->second.as_object();
        for (const auto& dep : deps) {
          const std::string& prop = dep.first;
          if (obj.find(prop) == obj.end()) continue;
          if (!dep.second.is_array()) continue;
          for (const auto& req : dep.second.as_array()) {
            if (!req.is_string()) continue;
            const std::string& rk = req.as_string();
            if (obj.find(rk) == obj.end()) {
              report_or_throw(opt, "missing dependentRequired property: " + rk + " (requires because " + prop + " is present)",
                              path + "." + rk);
            }
          }
        }
      }
    }

    // propertyNames
    {
      auto it_pn = sch.find("propertyNames");
      if (it_pn != sch.end() && it_pn->second.is_object()) {
        for (const auto& kv : obj) {
          Json keyv(kv.first);
          if (!schema_passes(keyv, it_pn->second, path + ".<propertyNames>")) {
            if (!report_or_throw(opt, "property name does not satisfy propertyNames: " + kv.first, path + ".<propertyNames>")) return;
          }
        }
      }
    }

    // properties
    const JsonObject* props = nullptr;
    auto it_props = sch.find("properties");
    if (it_props != sch.end() && it_props->second.is_object()) props = &it_props->second.as_object();

    // additionalProperties
    enum class APMode { Allow, Forbid, Schema };
    APMode ap = APMode::Allow;
    const Json* ap_schema = nullptr;
    auto it_ap = sch.find("additionalProperties");
    if (it_ap != sch.end()) {
      if (it_ap->second.is_bool()) {
        ap = it_ap->second.as_bool() ? APMode::Allow : APMode::Forbid;
      } else if (it_ap->second.is_object()) {
        ap = APMode::Schema;
        ap_schema = &it_ap->second;
      }
    }

    for (const auto& kv : obj) {
      const std::string& key = kv.first;
      const Json& val = kv.second;
      if (props && props->find(key) != props->end()) {
        validate_impl(val, props->at(key), path + "." + key, opt);
      } else {
        if (ap == APMode::Forbid) {
          report_or_throw(opt, "additionalProperties forbidden: " + key, path + "." + key);
        }
        if (ap == APMode::Schema && ap_schema) {
          validate_impl(val, *ap_schema, path + "." + key, opt);
        }
      }
    }
  }

  // if / then / else (applies to any type)
  {
    auto it_if = sch.find("if");
    if (it_if != sch.end() && it_if->second.is_object()) {
      bool cond = schema_passes(value, it_if->second, path);
      auto it_then = sch.find("then");
      auto it_else = sch.find("else");
      if (cond) {
        if (it_then != sch.end() && it_then->second.is_object()) {
          validate_impl(value, it_then->second, path, opt);
        }
      } else {
        if (it_else != sch.end() && it_else->second.is_object()) {
          validate_impl(value, it_else->second, path, opt);
        }
      }
    }
  }
}

void validate(const Json& value, const Json& schema, const std::string& path) {
  ValidateOptions opt;
  validate_impl(value, schema, path, opt);
}

std::vector<ValidationError> validate_all(const Json& value, const Json& schema, const std::string& path) {
  std::vector<ValidationError> errors;
  ValidateOptions opt;
  opt.collect_all = true;
  opt.errors = &errors;
  validate_impl(value, schema, path, opt);
  return errors;
}

static void apply_defaults(Json& value, const Json& schema) {
  if (!schema.is_object()) return;
  const auto& sch = schema.as_object();

  if (value.is_object()) {
    auto it_props = sch.find("properties");
    if (it_props != sch.end() && it_props->second.is_object()) {
      const auto& props = it_props->second.as_object();
      for (const auto& kv : props) {
        const std::string& key = kv.first;
        const Json& prop_schema = kv.second;
        if (!prop_schema.is_object()) continue;

        auto& obj = value.as_object();
        if (obj.find(key) == obj.end()) {
          auto it_def = prop_schema.as_object().find("default");
          if (it_def != prop_schema.as_object().end()) {
            obj[key] = it_def->second;
          }
        }

        auto it2 = obj.find(key);
        if (it2 != obj.end()) {
          apply_defaults(it2->second, prop_schema);
        }
      }
    }
  }

  if (value.is_array()) {
    auto it_items = sch.find("items");
    if (it_items != sch.end() && it_items->second.is_object()) {
      for (auto& elem : value.as_array()) {
        apply_defaults(elem, it_items->second);
      }
    }
  }
}

Json parse_and_validate(const std::string& text, const Json& schema) {
  Json v = loads_jsonish(text);
  validate(v, schema, "$");
  return v;
}

JsonishParseResult parse_and_validate_ex(const std::string& text, const Json& schema, const RepairConfig& repair) {
  JsonishParseResult r = loads_jsonish_ex(text, repair);
  validate(r.value, schema, "$");
  return r;
}

JsonArray parse_and_validate_all(const std::string& text, const Json& schema) {
  return parse_and_validate_all_ex(text, schema, RepairConfig{}).values;
}

JsonishParseAllResult parse_and_validate_all_ex(const std::string& text, const Json& schema, const RepairConfig& repair) {
  JsonishParseAllResult r = loads_jsonish_all_ex(text, repair);
  for (size_t i = 0; i < r.values.size(); ++i) {
    validate(r.values[i], schema, "$[" + std::to_string(i) + "]");
  }
  return r;
}

Json parse_and_validate_with_defaults(const std::string& text, const Json& schema) {
  Json v = loads_jsonish(text);
  apply_defaults(v, schema);
  validate(v, schema, "$");
  return v;
}

JsonishParseResult parse_and_validate_with_defaults_ex(
    const std::string& text, const Json& schema, const RepairConfig& repair) {
  JsonishParseResult r = loads_jsonish_ex(text, repair);
  apply_defaults(r.value, schema);
  validate(r.value, schema, "$");
  return r;
}

// ---------------- Markdown parsing/validation ----------------

MarkdownParsed parse_markdown(const std::string& text) {
  MarkdownParsed out;
  out.text = text;
  out.lines = split_lines(text);

  bool in_code = false;
  std::string fence_lang;
  std::ostringstream fence_body;

  std::string current_section;
  for (size_t idx = 0; idx < out.lines.size(); ++idx) {
    const int lineNo = static_cast<int>(idx) + 1;
    const std::string& line = out.lines[idx];

    // code fences
    {
      std::smatch m;
      std::regex fence(R"(^\s*```\s*([A-Za-z0-9_-]+)?\s*$)");
      if (std::regex_match(line, m, fence)) {
        if (!in_code) {
          in_code = true;
          fence_lang = m.size() >= 2 ? m[1].str() : "";
          fence_body.str("");
          fence_body.clear();
        } else {
          in_code = false;
          MarkdownCodeBlock cb;
          cb.lang = to_lower(fence_lang);
          cb.body = fence_body.str();
          // trim trailing newline
          if (!cb.body.empty() && cb.body.back() == '\n') cb.body.pop_back();
          out.codeBlocks.push_back(std::move(cb));
          fence_lang.clear();
        }
        continue;
      }
    }

    if (in_code) {
      fence_body << line << "\n";
      continue;
    }

    // heading
    {
      std::smatch m;
      std::regex h(R"(^\s*(#{1,6})\s+(.*?)\s*$)");
      if (std::regex_match(line, m, h)) {
        MarkdownHeading hh;
        hh.level = static_cast<int>(m[1].str().size());
        hh.title = m[2].str();
        hh.line = lineNo;
        out.headings.push_back(hh);
        current_section = hh.title;
        out.sections[current_section];
        continue;
      }
    }

    // bullets / tasks
    {
      std::regex bullet(R"(^\s*[-*+]\s+.+$)");
      if (std::regex_match(line, bullet)) {
        out.bulletLineNumbers.push_back(lineNo);
      }
      std::regex task(R"(^\s*[-*+]\s+\[( |x|X)\]\s+.+$)");
      if (std::regex_match(line, task)) {
        out.taskLineNumbers.push_back(lineNo);
      }
    }

    // tables (very lightweight): line with | and next line is --- separator
    if (line.find('|') != std::string::npos && idx + 1 < out.lines.size()) {
      const std::string& next = out.lines[idx + 1];
      if (next.find('|') != std::string::npos && std::regex_search(next, std::regex(R"(^\s*\|?\s*[-:]+)", std::regex::icase))) {
        std::ostringstream raw;
        raw << line << "\n" << next;
        out.tables.push_back(MarkdownTable{lineNo, raw.str()});
      }
    }

    // section content
    if (!current_section.empty()) {
      out.sections[current_section].push_back(line);
    }
  }

  return out;
}

static bool json_bool(const JsonObject& o, const std::string& key, bool def) {
  auto it = o.find(key);
  if (it == o.end()) return def;
  if (!it->second.is_bool()) return def;
  return it->second.as_bool();
}

static std::optional<double> json_num_opt(const JsonObject& o, const std::string& key) {
  auto it = o.find(key);
  if (it == o.end()) return std::nullopt;
  if (!it->second.is_number()) return std::nullopt;
  return it->second.as_number();
}

static std::vector<std::string> json_string_list(const JsonObject& o, const std::string& key) {
  std::vector<std::string> out;
  auto it = o.find(key);
  if (it == o.end() || !it->second.is_array()) return out;
  for (const auto& v : it->second.as_array()) {
    if (v.is_string()) out.push_back(v.as_string());
  }
  return out;
}

void validate_markdown(const MarkdownParsed& parsed, const Json& schema) {
  const auto& sch = require_object_schema(schema, "$");

  // forbidHtml
  if (json_bool(sch, "forbidHtml", false)) {
    if (parsed.text.find('<') != std::string::npos) {
      throw ValidationError("HTML appears in markdown", "$.html");
    }
  }

  // maxLineLength
  if (auto mx = json_num_opt(sch, "maxLineLength")) {
    for (size_t i = 0; i < parsed.lines.size(); ++i) {
      if (static_cast<double>(parsed.lines[i].size()) > *mx) {
        throw ValidationError("line too long", "$.lines[" + std::to_string(i + 1) + "]");
      }
    }
  }

  // headings
  {
    auto required = json_string_list(sch, "requiredHeadings");
    for (const auto& h : required) {
      bool found = false;
      for (const auto& hh : parsed.headings) {
        if (hh.title == h) {
          found = true;
          break;
        }
      }
      if (!found) throw ValidationError("missing required heading: " + h, "$.headings[" + h + "]");
    }
  }

  // code blocks count / fences
  {
    if (auto mn = json_num_opt(sch, "minCodeBlocks")) {
      if (static_cast<double>(parsed.codeBlocks.size()) < *mn) throw ValidationError("too few code blocks", "$.codeBlocks");
    }
    if (auto mx = json_num_opt(sch, "maxCodeBlocks")) {
      if (static_cast<double>(parsed.codeBlocks.size()) > *mx) throw ValidationError("too many code blocks", "$.codeBlocks");
    }

    auto required_fences = json_string_list(sch, "requiredCodeFences");
    for (const auto& lang : required_fences) {
      bool found = false;
      for (const auto& cb : parsed.codeBlocks) {
        if (to_lower(cb.lang) == to_lower(lang)) {
          found = true;
          break;
        }
      }
      if (!found) throw ValidationError("missing required code fence: " + lang, "$.codeFences[" + lang + "]");
    }
  }

  // tables
  if (auto mn = json_num_opt(sch, "minTables")) {
    if (static_cast<double>(parsed.tables.size()) < *mn) throw ValidationError("too few tables", "$.tables");
  }

  // task list
  if (json_bool(sch, "requireTaskList", false)) {
    if (parsed.taskLineNumbers.empty()) throw ValidationError("task list required", "$.tasks");
  }

  // sections rules
  {
    auto it = sch.find("sections");
    if (it != sch.end() && it->second.is_object()) {
      const auto& sections = it->second.as_object();
      for (const auto& kv : sections) {
        const std::string& title = kv.first;
        const Json& rules_json = kv.second;
        if (!rules_json.is_object()) continue;

        auto sec_it = parsed.sections.find(title);
        if (sec_it == parsed.sections.end()) throw ValidationError("missing section: " + title, "$.sections[" + title + "]");

        std::string joined;
        for (const auto& line : sec_it->second) {
          joined += line;
          joined.push_back('\n');
        }
        if (!joined.empty()) joined.pop_back();

        const auto& rules = rules_json.as_object();
        if (auto mn = json_num_opt(rules, "minLength")) {
          if (static_cast<double>(joined.size()) < *mn) throw ValidationError("section too short: " + title, "$.sections[" + title + "].text");
        }
        if (auto mx = json_num_opt(rules, "maxLength")) {
          if (static_cast<double>(joined.size()) > *mx) throw ValidationError("section too long: " + title, "$.sections[" + title + "].text");
        }

        if (json_bool(rules, "requireBullets", false)) {
          bool has_bullet = false;
          for (const auto& line : sec_it->second) {
            if (std::regex_match(line, std::regex(R"(^\s*[-*+]\s+.+$)"))) {
              has_bullet = true;
              break;
            }
          }
          if (!has_bullet) throw ValidationError("section requires bullets: " + title, "$.sections[" + title + "].bullets");
        }

        int bullets = 0;
        for (const auto& line : sec_it->second) {
          if (std::regex_match(line, std::regex(R"(^\s*[-*+]\s+.+$)"))) bullets++;
        }
        if (auto mn = json_num_opt(rules, "minBullets")) {
          if (bullets < static_cast<int>(*mn)) throw ValidationError("too few bullets in section: " + title, "$.sections[" + title + "].bullets");
        }
        if (auto mx = json_num_opt(rules, "maxBullets")) {
          if (bullets > static_cast<int>(*mx)) throw ValidationError("too many bullets in section: " + title, "$.sections[" + title + "].bullets");
        }
      }
    }
  }
}

MarkdownParsed parse_and_validate_markdown(const std::string& text, const Json& schema) {
  MarkdownParsed p = parse_markdown(text);
  validate_markdown(p, schema);
  return p;
}

// ---------------- KV parsing/validation ----------------

KeyValue loads_kv(const std::string& text) {
  KeyValue out;
  auto lines = split_lines(text);
  std::regex kv_re(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*?)\s*$)");
  for (const auto& line : lines) {
    std::string t = ltrim_copy(line);
    if (t.empty() || t[0] == '#') continue;
    std::smatch m;
    if (std::regex_match(line, m, kv_re)) {
      std::string k = m[1].str();
      std::string v = m[2].str();
      // strip inline comment " #..." if value is unquoted
      if (!v.empty() && v.front() != '"' && v.front() != '\'') {
        auto hash = v.find(" #");
        if (hash != std::string::npos) v = v.substr(0, hash);
      }
      // unquote
      if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\''))) {
        v = v.substr(1, v.size() - 2);
      }
      out[k] = v;
    }
  }
  return out;
}

void validate_kv(const KeyValue& kv, const Json& schema) {
  const auto& sch = require_object_schema(schema, "$");

  // required
  for (const auto& k : json_string_list(sch, "required")) {
    if (kv.find(k) == kv.end()) throw ValidationError("missing required key: " + k, "$." + k);
  }

  bool allow_extra = true;
  {
    auto it = sch.find("allowExtra");
    if (it != sch.end() && it->second.is_bool()) allow_extra = it->second.as_bool();
  }

  // patterns
  std::map<std::string, std::regex> patterns;
  {
    auto it = sch.find("patterns");
    if (it != sch.end() && it->second.is_object()) {
      for (const auto& p : it->second.as_object()) {
        if (p.second.is_string()) {
          patterns.emplace(p.first, std::regex(p.second.as_string()));
        }
      }
    }
  }

  // enum
  std::map<std::string, std::vector<std::string>> enums;
  {
    auto it = sch.find("enum");
    if (it != sch.end() && it->second.is_object()) {
      for (const auto& e : it->second.as_object()) {
        if (!e.second.is_array()) continue;
        std::vector<std::string> vals;
        for (const auto& v : e.second.as_array()) {
          if (v.is_string()) vals.push_back(v.as_string());
        }
        enums.emplace(e.first, std::move(vals));
      }
    }
  }

  // if allowExtra==false, only required/patterns/enum keys allowed
  if (!allow_extra) {
    std::set<std::string> allowed;
    for (const auto& k : json_string_list(sch, "required")) allowed.insert(k);
    for (const auto& p : patterns) allowed.insert(p.first);
    for (const auto& e : enums) allowed.insert(e.first);

    for (const auto& kvp : kv) {
      if (allowed.find(kvp.first) == allowed.end()) {
        throw ValidationError("extra key not allowed: " + kvp.first, "$." + kvp.first);
      }
    }
  }

  for (const auto& kvp : kv) {
    auto pit = patterns.find(kvp.first);
    if (pit != patterns.end()) {
      if (!std::regex_search(kvp.second, pit->second)) {
        throw ValidationError("value does not match pattern for key: " + kvp.first, "$." + kvp.first);
      }
    }
    auto eit = enums.find(kvp.first);
    if (eit != enums.end()) {
      bool ok = false;
      for (const auto& allowed : eit->second) {
        if (kvp.second == allowed) {
          ok = true;
          break;
        }
      }
      if (!ok) throw ValidationError("value not in enum for key: " + kvp.first, "$." + kvp.first);
    }
  }
}

KeyValue parse_and_validate_kv(const std::string& text, const Json& schema) {
  auto kv = loads_kv(text);
  validate_kv(kv, schema);
  return kv;
}

// ---------------- YAML-ish extraction/parsing/validation ----------------

std::string extract_yaml_candidate(const std::string& text) {
  // Look for ```yaml or ```yml fenced blocks first
  const std::string fence_yaml = "```yaml";
  const std::string fence_yml = "```yml";
  const std::string fence_close = "```";
  
  auto lines = split_lines(text);
  
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string trimmed = ltrim_copy(lines[i]);
    if (trimmed.find(fence_yaml) == 0 || trimmed.find(fence_yml) == 0) {
      std::string body;
      for (size_t j = i + 1; j < lines.size(); ++j) {
        std::string line_trim = ltrim_copy(lines[j]);
        if (line_trim.find(fence_close) == 0) {
          return body;
        }
        if (!body.empty()) body += "\n";
        body += lines[j];
      }
      return body;
    }
  }
  
  // If no fence found, try to extract YAML-like content
  // Look for lines with key: value or list items starting with -
  std::string yaml_content;
  bool found_yaml_pattern = false;
  
  for (const auto& line : lines) {
    std::string trimmed = ltrim_copy(line);
    if (trimmed.empty()) continue;
    
    // Check for YAML patterns: "key:" or "- " or document separator "---"
    if (trimmed.find(':') != std::string::npos || 
        trimmed.find("- ") == 0 || 
        trimmed == "---") {
      found_yaml_pattern = true;
      if (!yaml_content.empty()) yaml_content += "\n";
      yaml_content += line;
    } else if (found_yaml_pattern && !trimmed.empty() && std::isspace(static_cast<unsigned char>(line[0]))) {
      // Continue with indented lines if we've found YAML pattern
      if (!yaml_content.empty()) yaml_content += "\n";
      yaml_content += line;
    } else if (found_yaml_pattern) {
      // Stop if we hit non-YAML content
      break;
    }
  }
  
  return found_yaml_pattern ? yaml_content : text;
}

std::vector<std::string> extract_yaml_candidates(const std::string& text) {
  std::vector<std::string> candidates;
  auto lines = split_lines(text);
  
  const std::string fence_yaml = "```yaml";
  const std::string fence_yml = "```yml";
  const std::string fence_close = "```";
  
  std::vector<bool> in_fence(lines.size(), false);
  
  // First pass: mark fenced regions
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string trimmed = ltrim_copy(lines[i]);
    if (trimmed.find(fence_yaml) == 0 || trimmed.find(fence_yml) == 0) {
      std::string body;
      for (size_t j = i + 1; j < lines.size(); ++j) {
        std::string line_trim = ltrim_copy(lines[j]);
        if (line_trim.find(fence_close) == 0) {
          candidates.push_back(body);
          for (size_t k = i; k <= j; ++k) in_fence[k] = true;
          i = j;
          break;
        }
        if (!body.empty()) body += "\n";
        body += lines[j];
      }
    }
  }
  
  // Second pass: look for YAML documents separated by ---
  std::string current_doc;
  bool in_yaml = false;
  
  for (size_t i = 0; i < lines.size(); ++i) {
    if (in_fence[i]) continue;
    
    std::string trimmed = ltrim_copy(lines[i]);
    
    // YAML document separator
    if (trimmed == "---" || trimmed == "---\n") {
      if (in_yaml && !current_doc.empty()) {
        candidates.push_back(current_doc);
      }
      current_doc.clear();
      in_yaml = true;
      continue;
    }
    
    // Detect YAML-like patterns
    if (trimmed.find(':') != std::string::npos || trimmed.find("- ") == 0) {
      in_yaml = true;
      if (!current_doc.empty()) current_doc += "\n";
      current_doc += lines[i];
    } else if (in_yaml && !trimmed.empty() && std::isspace(static_cast<unsigned char>(lines[i][0]))) {
      if (!current_doc.empty()) current_doc += "\n";
      current_doc += lines[i];
    } else if (in_yaml && !trimmed.empty()) {
      if (!current_doc.empty()) {
        candidates.push_back(current_doc);
        current_doc.clear();
      }
      in_yaml = false;
    }
  }
  
  if (in_yaml && !current_doc.empty()) {
    candidates.push_back(current_doc);
  }
  
  return candidates;
}

static std::string apply_yaml_repairs(const std::string& text, const YamlRepairConfig& cfg, YamlRepairMetadata& meta) {
  std::string result = text;
  
  // Fix tabs to spaces
  if (cfg.fix_tabs && result.find('\t') != std::string::npos) {
    meta.fixed_tabs = true;
    std::string fixed;
    for (char c : result) {
      if (c == '\t') {
        fixed += "  "; // Convert tab to 2 spaces
      } else {
        fixed.push_back(c);
      }
    }
    result = fixed;
  }
  
  // Normalize indentation (ensure consistent 2-space indentation)
  if (cfg.normalize_indentation) {
    auto lines = split_lines(result);
    std::string normalized;
    bool changed = false;
    
    for (const auto& line : lines) {
      if (line.empty()) {
        normalized += "\n";
        continue;
      }
      
      size_t indent = 0;
      while (indent < line.size() && std::isspace(static_cast<unsigned char>(line[indent]))) {
        indent++;
      }
      
      // Check if indentation is not a multiple of 2
      if (indent > 0 && indent % 2 != 0) {
        changed = true;
        size_t normalized_indent = ((indent + 1) / 2) * 2;
        normalized += std::string(normalized_indent, ' ') + line.substr(indent);
      } else {
        normalized += line;
      }
      normalized += "\n";
    }
    
    if (changed) {
      meta.normalized_indentation = true;
      result = normalized;
    }
  }
  
  return result;
}

// Minimalist YAML parser (supports subset: scalars, lists, objects, no advanced features)
static Json parse_yaml_value(const std::string& yaml_text);

// Forward declaration for recursive parsing
static Json parse_yaml_node(const std::vector<std::pair<int, std::string>>& content, size_t& idx, int parent_indent);

static Json parse_yaml_node(const std::vector<std::pair<int, std::string>>& content, size_t& idx, int parent_indent) {
  if (idx >= content.size()) return Json(nullptr);
  
  int indent = content[idx].first;
  const std::string& line = content[idx].second;
  
  // List item
  if (line.find("- ") == 0) {
    JsonArray arr;
    int list_indent = indent;
    
    while (idx < content.size() && content[idx].first >= list_indent) {
      int cur_indent = content[idx].first;
      const std::string& cur_line = content[idx].second;
      if (cur_indent == list_indent && cur_line.find("- ") == 0) {
        std::string item_text = cur_line.substr(2);
        item_text = ltrim_copy(item_text);
        
        if (item_text.empty()) {
          // Empty list item: "- " followed by nested content
          idx++;
          if (idx < content.size() && content[idx].first > list_indent) {
            arr.push_back(parse_yaml_node(content, idx, list_indent + 2));
          } else {
            arr.push_back(Json(nullptr));
          }
        } else if (item_text.find(':') != std::string::npos) {
          // List item starts with "- key: value" - this is an inline object
          // We need to collect all properties at the same or greater indent level
          JsonObject obj;
          
          // Parse the first key-value from the list item line
          size_t colon_pos = item_text.find(':');
          std::string key = item_text.substr(0, colon_pos);
          std::string val = item_text.substr(colon_pos + 1);
          key = ltrim_copy(key);
          while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
          val = ltrim_copy(val);
          
          // Calculate the virtual indent for subsequent properties
          // "- name: Alice" means "name" starts at indent + 2
          int item_obj_indent = list_indent + 2;
          
          if (val.empty()) {
            // Value is nested
            idx++;
            if (idx < content.size() && content[idx].first > item_obj_indent) {
              obj[key] = parse_yaml_node(content, idx, item_obj_indent);
            } else {
              obj[key] = Json(nullptr);
            }
          } else {
            obj[key] = parse_yaml_value(val);
            idx++;
          }
          
          // Now parse any additional properties at the same virtual indent level
          while (idx < content.size()) {
            int next_indent = content[idx].first;
            const std::string& next_line = content[idx].second;
            
            // Must be at the same indent as the object properties (item_obj_indent)
            if (next_indent != item_obj_indent) break;
            // Must not be a list item
            if (next_line.find("- ") == 0) break;
            // Must have a colon
            size_t next_colon = next_line.find(':');
            if (next_colon == std::string::npos) break;
            
            std::string next_key = next_line.substr(0, next_colon);
            std::string next_val = next_line.substr(next_colon + 1);
            next_key = ltrim_copy(next_key);
            while (!next_key.empty() && std::isspace(static_cast<unsigned char>(next_key.back()))) next_key.pop_back();
            next_val = ltrim_copy(next_val);
            
            if (next_val.empty()) {
              idx++;
              if (idx < content.size() && content[idx].first > next_indent) {
                obj[next_key] = parse_yaml_node(content, idx, next_indent);
              } else {
                obj[next_key] = Json(nullptr);
              }
            } else {
              obj[next_key] = parse_yaml_value(next_val);
              idx++;
            }
          }
          
          arr.push_back(Json(obj));
        } else {
          // Simple scalar list item
          idx++;
          arr.push_back(parse_yaml_value(item_text));
        }
      } else {
        break;
      }
    }
    return Json(arr);
  }
  
  // Object
  if (line.find(':') != std::string::npos) {
    JsonObject obj;
    int obj_indent = indent;
    
    while (idx < content.size() && content[idx].first >= obj_indent) {
      int cur_indent = content[idx].first;
      const std::string& cur_line = content[idx].second;
      if (cur_indent != obj_indent) break;
      
      size_t colon_pos = cur_line.find(':');
      if (colon_pos == std::string::npos) break;
      
      std::string key = cur_line.substr(0, colon_pos);
      key = ltrim_copy(key);
      while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
      
      std::string value_part = cur_line.substr(colon_pos + 1);
      value_part = ltrim_copy(value_part);
      
      idx++;
      
      if (value_part.empty()) {
        // Multi-line value
        if (idx < content.size() && content[idx].first > cur_indent) {
          obj[key] = parse_yaml_node(content, idx, cur_indent);
        } else {
          obj[key] = Json(nullptr);
        }
      } else {
        obj[key] = parse_yaml_value(value_part);
      }
    }
    return Json(obj);
  }
  
  // Scalar
  idx++;
  return parse_yaml_value(line);
}

static Json parse_yaml_impl(const std::string& text) {
  auto lines = split_lines(text);
  if (lines.empty()) return Json(nullptr);
  
  // Remove empty lines and trim
  std::vector<std::pair<int, std::string>> content; // (indent, line)
  for (const auto& line : lines) {
    std::string trimmed = ltrim_copy(line);
    if (trimmed.empty() || trimmed[0] == '#') continue;
    
    size_t indent = 0;
    while (indent < line.size() && std::isspace(static_cast<unsigned char>(line[indent]))) {
      indent++;
    }
    content.push_back({static_cast<int>(indent), trimmed});
  }
  
  if (content.empty()) return Json(nullptr);
  
  size_t idx = 0;
  return parse_yaml_node(content, idx, -1);
}

static Json parse_yaml_value(const std::string& yaml_text) {
  std::string val = yaml_text;
  
  // Trim
  while (!val.empty() && std::isspace(static_cast<unsigned char>(val.front()))) val.erase(0, 1);
  while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) val.pop_back();
  
  if (val.empty()) return Json(nullptr);
  
  // YAML literals
  if (val == "null" || val == "~") return Json(nullptr);
  if (val == "true" || val == "True" || val == "TRUE") return Json(true);
  if (val == "false" || val == "False" || val == "FALSE") return Json(false);
  
  // Quoted string
  if ((val.front() == '"' && val.back() == '"') || 
      (val.front() == '\'' && val.back() == '\'')) {
    return Json(val.substr(1, val.size() - 2));
  }
  
  // Number
  char* end = nullptr;
  double num = std::strtod(val.c_str(), &end);
  if (end == val.c_str() + val.size()) {
    return Json(num);
  }
  
  // Inline JSON array or object
  if ((val.front() == '[' && val.back() == ']') || 
      (val.front() == '{' && val.back() == '}')) {
    try {
      return loads_jsonish(val);
    } catch (...) {
      // Fall through to string
    }
  }
  
  // Default: string
  return Json(val);
}

Json loads_yamlish(const std::string& text) {
  YamlRepairConfig cfg;
  YamlRepairMetadata meta;
  std::string candidate = extract_yaml_candidate(text);
  meta.extracted_from_fence = (candidate != text);
  std::string fixed = apply_yaml_repairs(candidate, cfg, meta);
  return parse_yaml_impl(fixed);
}

YamlishParseResult loads_yamlish_ex(const std::string& text, const YamlRepairConfig& repair) {
  YamlRepairMetadata meta;
  std::string candidate = extract_yaml_candidate(text);
  meta.extracted_from_fence = (candidate != text);
  std::string fixed = apply_yaml_repairs(candidate, repair, meta);
  Json value = parse_yaml_impl(fixed);
  return {value, fixed, meta};
}

JsonArray loads_yamlish_all(const std::string& text) {
  auto candidates = extract_yaml_candidates(text);
  JsonArray result;
  for (const auto& cand : candidates) {
    result.push_back(loads_yamlish(cand));
  }
  return result;
}

YamlishParseAllResult loads_yamlish_all_ex(const std::string& text, const YamlRepairConfig& repair) {
  auto candidates = extract_yaml_candidates(text);
  YamlishParseAllResult result;
  for (const auto& cand : candidates) {
    auto r = loads_yamlish_ex(cand, repair);
    result.values.push_back(r.value);
    result.fixed.push_back(r.fixed);
    result.metadata.push_back(r.metadata);
  }
  return result;
}

Json parse_and_validate_yaml(const std::string& text, const Json& schema) {
  Json value = loads_yamlish(text);
  validate(value, schema, "$");
  return value;
}

YamlishParseResult parse_and_validate_yaml_ex(const std::string& text, const Json& schema, const YamlRepairConfig& repair) {
  auto result = loads_yamlish_ex(text, repair);
  validate(result.value, schema, "$");
  return result;
}

JsonArray parse_and_validate_yaml_all(const std::string& text, const Json& schema) {
  JsonArray values = loads_yamlish_all(text);
  for (size_t i = 0; i < values.size(); ++i) {
    validate(values[i], schema, "$[" + std::to_string(i) + "]");
  }
  return values;
}

YamlishParseAllResult parse_and_validate_yaml_all_ex(const std::string& text, const Json& schema, const YamlRepairConfig& repair) {
  auto result = loads_yamlish_all_ex(text, repair);
  for (size_t i = 0; i < result.values.size(); ++i) {
    validate(result.values[i], schema, "$[" + std::to_string(i) + "]");
  }
  return result;
}

static std::string yaml_to_string_impl(const Json& v, int indent, int level, bool inline_mode);

static std::string yaml_to_string_impl(const Json& v, int indent, int level, bool inline_mode) {
  std::string ind(level * indent, ' ');
  
  if (v.is_null()) return "null";
  if (v.is_bool()) return v.as_bool() ? "true" : "false";
  if (v.is_number()) {
    double num = v.as_number();
    if (std::floor(num) == num && num >= -1e15 && num <= 1e15) {
      return std::to_string(static_cast<int64_t>(num));
    }
    return std::to_string(num);
  }
  if (v.is_string()) {
    const auto& s = v.as_string();
    // Quote if contains special chars or looks like number/bool
    if (s.empty() || s == "null" || s == "true" || s == "false" || 
        s.find(':') != std::string::npos || s.find('#') != std::string::npos ||
        s.find('\n') != std::string::npos) {
      return "\"" + json_escape(s) + "\"";
    }
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    if (end == s.c_str() + s.size()) {
      return "\"" + s + "\"";
    }
    return s;
  }
  if (v.is_array()) {
    const auto& arr = v.as_array();
    if (arr.empty()) return "[]";
    
    std::string result;
    for (const auto& el : arr) {
      result += ind + "- ";
      if (el.is_object() || el.is_array()) {
        result += "\n" + yaml_to_string_impl(el, indent, level + 1, false);
      } else {
        result += yaml_to_string_impl(el, indent, 0, true);
      }
      result += "\n";
    }
    return result.substr(0, result.size() - 1); // Remove trailing newline
  }
  if (v.is_object()) {
    const auto& obj = v.as_object();
    if (obj.empty()) return "{}";
    
    std::string result;
    for (const auto& kv : obj) {
      result += ind + kv.first + ": ";
      if (kv.second.is_object() || kv.second.is_array()) {
        result += "\n" + yaml_to_string_impl(kv.second, indent, level + 1, false);
      } else {
        result += yaml_to_string_impl(kv.second, indent, 0, true);
      }
      result += "\n";
    }
    return result.substr(0, result.size() - 1); // Remove trailing newline
  }
  return "null";
}

std::string dumps_yaml(const Json& value, int indent) {
  return yaml_to_string_impl(value, indent, 0, false);
}

// ---------------- TOML extraction/parsing/validation ----------------

std::string extract_toml_candidate(const std::string& text) {
  // Try to find ```toml fenced block first
  size_t fence_start = text.find("```toml");
  if (fence_start == std::string::npos) {
    fence_start = text.find("```TOML");
  }
  
  if (fence_start != std::string::npos) {
    size_t content_start = text.find('\n', fence_start);
    if (content_start != std::string::npos) {
      content_start++;
      size_t fence_end = text.find("```", content_start);
      if (fence_end != std::string::npos) {
        return text.substr(content_start, fence_end - content_start);
      }
    }
  }
  
  // Look for TOML-like structure: [section] or key = value patterns
  std::string trimmed = text;
  // Trim leading/trailing whitespace
  size_t start = trimmed.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return text;
  size_t end = trimmed.find_last_not_of(" \t\n\r");
  trimmed = trimmed.substr(start, end - start + 1);
  
  // Check if it looks like TOML (has [section] headers or key = value lines)
  bool has_section = trimmed.find('[') != std::string::npos && trimmed.find(']') != std::string::npos;
  bool has_assignment = trimmed.find(" = ") != std::string::npos || trimmed.find("= ") != std::string::npos;
  
  if (has_section || has_assignment) {
    return trimmed;
  }
  
  return text;
}

std::vector<std::string> extract_toml_candidates(const std::string& text) {
  std::vector<std::string> results;
  
  // Find all ```toml fenced blocks
  size_t pos = 0;
  while (pos < text.size()) {
    size_t fence_start = text.find("```toml", pos);
    if (fence_start == std::string::npos) {
      fence_start = text.find("```TOML", pos);
    }
    
    if (fence_start == std::string::npos) break;
    
    size_t content_start = text.find('\n', fence_start);
    if (content_start == std::string::npos) break;
    content_start++;
    
    size_t fence_end = text.find("```", content_start);
    if (fence_end == std::string::npos) break;
    
    results.push_back(text.substr(content_start, fence_end - content_start));
    pos = fence_end + 3;
  }
  
  // If no fenced blocks found, try to extract a single TOML document
  if (results.empty()) {
    std::string candidate = extract_toml_candidate(text);
    if (!candidate.empty()) {
      results.push_back(candidate);
    }
  }
  
  return results;
}

static std::string apply_toml_repairs(const std::string& text, const TomlRepairConfig& cfg, TomlRepairMetadata& meta) {
  std::string result = text;
  
  // Normalize whitespace (tabs to spaces)
  if (cfg.normalize_whitespace) {
    std::string normalized;
    bool changed = false;
    for (char c : result) {
      if (c == '\t') {
        normalized += "  ";
        changed = true;
      } else {
        normalized += c;
      }
    }
    if (changed) {
      result = normalized;
      meta.normalized_whitespace = true;
    }
  }
  
  // Convert single quotes to double quotes (for string values)
  if (cfg.allow_single_quotes) {
    std::string converted;
    bool in_double = false;
    bool in_single = false;
    bool changed = false;
    
    for (size_t i = 0; i < result.size(); ++i) {
      char c = result[i];
      
      if (c == '"' && !in_single) {
        in_double = !in_double;
        converted += c;
      } else if (c == '\'' && !in_double) {
        // Convert single quote to double quote
        converted += '"';
        in_single = !in_single;
        changed = true;
      } else {
        converted += c;
      }
    }
    
    if (changed) {
      result = converted;
      meta.converted_single_quotes = true;
    }
  }
  
  return result;
}

// Forward declarations for TOML parsing
static Json parse_toml_value(const std::string& value_str);
static Json parse_toml_inline_table(const std::string& text, size_t& pos);
static Json parse_toml_inline_array(const std::string& text, size_t& pos);

// Parse a TOML value (string, number, bool, array, inline table, datetime)
static Json parse_toml_value(const std::string& value_str) {
  std::string trimmed = value_str;
  // Trim whitespace
  size_t start = trimmed.find_first_not_of(" \t");
  if (start == std::string::npos) return Json(nullptr);
  size_t end = trimmed.find_last_not_of(" \t\r\n");
  trimmed = trimmed.substr(start, end - start + 1);
  
  if (trimmed.empty()) return Json(nullptr);
  
  // Boolean
  if (trimmed == "true") return Json(true);
  if (trimmed == "false") return Json(false);
  
  // String (double-quoted)
  if (trimmed.size() >= 2 && trimmed[0] == '"' && trimmed.back() == '"') {
    std::string str = trimmed.substr(1, trimmed.size() - 2);
    // Handle escape sequences
    std::string unescaped;
    for (size_t i = 0; i < str.size(); ++i) {
      if (str[i] == '\\' && i + 1 < str.size()) {
        switch (str[i + 1]) {
          case 'n': unescaped += '\n'; ++i; break;
          case 't': unescaped += '\t'; ++i; break;
          case 'r': unescaped += '\r'; ++i; break;
          case '\\': unescaped += '\\'; ++i; break;
          case '"': unescaped += '"'; ++i; break;
          default: unescaped += str[i]; break;
        }
      } else {
        unescaped += str[i];
      }
    }
    return Json(unescaped);
  }
  
  // Multiline basic string
  if (trimmed.size() >= 6 && trimmed.substr(0, 3) == "\"\"\"" && trimmed.substr(trimmed.size() - 3) == "\"\"\"") {
    std::string str = trimmed.substr(3, trimmed.size() - 6);
    // Remove leading newline if present
    if (!str.empty() && str[0] == '\n') str = str.substr(1);
    return Json(str);
  }
  
  // Literal string (single-quoted, no escaping)
  if (trimmed.size() >= 2 && trimmed[0] == '\'' && trimmed.back() == '\'') {
    return Json(trimmed.substr(1, trimmed.size() - 2));
  }
  
  // Multiline literal string
  if (trimmed.size() >= 6 && trimmed.substr(0, 3) == "'''" && trimmed.substr(trimmed.size() - 3) == "'''") {
    std::string str = trimmed.substr(3, trimmed.size() - 6);
    if (!str.empty() && str[0] == '\n') str = str.substr(1);
    return Json(str);
  }
  
  // Inline table
  if (trimmed[0] == '{') {
    size_t pos = 0;
    return parse_toml_inline_table(trimmed, pos);
  }
  
  // Array
  if (trimmed[0] == '[') {
    size_t pos = 0;
    return parse_toml_inline_array(trimmed, pos);
  }
  
  // Integer (with optional underscores and prefixes)
  {
    std::string num_str = trimmed;
    // Remove underscores
    num_str.erase(std::remove(num_str.begin(), num_str.end(), '_'), num_str.end());
    
    // Check for hex, octal, binary
    if (num_str.size() > 2 && num_str[0] == '0') {
      if (num_str[1] == 'x' || num_str[1] == 'X') {
        // Hex
        try {
          int64_t val = std::stoll(num_str.substr(2), nullptr, 16);
          return Json(static_cast<double>(val));
        } catch (...) {}
      } else if (num_str[1] == 'o' || num_str[1] == 'O') {
        // Octal
        try {
          int64_t val = std::stoll(num_str.substr(2), nullptr, 8);
          return Json(static_cast<double>(val));
        } catch (...) {}
      } else if (num_str[1] == 'b' || num_str[1] == 'B') {
        // Binary
        try {
          int64_t val = std::stoll(num_str.substr(2), nullptr, 2);
          return Json(static_cast<double>(val));
        } catch (...) {}
      }
    }
    
    // Try integer
    try {
      size_t processed;
      int64_t ival = std::stoll(num_str, &processed);
      if (processed == num_str.size()) {
        return Json(static_cast<double>(ival));
      }
    } catch (...) {}
    
    // Try float (including inf, nan)
    if (num_str == "inf" || num_str == "+inf") {
      return Json(std::numeric_limits<double>::infinity());
    }
    if (num_str == "-inf") {
      return Json(-std::numeric_limits<double>::infinity());
    }
    if (num_str == "nan" || num_str == "+nan" || num_str == "-nan") {
      return Json(std::numeric_limits<double>::quiet_NaN());
    }
    
    try {
      size_t processed;
      double dval = std::stod(num_str, &processed);
      if (processed == num_str.size()) {
        return Json(dval);
      }
    } catch (...) {}
  }
  
  // Datetime (return as string for now)
  // Check for ISO 8601 patterns
  if (trimmed.size() >= 10 && std::isdigit(trimmed[0]) && trimmed[4] == '-' && trimmed[7] == '-') {
    return Json(trimmed);
  }
  
  // Time only
  if (trimmed.size() >= 5 && std::isdigit(trimmed[0]) && trimmed[2] == ':') {
    return Json(trimmed);
  }
  
  // Unquoted string (be lenient)
  return Json(trimmed);
}

static Json parse_toml_inline_table(const std::string& text, size_t& pos) {
  JsonObject obj;
  
  if (pos >= text.size() || text[pos] != '{') return Json(obj);
  ++pos; // skip '{'
  
  while (pos < text.size()) {
    // Skip whitespace
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
      ++pos;
    }
    
    if (pos >= text.size() || text[pos] == '}') {
      ++pos;
      break;
    }
    
    // Parse key
    std::string key;
    while (pos < text.size() && text[pos] != '=' && text[pos] != ' ' && text[pos] != '\t') {
      if (text[pos] == '"') {
        // Quoted key
        ++pos;
        while (pos < text.size() && text[pos] != '"') {
          if (text[pos] == '\\' && pos + 1 < text.size()) {
            key += text[pos + 1];
            pos += 2;
          } else {
            key += text[pos++];
          }
        }
        if (pos < text.size()) ++pos; // skip closing quote
        break;
      }
      key += text[pos++];
    }
    
    // Skip to '='
    while (pos < text.size() && text[pos] != '=') ++pos;
    if (pos < text.size()) ++pos; // skip '='
    
    // Skip whitespace
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
    
    // Parse value
    std::string value_str;
    int brace_depth = 0;
    int bracket_depth = 0;
    bool in_string = false;
    char string_char = 0;
    
    while (pos < text.size()) {
      char c = text[pos];
      
      if (!in_string) {
        if (c == '"' || c == '\'') {
          in_string = true;
          string_char = c;
          value_str += c;
          ++pos;
          continue;
        }
        if (c == '{') brace_depth++;
        if (c == '}') {
          if (brace_depth == 0) break;
          brace_depth--;
        }
        if (c == '[') bracket_depth++;
        if (c == ']') bracket_depth--;
        if (c == ',' && brace_depth == 0 && bracket_depth == 0) {
          ++pos;
          break;
        }
      } else {
        if (c == string_char && (value_str.empty() || value_str.back() != '\\')) {
          in_string = false;
        }
      }
      
      value_str += c;
      ++pos;
    }
    
    if (!key.empty()) {
      obj[key] = parse_toml_value(value_str);
    }
  }
  
  return Json(obj);
}

static Json parse_toml_inline_array(const std::string& text, size_t& pos) {
  JsonArray arr;
  
  if (pos >= text.size() || text[pos] != '[') return Json(arr);
  ++pos; // skip '['
  
  while (pos < text.size()) {
    // Skip whitespace and newlines
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
      ++pos;
    }
    
    if (pos >= text.size() || text[pos] == ']') {
      ++pos;
      break;
    }
    
    // Parse element
    std::string value_str;
    int brace_depth = 0;
    int bracket_depth = 0;
    bool in_string = false;
    char string_char = 0;
    
    while (pos < text.size()) {
      char c = text[pos];
      
      if (!in_string) {
        if (c == '"' || c == '\'') {
          in_string = true;
          string_char = c;
          value_str += c;
          ++pos;
          continue;
        }
        if (c == '{') brace_depth++;
        if (c == '}') brace_depth--;
        if (c == '[') bracket_depth++;
        if (c == ']') {
          if (bracket_depth == 0) break;
          bracket_depth--;
        }
        if (c == ',' && brace_depth == 0 && bracket_depth == 0) {
          ++pos;
          break;
        }
      } else {
        if (c == string_char && (value_str.empty() || value_str.back() != '\\')) {
          in_string = false;
        }
      }
      
      value_str += c;
      ++pos;
    }
    
    // Trim value
    size_t start = value_str.find_first_not_of(" \t\n\r");
    size_t end = value_str.find_last_not_of(" \t\n\r");
    if (start != std::string::npos && end != std::string::npos) {
      value_str = value_str.substr(start, end - start + 1);
    }
    
    if (!value_str.empty()) {
      arr.push_back(parse_toml_value(value_str));
    }
  }
  
  return Json(arr);
}

static Json parse_toml_impl(const std::string& text) {
  JsonObject root;
  JsonObject* current_table = &root;
  std::string current_path;
  bool in_array_of_tables = false;
  
  std::vector<std::string> lines;
  std::istringstream iss(text);
  std::string line;
  while (std::getline(iss, line)) {
    lines.push_back(line);
  }
  
  // Accumulate multiline values
  std::string accumulated_value;
  std::string pending_key;
  bool in_multiline_string = false;
  bool in_multiline_array = false;
  int array_bracket_depth = 0;
  
  for (size_t i = 0; i < lines.size(); ++i) {
    line = lines[i];
    
    // Handle multiline string continuation
    if (in_multiline_string) {
      accumulated_value += "\n" + line;
      // Check for closing quotes
      if (accumulated_value.find("\"\"\"") != std::string::npos && 
          accumulated_value.rfind("\"\"\"") > accumulated_value.find("\"\"\"") + 2) {
        current_table->operator[](pending_key) = parse_toml_value(accumulated_value);
        in_multiline_string = false;
        accumulated_value.clear();
        pending_key.clear();
      } else if (accumulated_value.find("'''") != std::string::npos &&
                 accumulated_value.rfind("'''") > accumulated_value.find("'''") + 2) {
        current_table->operator[](pending_key) = parse_toml_value(accumulated_value);
        in_multiline_string = false;
        accumulated_value.clear();
        pending_key.clear();
      }
      continue;
    }
    
    // Handle multiline array continuation
    if (in_multiline_array) {
      accumulated_value += "\n" + line;
      for (char c : line) {
        if (c == '[') array_bracket_depth++;
        else if (c == ']') array_bracket_depth--;
      }
      if (array_bracket_depth <= 0) {
        current_table->operator[](pending_key) = parse_toml_value(accumulated_value);
        in_multiline_array = false;
        accumulated_value.clear();
        pending_key.clear();
      }
      continue;
    }
    
    // Trim whitespace
    size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos) continue;
    size_t end = line.find_last_not_of(" \t\r\n");
    line = line.substr(start, end - start + 1);
    
    // Skip empty lines and comments
    if (line.empty() || line[0] == '#') continue;
    
    // Check for array of tables [[section]]
    if (line.size() >= 4 && line[0] == '[' && line[1] == '[') {
      size_t close = line.rfind("]]");
      if (close != std::string::npos) {
        std::string path = line.substr(2, close - 2);
        // Trim the path
        size_t ps = path.find_first_not_of(" \t");
        size_t pe = path.find_last_not_of(" \t");
        if (ps != std::string::npos && pe != std::string::npos) {
          path = path.substr(ps, pe - ps + 1);
        }
        
        // Navigate/create path and add array element
        current_table = &root;
        std::istringstream path_stream(path);
        std::string segment;
        std::vector<std::string> segments;
        while (std::getline(path_stream, segment, '.')) {
          // Handle quoted keys
          size_t qs = segment.find_first_not_of(" \t\"");
          size_t qe = segment.find_last_not_of(" \t\"");
          if (qs != std::string::npos && qe != std::string::npos) {
            segment = segment.substr(qs, qe - qs + 1);
          }
          segments.push_back(segment);
        }
        
        for (size_t si = 0; si < segments.size(); ++si) {
          const std::string& seg = segments[si];
          if (si == segments.size() - 1) {
            // Last segment: create or access array, add new table
            if (current_table->find(seg) == current_table->end()) {
              current_table->operator[](seg) = JsonArray{};
            }
            auto& arr = current_table->operator[](seg).as_array();
            arr.push_back(JsonObject{});
            current_table = &arr.back().as_object();
          } else {
            // Navigate to nested table
            if (current_table->find(seg) == current_table->end()) {
              current_table->operator[](seg) = JsonObject{};
            }
            Json& next = current_table->operator[](seg);
            if (next.is_array()) {
              // Get last element of array
              current_table = &next.as_array().back().as_object();
            } else {
              current_table = &next.as_object();
            }
          }
        }
        current_path = path;
        in_array_of_tables = true;
        continue;
      }
    }
    
    // Check for table [section]
    if (line[0] == '[' && (line.size() < 2 || line[1] != '[')) {
      size_t close = line.find(']');
      if (close != std::string::npos) {
        std::string path = line.substr(1, close - 1);
        // Trim the path
        size_t ps = path.find_first_not_of(" \t");
        size_t pe = path.find_last_not_of(" \t");
        if (ps != std::string::npos && pe != std::string::npos) {
          path = path.substr(ps, pe - ps + 1);
        }
        
        // Navigate/create nested tables
        current_table = &root;
        std::istringstream path_stream(path);
        std::string segment;
        while (std::getline(path_stream, segment, '.')) {
          // Handle quoted keys
          size_t qs = segment.find_first_not_of(" \t\"");
          size_t qe = segment.find_last_not_of(" \t\"");
          if (qs != std::string::npos && qe != std::string::npos) {
            segment = segment.substr(qs, qe - qs + 1);
          }
          
          if (current_table->find(segment) == current_table->end()) {
            current_table->operator[](segment) = JsonObject{};
          }
          Json& next = current_table->operator[](segment);
          if (next.is_array()) {
            // Navigate into the last element of an array of tables
            current_table = &next.as_array().back().as_object();
          } else {
            current_table = &next.as_object();
          }
        }
        current_path = path;
        in_array_of_tables = false;
        continue;
      }
    }
    
    // Key-value pair
    size_t eq_pos = line.find('=');
    if (eq_pos != std::string::npos) {
      std::string key = line.substr(0, eq_pos);
      std::string value = line.substr(eq_pos + 1);
      
      // Trim key
      size_t ks = key.find_first_not_of(" \t");
      size_t ke = key.find_last_not_of(" \t");
      if (ks != std::string::npos && ke != std::string::npos) {
        key = key.substr(ks, ke - ks + 1);
      }
      
      // Remove quotes from key if present
      if (key.size() >= 2 && key[0] == '"' && key.back() == '"') {
        key = key.substr(1, key.size() - 2);
      }
      
      // Trim value
      size_t vs = value.find_first_not_of(" \t");
      if (vs != std::string::npos) {
        value = value.substr(vs);
      }
      
      // Remove trailing comment (if not in string)
      bool in_str = false;
      char str_char = 0;
      for (size_t ci = 0; ci < value.size(); ++ci) {
        if (!in_str && (value[ci] == '"' || value[ci] == '\'')) {
          in_str = true;
          str_char = value[ci];
        } else if (in_str && value[ci] == str_char && (ci == 0 || value[ci - 1] != '\\')) {
          in_str = false;
        } else if (!in_str && value[ci] == '#') {
          value = value.substr(0, ci);
          break;
        }
      }
      
      // Trim value again
      ke = value.find_last_not_of(" \t\r\n");
      if (ke != std::string::npos) {
        value = value.substr(0, ke + 1);
      }
      
      // Check for multiline string start
      if ((value.substr(0, 3) == "\"\"\"" && value.find("\"\"\"", 3) == std::string::npos) ||
          (value.substr(0, 3) == "'''" && value.find("'''", 3) == std::string::npos)) {
        in_multiline_string = true;
        accumulated_value = value;
        pending_key = key;
        continue;
      }
      
      // Check for multiline array start
      if (value[0] == '[') {
        array_bracket_depth = 0;
        for (char c : value) {
          if (c == '[') array_bracket_depth++;
          else if (c == ']') array_bracket_depth--;
        }
        if (array_bracket_depth > 0) {
          in_multiline_array = true;
          accumulated_value = value;
          pending_key = key;
          continue;
        }
      }
      
      // Handle dotted keys (e.g., a.b.c = value)
      if (key.find('.') != std::string::npos) {
        JsonObject* target = current_table;
        std::istringstream key_stream(key);
        std::string key_part;
        std::vector<std::string> key_parts;
        while (std::getline(key_stream, key_part, '.')) {
          size_t kps = key_part.find_first_not_of(" \t\"");
          size_t kpe = key_part.find_last_not_of(" \t\"");
          if (kps != std::string::npos && kpe != std::string::npos) {
            key_part = key_part.substr(kps, kpe - kps + 1);
          }
          key_parts.push_back(key_part);
        }
        
        for (size_t ki = 0; ki < key_parts.size() - 1; ++ki) {
          if (target->find(key_parts[ki]) == target->end()) {
            target->operator[](key_parts[ki]) = JsonObject{};
          }
          target = &target->operator[](key_parts[ki]).as_object();
        }
        target->operator[](key_parts.back()) = parse_toml_value(value);
      } else {
        current_table->operator[](key) = parse_toml_value(value);
      }
    }
  }
  
  return Json(root);
}

Json loads_tomlish(const std::string& text) {
  TomlRepairConfig cfg;
  TomlRepairMetadata meta;
  std::string candidate = extract_toml_candidate(text);
  meta.extracted_from_fence = (candidate != text);
  std::string fixed = apply_toml_repairs(candidate, cfg, meta);
  return parse_toml_impl(fixed);
}

TomlishParseResult loads_tomlish_ex(const std::string& text, const TomlRepairConfig& repair) {
  TomlRepairMetadata meta;
  std::string candidate = extract_toml_candidate(text);
  meta.extracted_from_fence = (candidate != text);
  std::string fixed = apply_toml_repairs(candidate, repair, meta);
  Json value = parse_toml_impl(fixed);
  return {value, fixed, meta};
}

JsonArray loads_tomlish_all(const std::string& text) {
  auto candidates = extract_toml_candidates(text);
  JsonArray result;
  for (const auto& cand : candidates) {
    result.push_back(loads_tomlish(cand));
  }
  return result;
}

TomlishParseAllResult loads_tomlish_all_ex(const std::string& text, const TomlRepairConfig& repair) {
  auto candidates = extract_toml_candidates(text);
  TomlishParseAllResult result;
  for (const auto& cand : candidates) {
    auto r = loads_tomlish_ex(cand, repair);
    result.values.push_back(r.value);
    result.fixed.push_back(r.fixed);
    result.metadata.push_back(r.metadata);
  }
  return result;
}

Json parse_and_validate_toml(const std::string& text, const Json& schema) {
  Json value = loads_tomlish(text);
  validate(value, schema, "$");
  return value;
}

TomlishParseResult parse_and_validate_toml_ex(const std::string& text, const Json& schema, const TomlRepairConfig& repair) {
  auto result = loads_tomlish_ex(text, repair);
  validate(result.value, schema, "$");
  return result;
}

JsonArray parse_and_validate_toml_all(const std::string& text, const Json& schema) {
  JsonArray values = loads_tomlish_all(text);
  for (size_t i = 0; i < values.size(); ++i) {
    validate(values[i], schema, "$[" + std::to_string(i) + "]");
  }
  return values;
}

TomlishParseAllResult parse_and_validate_toml_all_ex(const std::string& text, const Json& schema, const TomlRepairConfig& repair) {
  auto result = loads_tomlish_all_ex(text, repair);
  for (size_t i = 0; i < result.values.size(); ++i) {
    validate(result.values[i], schema, "$[" + std::to_string(i) + "]");
  }
  return result;
}

static std::string toml_escape_string(const std::string& s) {
  std::string result;
  for (char c : s) {
    switch (c) {
      case '\n': result += "\\n"; break;
      case '\t': result += "\\t"; break;
      case '\r': result += "\\r"; break;
      case '\\': result += "\\\\"; break;
      case '"': result += "\\\""; break;
      default: result += c; break;
    }
  }
  return result;
}

static void dumps_toml_impl(const Json& value, const std::string& prefix, std::string& output, bool is_root);

static void dumps_toml_impl(const Json& value, const std::string& prefix, std::string& output, bool is_root) {
  if (!value.is_object()) {
    // Non-object at root - just output as key-value if possible
    return;
  }
  
  const auto& obj = value.as_object();
  
  // First pass: output all non-table, non-array-of-tables values
  for (const auto& kv : obj) {
    const std::string& key = kv.first;
    const Json& val = kv.second;
    
    // Skip nested tables and arrays of tables (handled later)
    if (val.is_object()) continue;
    if (val.is_array() && !val.as_array().empty() && val.as_array()[0].is_object()) continue;
    
    // Quote key if needed
    std::string safe_key = key;
    if (key.find(' ') != std::string::npos || key.find('.') != std::string::npos || 
        key.find('#') != std::string::npos || key.find('=') != std::string::npos) {
      safe_key = "\"" + toml_escape_string(key) + "\"";
    }
    
    output += safe_key + " = ";
    
    if (val.is_null()) {
      output += "\"\"";  // TOML has no null, use empty string
    } else if (val.is_bool()) {
      output += val.as_bool() ? "true" : "false";
    } else if (val.is_number()) {
      double num = val.as_number();
      if (std::floor(num) == num && num >= -1e15 && num <= 1e15) {
        output += std::to_string(static_cast<int64_t>(num));
      } else {
        output += std::to_string(num);
      }
    } else if (val.is_string()) {
      output += "\"" + toml_escape_string(val.as_string()) + "\"";
    } else if (val.is_array()) {
      output += "[";
      const auto& arr = val.as_array();
      for (size_t i = 0; i < arr.size(); ++i) {
        if (i > 0) output += ", ";
        const Json& el = arr[i];
        if (el.is_null()) {
          output += "\"\"";
        } else if (el.is_bool()) {
          output += el.as_bool() ? "true" : "false";
        } else if (el.is_number()) {
          double num = el.as_number();
          if (std::floor(num) == num && num >= -1e15 && num <= 1e15) {
            output += std::to_string(static_cast<int64_t>(num));
          } else {
            output += std::to_string(num);
          }
        } else if (el.is_string()) {
          output += "\"" + toml_escape_string(el.as_string()) + "\"";
        }
      }
      output += "]";
    }
    output += "\n";
  }
  
  // Second pass: output nested tables
  for (const auto& kv : obj) {
    const std::string& key = kv.first;
    const Json& val = kv.second;
    
    if (val.is_object()) {
      std::string new_prefix = prefix.empty() ? key : prefix + "." + key;
      output += "\n[" + new_prefix + "]\n";
      dumps_toml_impl(val, new_prefix, output, false);
    }
  }
  
  // Third pass: output arrays of tables
  for (const auto& kv : obj) {
    const std::string& key = kv.first;
    const Json& val = kv.second;
    
    if (val.is_array() && !val.as_array().empty() && val.as_array()[0].is_object()) {
      const auto& arr = val.as_array();
      std::string new_prefix = prefix.empty() ? key : prefix + "." + key;
      for (const auto& el : arr) {
        output += "\n[[" + new_prefix + "]]\n";
        dumps_toml_impl(el, new_prefix, output, false);
      }
    }
  }
}

std::string dumps_toml(const Json& value) {
  std::string output;
  dumps_toml_impl(value, "", output, true);
  // Trim leading newline if present
  if (!output.empty() && output[0] == '\n') {
    output = output.substr(1);
  }
  return output;
}

// ---------------- XML/HTML extraction/parsing/validation ----------------

// HTML void elements (self-closing by default)
static const std::set<std::string> html_void_elements = {
    "area", "base", "br", "col", "embed", "hr", "img", "input",
    "link", "meta", "param", "source", "track", "wbr"
};

// HTML entities map
static const std::map<std::string, std::string> html_entities = {
    {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"},
    {"nbsp", "\xC2\xA0"}, {"copy", "\xC2\xA9"}, {"reg", "\xC2\xAE"},
    {"trade", "\xE2\x84\xA2"}, {"euro", "\xE2\x82\xAC"}, {"pound", "\xC2\xA3"},
    {"yen", "\xC2\xA5"}, {"cent", "\xC2\xA2"}, {"deg", "\xC2\xB0"},
    {"plusmn", "\xC2\xB1"}, {"times", "\xC3\x97"}, {"divide", "\xC3\xB7"},
    {"mdash", "\xE2\x80\x94"}, {"ndash", "\xE2\x80\x93"}, {"hellip", "\xE2\x80\xA6"},
    {"laquo", "\xC2\xAB"}, {"raquo", "\xC2\xBB"}, {"ldquo", "\xE2\x80\x9C"},
    {"rdquo", "\xE2\x80\x9D"}, {"lsquo", "\xE2\x80\x98"}, {"rsquo", "\xE2\x80\x99"}
};

static std::string decode_html_entity(const std::string& entity) {
  // Named entity
  auto it = html_entities.find(entity);
  if (it != html_entities.end()) {
    return it->second;
  }
  
  // Numeric entity &#123; or &#x1F;
  if (!entity.empty() && entity[0] == '#') {
    try {
      unsigned long code = 0;
      if (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')) {
        code = std::stoul(entity.substr(2), nullptr, 16);
      } else {
        code = std::stoul(entity.substr(1), nullptr, 10);
      }
      // Convert to UTF-8
      std::string utf8;
      if (code < 0x80) {
        utf8 += static_cast<char>(code);
      } else if (code < 0x800) {
        utf8 += static_cast<char>(0xC0 | (code >> 6));
        utf8 += static_cast<char>(0x80 | (code & 0x3F));
      } else if (code < 0x10000) {
        utf8 += static_cast<char>(0xE0 | (code >> 12));
        utf8 += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        utf8 += static_cast<char>(0x80 | (code & 0x3F));
      } else if (code < 0x110000) {
        utf8 += static_cast<char>(0xF0 | (code >> 18));
        utf8 += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        utf8 += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        utf8 += static_cast<char>(0x80 | (code & 0x3F));
      }
      return utf8;
    } catch (...) {
      return "&" + entity + ";";
    }
  }
  
  return "&" + entity + ";";
}

static std::string decode_html_entities(const std::string& text) {
  std::string result;
  result.reserve(text.size());
  
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '&') {
      size_t end = text.find(';', i + 1);
      if (end != std::string::npos && end - i < 12) {
        std::string entity = text.substr(i + 1, end - i - 1);
        result += decode_html_entity(entity);
        i = end;
        continue;
      }
    }
    result += text[i];
  }
  
  return result;
}

std::string extract_xml_candidate(const std::string& text) {
  // Try to find ```xml or ```html fenced block first
  for (const char* fence : {"```xml", "```XML", "```html", "```HTML"}) {
    size_t fence_start = text.find(fence);
    if (fence_start != std::string::npos) {
      size_t content_start = text.find('\n', fence_start);
      if (content_start != std::string::npos) {
        content_start++;
        size_t fence_end = text.find("```", content_start);
        if (fence_end != std::string::npos) {
          return text.substr(content_start, fence_end - content_start);
        }
      }
    }
  }
  
  // Look for XML declaration or root element
  size_t start = text.find("<?xml");
  if (start == std::string::npos) {
    start = text.find("<!DOCTYPE");
  }
  if (start == std::string::npos) {
    // Find first < that starts a tag
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '<' && i + 1 < text.size() && 
          (std::isalpha(text[i + 1]) || text[i + 1] == '!' || text[i + 1] == '?')) {
        start = i;
        break;
      }
    }
  }
  
  if (start == std::string::npos) return text;
  
  // Find the end - look for matching close or end of last tag
  int depth = 0;
  size_t end = start;
  bool in_tag = false;
  bool in_string = false;
  char string_char = 0;
  
  for (size_t i = start; i < text.size(); ++i) {
    char c = text[i];
    
    if (in_string) {
      if (c == string_char) in_string = false;
      continue;
    }
    
    if (in_tag) {
      if (c == '"' || c == '\'') {
        in_string = true;
        string_char = c;
      } else if (c == '>') {
        in_tag = false;
        end = i + 1;
      }
      continue;
    }
    
    if (c == '<') {
      if (i + 1 < text.size()) {
        if (text[i + 1] == '/') {
          depth--;
        } else if (text[i + 1] == '!' || text[i + 1] == '?') {
          // Comment, doctype, or PI - don't change depth
        } else if (std::isalpha(text[i + 1])) {
          // Check for self-closing
          size_t close = text.find('>', i);
          if (close != std::string::npos && text[close - 1] != '/') {
            depth++;
          }
        }
      }
      in_tag = true;
    }
    
    if (depth <= 0 && end > start) break;
  }
  
  return text.substr(start, end - start);
}

std::vector<std::string> extract_xml_candidates(const std::string& text) {
  std::vector<std::string> results;
  
  // Find all ```xml or ```html fenced blocks
  size_t pos = 0;
  while (pos < text.size()) {
    size_t fence_start = std::string::npos;
    for (const char* fence : {"```xml", "```XML", "```html", "```HTML"}) {
      size_t found = text.find(fence, pos);
      if (found != std::string::npos && (fence_start == std::string::npos || found < fence_start)) {
        fence_start = found;
      }
    }
    
    if (fence_start == std::string::npos) break;
    
    size_t content_start = text.find('\n', fence_start);
    if (content_start == std::string::npos) break;
    content_start++;
    
    size_t fence_end = text.find("```", content_start);
    if (fence_end == std::string::npos) break;
    
    results.push_back(text.substr(content_start, fence_end - content_start));
    pos = fence_end + 3;
  }
  
  // If no fenced blocks found, try to extract a single XML/HTML document
  if (results.empty()) {
    std::string candidate = extract_xml_candidate(text);
    if (!candidate.empty()) {
      results.push_back(candidate);
    }
  }
  
  return results;
}

// Forward declarations
static XmlNode parse_xml_node(const std::string& text, size_t& pos, const XmlRepairConfig& cfg, XmlRepairMetadata& meta);
static void skip_whitespace(const std::string& text, size_t& pos);
static std::string parse_xml_name(const std::string& text, size_t& pos);
static std::string parse_xml_attribute_value(const std::string& text, size_t& pos, const XmlRepairConfig& cfg, XmlRepairMetadata& meta);

static void skip_whitespace(const std::string& text, size_t& pos) {
  while (pos < text.size() && std::isspace(text[pos])) ++pos;
}

static std::string parse_xml_name(const std::string& text, size_t& pos) {
  std::string name;
  while (pos < text.size()) {
    char c = text[pos];
    if (std::isalnum(c) || c == '_' || c == '-' || c == ':' || c == '.') {
      name += c;
      ++pos;
    } else {
      break;
    }
  }
  return name;
}

static std::string parse_xml_attribute_value(const std::string& text, size_t& pos, const XmlRepairConfig& cfg, XmlRepairMetadata& meta) {
  skip_whitespace(text, pos);
  
  if (pos >= text.size()) return "";
  
  char quote = text[pos];
  if (quote == '"' || quote == '\'') {
    ++pos;
    std::string value;
    while (pos < text.size() && text[pos] != quote) {
      value += text[pos++];
    }
    if (pos < text.size()) ++pos; // Skip closing quote
    if (cfg.decode_entities) {
      std::string decoded = decode_html_entities(value);
      if (decoded != value) meta.decoded_entities = true;
      return decoded;
    }
    return value;
  }
  
  // Unquoted attribute value (HTML-style)
  if (cfg.fix_unquoted_attributes) {
    meta.fixed_unquoted_attributes = true;
    std::string value;
    while (pos < text.size() && !std::isspace(text[pos]) && 
           text[pos] != '>' && text[pos] != '/' && text[pos] != '=') {
      value += text[pos++];
    }
    if (cfg.decode_entities) {
      std::string decoded = decode_html_entities(value);
      if (decoded != value) meta.decoded_entities = true;
      return decoded;
    }
    return value;
  }
  
  return "";
}

static XmlNode parse_xml_node(const std::string& text, size_t& pos, const XmlRepairConfig& cfg, XmlRepairMetadata& meta) {
  skip_whitespace(text, pos);
  
  if (pos >= text.size()) {
    XmlNode empty;
    empty.type = XmlNode::Type::Text;
    return empty;
  }
  
  // Text node
  if (text[pos] != '<') {
    XmlNode node;
    node.type = XmlNode::Type::Text;
    while (pos < text.size() && text[pos] != '<') {
      node.text += text[pos++];
    }
    if (cfg.decode_entities) {
      std::string decoded = decode_html_entities(node.text);
      if (decoded != node.text) {
        meta.decoded_entities = true;
        node.text = decoded;
      }
    }
    if (cfg.normalize_whitespace) {
      // Normalize whitespace
      std::string normalized;
      bool last_was_space = true;
      for (char c : node.text) {
        if (std::isspace(c)) {
          if (!last_was_space) {
            normalized += ' ';
            last_was_space = true;
          }
        } else {
          normalized += c;
          last_was_space = false;
        }
      }
      if (normalized != node.text) {
        meta.normalized_whitespace = true;
        node.text = normalized;
      }
    }
    return node;
  }
  
  // Check for special nodes
  if (pos + 1 < text.size()) {
    // Comment <!-- ... -->
    if (text.substr(pos, 4) == "<!--") {
      XmlNode node;
      node.type = XmlNode::Type::Comment;
      pos += 4;
      size_t end = text.find("-->", pos);
      if (end != std::string::npos) {
        node.text = text.substr(pos, end - pos);
        pos = end + 3;
      } else {
        node.text = text.substr(pos);
        pos = text.size();
      }
      return node;
    }
    
    // CDATA <![CDATA[ ... ]]>
    if (text.substr(pos, 9) == "<![CDATA[") {
      XmlNode node;
      node.type = XmlNode::Type::CData;
      pos += 9;
      size_t end = text.find("]]>", pos);
      if (end != std::string::npos) {
        node.text = text.substr(pos, end - pos);
        pos = end + 3;
      } else {
        node.text = text.substr(pos);
        pos = text.size();
      }
      return node;
    }
    
    // DOCTYPE <!DOCTYPE ...>
    if (text.substr(pos, 9) == "<!DOCTYPE" || text.substr(pos, 9) == "<!doctype") {
      XmlNode node;
      node.type = XmlNode::Type::Doctype;
      pos += 9;
      int depth = 1;
      while (pos < text.size() && depth > 0) {
        if (text[pos] == '<') depth++;
        else if (text[pos] == '>') depth--;
        if (depth > 0) node.text += text[pos];
        ++pos;
      }
      return node;
    }
    
    // Processing instruction <?...?>
    if (text[pos] == '<' && text[pos + 1] == '?') {
      XmlNode node;
      node.type = XmlNode::Type::ProcessingInstruction;
      pos += 2;
      node.name = parse_xml_name(text, pos);
      skip_whitespace(text, pos);
      size_t end = text.find("?>", pos);
      if (end != std::string::npos) {
        node.text = text.substr(pos, end - pos);
        pos = end + 2;
      } else {
        node.text = text.substr(pos);
        pos = text.size();
      }
      return node;
    }
    
    // Closing tag </...> - shouldn't happen at top level, but handle gracefully
    if (text[pos] == '<' && text[pos + 1] == '/') {
      // Skip to end of closing tag
      size_t end = text.find('>', pos);
      if (end != std::string::npos) pos = end + 1;
      XmlNode empty;
      empty.type = XmlNode::Type::Text;
      return empty;
    }
  }
  
  // Element node
  XmlNode node;
  node.type = XmlNode::Type::Element;
  
  ++pos; // Skip '<'
  node.name = parse_xml_name(text, pos);
  
  if (cfg.lowercase_names) {
    std::string lower = to_lower(node.name);
    if (lower != node.name) {
      meta.lowercased_names = true;
      node.name = lower;
    }
  }
  
  // Parse attributes
  while (pos < text.size()) {
    skip_whitespace(text, pos);
    
    if (pos >= text.size()) break;
    if (text[pos] == '>' || text[pos] == '/') break;
    
    std::string attr_name = parse_xml_name(text, pos);
    if (attr_name.empty()) {
      ++pos; // Skip unknown character
      continue;
    }
    
    if (cfg.lowercase_names) {
      std::string lower = to_lower(attr_name);
      if (lower != attr_name) {
        meta.lowercased_names = true;
        attr_name = lower;
      }
    }
    
    skip_whitespace(text, pos);
    
    std::string attr_value;
    if (pos < text.size() && text[pos] == '=') {
      ++pos; // Skip '='
      attr_value = parse_xml_attribute_value(text, pos, cfg, meta);
    } else {
      // Boolean attribute (HTML-style)
      attr_value = attr_name;
    }
    
    node.attributes[attr_name] = attr_value;
  }
  
  // Check for self-closing
  skip_whitespace(text, pos);
  if (pos < text.size() && text[pos] == '/') {
    node.self_closing = true;
    ++pos;
    skip_whitespace(text, pos);
    if (pos < text.size() && text[pos] == '>') ++pos;
    return node;
  }
  
  // Skip '>'
  if (pos < text.size() && text[pos] == '>') ++pos;
  
  // HTML void elements
  if (cfg.html_mode) {
    std::string lower_name = to_lower(node.name);
    if (html_void_elements.count(lower_name)) {
      node.self_closing = true;
      return node;
    }
  }
  
  // Parse children
  std::string close_tag = "</" + node.name;
  std::string close_tag_lower = "</" + to_lower(node.name);
  
  while (pos < text.size()) {
    skip_whitespace(text, pos);
    
    // Check for closing tag
    if (pos + close_tag.size() <= text.size()) {
      std::string potential = text.substr(pos, close_tag.size());
      if (potential == close_tag || to_lower(potential) == close_tag_lower) {
        // Found closing tag
        pos += close_tag.size();
        // Skip to '>'
        while (pos < text.size() && text[pos] != '>') ++pos;
        if (pos < text.size()) ++pos;
        return node;
      }
    }
    
    // Check for another closing tag (auto-close current)
    if (pos + 2 <= text.size() && text[pos] == '<' && text[pos + 1] == '/') {
      if (cfg.auto_close_tags) {
        meta.auto_closed_tags = true;
        meta.unclosed_tag_count++;
      }
      return node;
    }
    
    // Parse child
    if (pos < text.size()) {
      XmlNode child = parse_xml_node(text, pos, cfg, meta);
      if (child.type != XmlNode::Type::Text || !child.text.empty()) {
        node.children.push_back(std::move(child));
      }
    }
  }
  
  // End of input without closing tag
  if (cfg.auto_close_tags) {
    meta.auto_closed_tags = true;
    meta.unclosed_tag_count++;
  }
  
  return node;
}

static XmlNode parse_xml_impl(const std::string& text, const XmlRepairConfig& cfg, XmlRepairMetadata& meta) {
  // Create a root container
  XmlNode root;
  root.type = XmlNode::Type::Element;
  root.name = "#document";
  
  size_t pos = 0;
  while (pos < text.size()) {
    XmlNode node = parse_xml_node(text, pos, cfg, meta);
    if (node.type == XmlNode::Type::Text && node.text.empty()) continue;
    root.children.push_back(std::move(node));
  }
  
  // If there's only one element child, return it as root
  if (root.children.size() == 1 && root.children[0].type == XmlNode::Type::Element) {
    return std::move(root.children[0]);
  }
  
  return root;
}

XmlNode loads_xml(const std::string& text) {
  XmlRepairConfig cfg;
  XmlRepairMetadata meta;
  std::string candidate = extract_xml_candidate(text);
  meta.extracted_from_fence = (candidate != text);
  return parse_xml_impl(candidate, cfg, meta);
}

XmlParseResult loads_xml_ex(const std::string& text, const XmlRepairConfig& repair) {
  XmlRepairMetadata meta;
  std::string candidate = extract_xml_candidate(text);
  meta.extracted_from_fence = (candidate != text);
  XmlNode root = parse_xml_impl(candidate, repair, meta);
  return {root, candidate, meta};
}

XmlNode loads_html(const std::string& text) {
  XmlRepairConfig cfg;
  cfg.html_mode = true;
  cfg.lowercase_names = true;
  XmlRepairMetadata meta;
  std::string candidate = extract_xml_candidate(text);
  meta.extracted_from_fence = (candidate != text);
  return parse_xml_impl(candidate, cfg, meta);
}

XmlParseResult loads_html_ex(const std::string& text, const XmlRepairConfig& repair) {
  XmlRepairConfig cfg = repair;
  cfg.html_mode = true;
  XmlRepairMetadata meta;
  std::string candidate = extract_xml_candidate(text);
  meta.extracted_from_fence = (candidate != text);
  XmlNode root = parse_xml_impl(candidate, cfg, meta);
  return {root, candidate, meta};
}

Json xml_to_json(const XmlNode& node) {
  JsonObject obj;
  
  switch (node.type) {
    case XmlNode::Type::Text:
      return Json(node.text);
    case XmlNode::Type::Comment:
      obj["#comment"] = node.text;
      return Json(obj);
    case XmlNode::Type::CData:
      obj["#cdata"] = node.text;
      return Json(obj);
    case XmlNode::Type::ProcessingInstruction:
      obj["#pi"] = node.name;
      obj["#pi-data"] = node.text;
      return Json(obj);
    case XmlNode::Type::Doctype:
      obj["#doctype"] = node.text;
      return Json(obj);
    case XmlNode::Type::Element:
      break;
  }
  
  obj["#name"] = node.name;
  
  if (!node.attributes.empty()) {
    JsonObject attrs;
    for (const auto& kv : node.attributes) {
      attrs[kv.first] = kv.second;
    }
    obj["@"] = Json(attrs);
  }
  
  if (!node.children.empty()) {
    // Check if all children are text
    bool all_text = true;
    std::string text_content;
    for (const auto& child : node.children) {
      if (child.type == XmlNode::Type::Text) {
        text_content += child.text;
      } else {
        all_text = false;
        break;
      }
    }
    
    if (all_text) {
      obj["#text"] = text_content;
    } else {
      JsonArray children;
      for (const auto& child : node.children) {
        children.push_back(xml_to_json(child));
      }
      obj["#children"] = Json(children);
    }
  }
  
  return Json(obj);
}

Json loads_xml_as_json(const std::string& text) {
  XmlNode node = loads_xml(text);
  return xml_to_json(node);
}

Json loads_html_as_json(const std::string& text) {
  XmlNode node = loads_html(text);
  return xml_to_json(node);
}

static std::string xml_escape(const std::string& s) {
  std::string result;
  for (char c : s) {
    switch (c) {
      case '&': result += "&amp;"; break;
      case '<': result += "&lt;"; break;
      case '>': result += "&gt;"; break;
      case '"': result += "&quot;"; break;
      case '\'': result += "&apos;"; break;
      default: result += c; break;
    }
  }
  return result;
}

static void dumps_xml_impl(const XmlNode& node, int indent, int level, std::string& output, bool is_html) {
  std::string ind(level * indent, ' ');
  
  switch (node.type) {
    case XmlNode::Type::Text:
      output += xml_escape(node.text);
      return;
    case XmlNode::Type::Comment:
      output += ind + "<!--" + node.text + "-->";
      return;
    case XmlNode::Type::CData:
      output += ind + "<![CDATA[" + node.text + "]]>";
      return;
    case XmlNode::Type::ProcessingInstruction:
      output += ind + "<?" + node.name + " " + node.text + "?>";
      return;
    case XmlNode::Type::Doctype:
      output += ind + "<!DOCTYPE" + node.text + ">";
      return;
    case XmlNode::Type::Element:
      break;
  }
  
  if (node.name == "#document") {
    for (const auto& child : node.children) {
      dumps_xml_impl(child, indent, level, output, is_html);
      if (child.type == XmlNode::Type::Element) output += "\n";
    }
    return;
  }
  
  output += ind + "<" + node.name;
  
  for (const auto& kv : node.attributes) {
    output += " " + kv.first + "=\"" + xml_escape(kv.second) + "\"";
  }
  
  bool is_void = is_html && html_void_elements.count(to_lower(node.name));
  
  if (node.children.empty() || node.self_closing) {
    if (is_html && is_void) {
      output += ">";
    } else if (is_html) {
      output += "></" + node.name + ">";
    } else {
      output += "/>";
    }
    return;
  }
  
  output += ">";
  
  // Check if only text children
  bool only_text = true;
  for (const auto& child : node.children) {
    if (child.type != XmlNode::Type::Text) {
      only_text = false;
      break;
    }
  }
  
  if (only_text) {
    for (const auto& child : node.children) {
      output += xml_escape(child.text);
    }
  } else {
    output += "\n";
    for (const auto& child : node.children) {
      dumps_xml_impl(child, indent, level + 1, output, is_html);
      if (child.type != XmlNode::Type::Text) output += "\n";
    }
    output += ind;
  }
  
  output += "</" + node.name + ">";
}

std::string dumps_xml(const XmlNode& node, int indent) {
  std::string output;
  dumps_xml_impl(node, indent, 0, output, false);
  return output;
}

std::string dumps_html(const XmlNode& node, int indent) {
  std::string output;
  dumps_xml_impl(node, indent, 0, output, true);
  return output;
}

std::string xml_text_content(const XmlNode& node) {
  std::string result;
  
  if (node.type == XmlNode::Type::Text || node.type == XmlNode::Type::CData) {
    return node.text;
  }
  
  for (const auto& child : node.children) {
    result += xml_text_content(child);
  }
  
  return result;
}

std::string xml_get_attribute(const XmlNode& node, const std::string& name) {
  auto it = node.attributes.find(name);
  if (it != node.attributes.end()) {
    return it->second;
  }
  // Try lowercase
  std::string lower = to_lower(name);
  it = node.attributes.find(lower);
  if (it != node.attributes.end()) {
    return it->second;
  }
  return "";
}

// Simple selector query (supports tag names, #id, .class)
static void query_xml_impl(XmlNode& node, const std::string& selector, std::vector<XmlNode*>& results) {
  if (node.type != XmlNode::Type::Element) return;
  
  bool match = false;
  
  if (selector.empty()) {
    match = true;
  } else if (selector[0] == '#') {
    // ID selector
    std::string id = selector.substr(1);
    match = (xml_get_attribute(node, "id") == id);
  } else if (selector[0] == '.') {
    // Class selector
    std::string cls = selector.substr(1);
    std::string classes = xml_get_attribute(node, "class");
    // Simple contains check
    match = (classes.find(cls) != std::string::npos);
  } else {
    // Tag name selector
    match = (node.name == selector || to_lower(node.name) == to_lower(selector));
  }
  
  if (match) {
    results.push_back(&node);
  }
  
  for (auto& child : node.children) {
    query_xml_impl(child, selector, results);
  }
}

std::vector<XmlNode*> query_xml(XmlNode& root, const std::string& selector) {
  std::vector<XmlNode*> results;
  query_xml_impl(root, selector, results);
  return results;
}

static void query_xml_impl_const(const XmlNode& node, const std::string& selector, std::vector<const XmlNode*>& results) {
  if (node.type != XmlNode::Type::Element) return;
  
  bool match = false;
  
  if (selector.empty()) {
    match = true;
  } else if (selector[0] == '#') {
    std::string id = selector.substr(1);
    match = (xml_get_attribute(node, "id") == id);
  } else if (selector[0] == '.') {
    std::string cls = selector.substr(1);
    std::string classes = xml_get_attribute(node, "class");
    match = (classes.find(cls) != std::string::npos);
  } else {
    match = (node.name == selector || to_lower(node.name) == to_lower(selector));
  }
  
  if (match) {
    results.push_back(&node);
  }
  
  for (const auto& child : node.children) {
    query_xml_impl_const(child, selector, results);
  }
}

std::vector<const XmlNode*> query_xml(const XmlNode& root, const std::string& selector) {
  std::vector<const XmlNode*> results;
  query_xml_impl_const(root, selector, results);
  return results;
}

void validate_xml(const XmlNode& node, const Json& schema, const std::string& path) {
  if (!schema.is_object()) return;
  const auto& s = schema.as_object();
  
  // Validate element name
  auto it = s.find("element");
  if (it != s.end() && it->second.is_string()) {
    if (node.name != it->second.as_string() && to_lower(node.name) != to_lower(it->second.as_string())) {
      throw ValidationError("Expected element '" + it->second.as_string() + "' but got '" + node.name + "'", path, "schema");
    }
  }
  
  // Validate required attributes
  it = s.find("requiredAttributes");
  if (it != s.end() && it->second.is_array()) {
    for (const auto& attr : it->second.as_array()) {
      if (attr.is_string()) {
        if (node.attributes.find(attr.as_string()) == node.attributes.end()) {
          throw ValidationError("Missing required attribute '" + attr.as_string() + "'", path, "schema");
        }
      }
    }
  }
  
  // Validate attribute values
  it = s.find("attributes");
  if (it != s.end() && it->second.is_object()) {
    for (const auto& kv : it->second.as_object()) {
      auto attr_it = node.attributes.find(kv.first);
      if (attr_it != node.attributes.end() && kv.second.is_object()) {
        const auto& attr_schema = kv.second.as_object();
        // Pattern validation
        auto pattern_it = attr_schema.find("pattern");
        if (pattern_it != attr_schema.end() && pattern_it->second.is_string()) {
          std::regex re(pattern_it->second.as_string());
          if (!std::regex_match(attr_it->second, re)) {
            throw ValidationError("Attribute '" + kv.first + "' does not match pattern", path + "/@" + kv.first, "schema");
          }
        }
        // Enum validation
        auto enum_it = attr_schema.find("enum");
        if (enum_it != attr_schema.end() && enum_it->second.is_array()) {
          bool found = false;
          for (const auto& val : enum_it->second.as_array()) {
            if (val.is_string() && val.as_string() == attr_it->second) {
              found = true;
              break;
            }
          }
          if (!found) {
            throw ValidationError("Attribute '" + kv.first + "' value not in allowed enum", path + "/@" + kv.first, "schema");
          }
        }
      }
    }
  }
  
  // Validate children
  it = s.find("children");
  if (it != s.end() && it->second.is_object()) {
    const auto& children_schema = it->second.as_object();
    
    // Min/max children
    auto min_it = children_schema.find("minItems");
    if (min_it != children_schema.end() && min_it->second.is_number()) {
      size_t element_count = 0;
      for (const auto& child : node.children) {
        if (child.type == XmlNode::Type::Element) element_count++;
      }
      if (element_count < static_cast<size_t>(min_it->second.as_number())) {
        throw ValidationError("Too few child elements", path, "limit");
      }
    }
    
    auto max_it = children_schema.find("maxItems");
    if (max_it != children_schema.end() && max_it->second.is_number()) {
      size_t element_count = 0;
      for (const auto& child : node.children) {
        if (child.type == XmlNode::Type::Element) element_count++;
      }
      if (element_count > static_cast<size_t>(max_it->second.as_number())) {
        throw ValidationError("Too many child elements", path, "limit");
      }
    }
    
    // Required children
    auto required_it = children_schema.find("required");
    if (required_it != children_schema.end() && required_it->second.is_array()) {
      for (const auto& req : required_it->second.as_array()) {
        if (!req.is_string()) continue;
        bool found = false;
        for (const auto& child : node.children) {
          if (child.type == XmlNode::Type::Element && 
              (child.name == req.as_string() || to_lower(child.name) == to_lower(req.as_string()))) {
            found = true;
            break;
          }
        }
        if (!found) {
          throw ValidationError("Missing required child element '" + req.as_string() + "'", path, "schema");
        }
      }
    }
  }
  
  // Recursively validate child elements
  it = s.find("childSchema");
  if (it != s.end() && it->second.is_object()) {
    size_t idx = 0;
    for (const auto& child : node.children) {
      if (child.type == XmlNode::Type::Element) {
        validate_xml(child, it->second, path + "/" + child.name + "[" + std::to_string(idx) + "]");
        idx++;
      }
    }
  }
}

XmlNode parse_and_validate_xml(const std::string& text, const Json& schema) {
  XmlNode node = loads_xml(text);
  validate_xml(node, schema, "$");
  return node;
}

XmlParseResult parse_and_validate_xml_ex(const std::string& text, const Json& schema, const XmlRepairConfig& repair) {
  auto result = loads_xml_ex(text, repair);
  validate_xml(result.root, schema, "$");
  return result;
}

// ---------------- SQL extraction/parsing/validation ----------------

static std::string strip_sql_strings_and_comments(const std::string& sql, bool& has_comments) {
  std::string out;
  out.reserve(sql.size());

  bool in_s = false;
  bool in_d = false;
  bool in_line_comment = false;
  bool in_block_comment = false;

  for (size_t i = 0; i < sql.size(); ++i) {
    char c = sql[i];
    char n = (i + 1 < sql.size()) ? sql[i + 1] : '\0';

    if (in_line_comment) {
      if (c == '\n') {
        in_line_comment = false;
        out.push_back(' ');
      }
      continue;
    }
    if (in_block_comment) {
      if (c == '*' && n == '/') {
        in_block_comment = false;
        ++i;
        out.push_back(' ');
      }
      continue;
    }

    if (!in_s && !in_d) {
      if (c == '-' && n == '-') {
        has_comments = true;
        in_line_comment = true;
        ++i;
        continue;
      }
      if (c == '/' && n == '*') {
        has_comments = true;
        in_block_comment = true;
        ++i;
        continue;
      }
    }

    if (!in_d && c == '\'' ) {
      in_s = !in_s;
      out.push_back(' ');
      continue;
    }
    if (!in_s && c == '"') {
      in_d = !in_d;
      out.push_back(' ');
      continue;
    }

    if (in_s || in_d) {
      out.push_back(' ');
    } else {
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
  }

  return out;
}

static std::vector<std::string> tokenize_sql_identifiers(const std::string& sql_lower_no_strings) {
  // Very lightweight tokenization.
  std::vector<std::string> tokens;
  std::string cur;
  for (char c : sql_lower_no_strings) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.') {
      cur.push_back(c);
    } else {
      if (!cur.empty()) {
        tokens.push_back(cur);
        cur.clear();
      }
    }
  }
  if (!cur.empty()) tokens.push_back(cur);
  return tokens;
}

static std::optional<std::string> try_extract_sql_statement(const std::string& text) {
  // 1) ```sql fenced
  {
    auto lines = split_lines(text);
    bool in = false;
    std::ostringstream body;
    for (size_t idx = 0; idx < lines.size(); ++idx) {
      std::string line = ltrim_copy(lines[idx]);
      std::string low = to_lower(line);
      if (!in) {
        if (low.rfind("```sql", 0) == 0) {
          in = true;
          body.str("");
          body.clear();
        }
      } else {
        if (low.rfind("```", 0) == 0) {
          std::string out = body.str();
          if (!out.empty() && out.back() == '\n') out.pop_back();
          return out;
        }
        body << lines[idx] << "\n";
      }
    }
    if (in) return std::nullopt;
  }

  // 2) first statement terminated by ';' outside strings/comments
  bool in_s = false;
  bool in_d = false;
  bool in_line_comment = false;
  bool in_block_comment = false;

  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    char n = (i + 1 < text.size()) ? text[i + 1] : '\0';

    if (in_line_comment) {
      if (c == '\n') in_line_comment = false;
      continue;
    }
    if (in_block_comment) {
      if (c == '*' && n == '/') {
        in_block_comment = false;
        ++i;
      }
      continue;
    }

    if (!in_s && !in_d) {
      if (c == '-' && n == '-') {
        in_line_comment = true;
        ++i;
        continue;
      }
      if (c == '/' && n == '*') {
        in_block_comment = true;
        ++i;
        continue;
      }
    }

    if (!in_d && c == '\'' && !(i > 0 && text[i - 1] == '\\')) {
      in_s = !in_s;
      continue;
    }
    if (!in_s && c == '"' && !(i > 0 && text[i - 1] == '\\')) {
      in_d = !in_d;
      continue;
    }

    if (!in_s && !in_d && c == ';') {
      std::string stmt = text.substr(0, i);
      if (!stmt.empty() && stmt.back() == '\r') stmt.pop_back();
      return stmt;
    }
  }

  return std::nullopt;
}

std::string extract_sql_candidate(const std::string& text) {
  // ```sql fenced (scan lines; MSVC std::regex doesn't support (?is) flags)
  {
    auto lines = split_lines(text);
    bool in = false;
    std::ostringstream body;
    for (size_t idx = 0; idx < lines.size(); ++idx) {
      std::string line = ltrim_copy(lines[idx]);
      std::string low = to_lower(line);
      if (!in) {
        if (low.rfind("```sql", 0) == 0) {
          in = true;
          body.str("");
          body.clear();
        }
      } else {
        if (low.rfind("```", 0) == 0) {
          std::string out = body.str();
          if (!out.empty() && out.back() == '\n') out.pop_back();
          return out;
        }
        body << lines[idx] << "\n";
      }
    }
  }
  // fallback: whole text
  return text;
}

static SqlParsed parse_sql_statement_only(const std::string& sql_statement) {
  SqlParsed out;
  out.sql = sql_statement;

  bool has_comments = false;
  std::string lowered = strip_sql_strings_and_comments(out.sql, has_comments);
  out.hasComments = has_comments;

  // statement type = first token
  auto tokens = tokenize_sql_identifiers(lowered);
  out.statementType = tokens.empty() ? "" : tokens[0];

  out.hasWhere = (lowered.find(" where ") != std::string::npos) || std::regex_search(lowered, std::regex(R"(^\s*where\b)", std::regex::icase));
  out.hasFrom = (lowered.find(" from ") != std::string::npos);
  out.hasUnion = (lowered.find(" union ") != std::string::npos);
  out.hasSubquery = std::regex_search(lowered, std::regex(R"(\(\s*select\b)", std::regex::icase));

  // limit
  std::smatch m;
  if (std::regex_search(lowered, m, std::regex(R"(\blimit\s+(\d+))", std::regex::icase))) {
    out.hasLimit = true;
    out.limit = std::stoi(m[1].str());
  }

  // tables: after from/join
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i] == "from" || tokens[i] == "join" || tokens[i] == "inner" || tokens[i] == "left" || tokens[i] == "right") {
      // allow "left join" pattern
      if (tokens[i] == "left" || tokens[i] == "right" || tokens[i] == "inner") {
        if (i + 1 < tokens.size() && tokens[i + 1] == "join") {
          i++;
        } else {
          continue;
        }
      }
      if (i + 1 < tokens.size()) {
        std::string t = tokens[i + 1];
        // strip schema if present
        auto dot = t.find('.');
        if (dot != std::string::npos) {
          t = t.substr(dot + 1);
        }
        if (!t.empty()) out.tables.push_back(t);
      }
    }
  }

  return out;
}

SqlParsed parse_sql(const std::string& text) {
  return parse_sql_statement_only(extract_sql_candidate(text));
}

static std::vector<std::string> json_string_list2(const JsonObject& o, const std::string& key) {
  return json_string_list(o, key);
}

static bool list_contains_ci(const std::vector<std::string>& items, const std::string& s) {
  auto sl = to_lower(s);
  for (const auto& it : items) {
    if (to_lower(it) == sl) return true;
  }
  return false;
}

struct SqlAnalysis {
  std::map<std::string, std::string> alias_to_table;  // includes table->table
  std::set<std::string> join_types;                   // left/right/inner/full/cross/join
  size_t join_count{0};
  std::set<std::string> called_functions;
  std::vector<std::pair<std::string, std::string>> qualified_columns;  // (table, col)
  std::set<std::string> unqualified_columns;
  bool has_qmark_placeholders{false};
  bool has_dollar_placeholders{false};
  bool has_or_true_pattern{false};
};

static bool is_sql_reserved_word(const std::string& t) {
  static const std::set<std::string> k{
      "select", "from", "where", "join", "inner", "left", "right", "full", "cross", "on", "group", "order", "by",
      "having", "limit", "offset", "union", "all", "distinct", "as", "and", "or", "not", "null", "is", "in", "like",
      "between", "case", "when", "then", "else", "end", "asc", "desc"};
  return k.find(t) != k.end();
}

static SqlAnalysis analyze_sql_safety(const std::string& lowered_no_strings) {
  SqlAnalysis a;

  auto tokens = tokenize_sql_identifiers(lowered_no_strings);

  auto normalize_table = [](std::string t) {
    auto dot = t.find('.');
    if (dot != std::string::npos) t = t.substr(dot + 1);
    return t;
  };

  // Alias mapping and join analysis.
  for (size_t i = 0; i < tokens.size(); ++i) {
    // Join types
    if (tokens[i] == "join") {
      ++a.join_count;
      std::string jt = "join";
      if (i > 0) {
        const std::string& prev = tokens[i - 1];
        if (prev == "left" || prev == "right" || prev == "inner" || prev == "full" || prev == "cross") {
          jt = prev;
        }
      }
      a.join_types.insert(jt);
    }

    bool is_from_or_join = (tokens[i] == "from" || tokens[i] == "join");
    if (!is_from_or_join) {
      if (tokens[i] == "left" || tokens[i] == "right" || tokens[i] == "inner" || tokens[i] == "full" || tokens[i] == "cross") {
        if (i + 1 < tokens.size() && tokens[i + 1] == "join") {
          is_from_or_join = true;
          i += 1;  // skip to join
        }
      }
    }

    if (!is_from_or_join) continue;
    if (i + 1 >= tokens.size()) continue;
    std::string table = normalize_table(tokens[i + 1]);
    if (table.empty()) continue;
    a.alias_to_table[table] = table;

    // Optional alias: FROM table [AS] alias
    size_t j = i + 2;
    if (j < tokens.size() && tokens[j] == "as") ++j;
    if (j < tokens.size()) {
      const std::string& alias = tokens[j];
      if (!alias.empty() && !is_sql_reserved_word(alias)) {
        a.alias_to_table[alias] = table;
      }
    }
  }

  // Function calls (best-effort)
  {
    std::smatch m;
    std::regex call_re(R"(\b([a-z_][a-z0-9_]*)\s*\()", std::regex::icase);
    auto begin = lowered_no_strings.cbegin();
    while (std::regex_search(begin, lowered_no_strings.cend(), m, call_re)) {
      std::string fn = to_lower(m[1].str());
      if (!is_sql_reserved_word(fn)) {
        a.called_functions.insert(fn);
      }
      begin = m.suffix().first;
    }
  }

  // Qualified column references: alias.col
  {
    std::smatch m;
    std::regex col_re(R"(\b([a-z_][a-z0-9_]*)\s*\.\s*([a-z_][a-z0-9_]*)\b)", std::regex::icase);
    auto begin = lowered_no_strings.cbegin();
    while (std::regex_search(begin, lowered_no_strings.cend(), m, col_re)) {
      std::string lhs = to_lower(m[1].str());
      std::string col = to_lower(m[2].str());
      auto it = a.alias_to_table.find(lhs);
      if (it != a.alias_to_table.end()) {
        a.qualified_columns.emplace_back(it->second, col);
      }
      begin = m.suffix().first;
    }
  }

  // Unqualified columns (very heuristic): in SELECT list and WHERE clause.
  auto extract_clause = [&](const std::string& re) -> std::string {
    std::smatch m;
    if (std::regex_search(lowered_no_strings, m, std::regex(re, std::regex::icase))) {
      if (m.size() >= 2) return m[1].str();
    }
    return "";
  };
  std::string select_part = extract_clause(R"(\bselect\b(.*?)(\bfrom\b|$))");
  std::string where_part = extract_clause(R"(\bwhere\b(.*?)(\border\s+by\b|\blimit\b|$))");

  auto scan_unqualified = [&](const std::string& part) {
    if (part.empty()) return;
    std::smatch m;
    std::regex r(R"(\b([a-z_][a-z0-9_]*)\b\s*(=|<>|!=|<=|>=|<|>|\blike\b|\bin\b|\bis\b))", std::regex::icase);
    auto begin = part.cbegin();
    while (std::regex_search(begin, part.cend(), m, r)) {
      std::string col = to_lower(m[1].str());
      if (!is_sql_reserved_word(col)) a.unqualified_columns.insert(col);
      begin = m.suffix().first;
    }
  };
  scan_unqualified(select_part);
  scan_unqualified(where_part);

  // Placeholder style
  a.has_qmark_placeholders = (lowered_no_strings.find('?') != std::string::npos);
  a.has_dollar_placeholders = std::regex_search(lowered_no_strings, std::regex(R"(\$\d+)", std::regex::icase));

  // OR-true patterns
  a.has_or_true_pattern =
      std::regex_search(lowered_no_strings, std::regex(R"(\bor\b\s*1\s*=\s*1\b)", std::regex::icase)) ||
      std::regex_search(lowered_no_strings, std::regex(R"(\bor\b\s*true\b)", std::regex::icase));

  return a;
}

void validate_sql(const SqlParsed& parsed, const Json& schema) {
  const auto& sch = require_object_schema(schema, "$");

  bool has_comments = parsed.hasComments;
  bool dummy = false;
  std::string lowered = strip_sql_strings_and_comments(parsed.sql, dummy);
  SqlAnalysis analysis = analyze_sql_safety(lowered);

  // forbid comments
  if (json_bool(sch, "forbidComments", false) && has_comments) {
    throw ValidationError("SQL comments forbidden", "$.comments");
  }

  // forbid semicolon
  if (json_bool(sch, "forbidSemicolon", false)) {
    if (parsed.sql.find(';') != std::string::npos) {
      throw ValidationError("SQL semicolon forbidden", "$.semicolon");
    }
  }

  // allowed statements
  {
    auto allowed = json_string_list2(sch, "allowedStatements");
    if (!allowed.empty()) {
      if (!list_contains_ci(allowed, parsed.statementType)) {
        throw ValidationError("statement type not allowed: " + parsed.statementType, "$.statementType");
      }
    }
  }

  // forbid keywords
  {
    auto forbid = json_string_list2(sch, "forbidKeywords");
    for (const auto& kw : forbid) {
      std::regex r("\\b" + kw + "\\b", std::regex::icase);
      if (std::regex_search(lowered, r)) {
        throw ValidationError("forbidden keyword: " + kw, "$.keywords[" + kw + "]");
      }
    }
  }

  if (json_bool(sch, "requireFrom", false) && !parsed.hasFrom) throw ValidationError("FROM required", "$.from");
  if (json_bool(sch, "requireWhere", false) && !parsed.hasWhere) throw ValidationError("WHERE required", "$.where");
  if (json_bool(sch, "requireLimit", false) && !parsed.hasLimit) throw ValidationError("LIMIT required", "$.limit");

  if (json_bool(sch, "forbidUnion", false) && parsed.hasUnion) throw ValidationError("UNION forbidden", "$.union");
  if (json_bool(sch, "forbidSubqueries", false) && parsed.hasSubquery) throw ValidationError("subqueries forbidden", "$.subquery");

  // maxLimit
  if (auto mx = json_num_opt(sch, "maxLimit")) {
    if (parsed.limit && *parsed.limit > static_cast<int>(*mx)) throw ValidationError("LIMIT exceeds maxLimit", "$.limit");
  }

  // forbid select *
  if (json_bool(sch, "forbidSelectStar", false)) {
    if (std::regex_search(lowered, std::regex(R"(\bselect\s*\*)", std::regex::icase))) {
      throw ValidationError("SELECT * forbidden", "$.selectStar");
    }
  }

  // forbid schemas
  {
    auto forbidSchemas = json_string_list2(sch, "forbidSchemas");
    if (!forbidSchemas.empty()) {
      for (const auto& tok : tokenize_sql_identifiers(lowered)) {
        auto dot = tok.find('.');
        if (dot != std::string::npos) {
          std::string schema_name = tok.substr(0, dot);
          if (list_contains_ci(forbidSchemas, schema_name)) {
            throw ValidationError("schema forbidden: " + schema_name, "$.schema[" + schema_name + "]");
          }
        }
      }
    }
  }

  // forbid cross join
  if (json_bool(sch, "forbidCrossJoin", false)) {
    if (std::regex_search(lowered, std::regex(R"(\bcross\s+join\b)", std::regex::icase))) {
      throw ValidationError("CROSS JOIN forbidden", "$.joins.cross");
    }
  }

  // maxJoins / allowedJoinTypes
  {
    if (auto mx = json_num_opt(sch, "maxJoins")) {
      if (analysis.join_count > static_cast<size_t>(*mx)) {
        throw ValidationError("JOIN count exceeds maxJoins", "$.joins.count");
      }
    }

    auto allowed = json_string_list2(sch, "allowedJoinTypes");
    if (!allowed.empty()) {
      for (const auto& jt : analysis.join_types) {
        if (!list_contains_ci(allowed, jt)) {
          throw ValidationError("JOIN type not allowed: " + jt, "$.joins.types[" + jt + "]");
        }
      }
    }
  }

  // forbid OR-true patterns
  if (json_bool(sch, "forbidOrTrue", false) && analysis.has_or_true_pattern) {
    throw ValidationError("OR-true pattern forbidden", "$.where.orTrue");
  }

  // placeholder style enforcement
  {
    auto it = sch.find("placeholderStyle");
    if (it != sch.end() && it->second.is_string()) {
      std::string style = to_lower(it->second.as_string());
      if (style == "qmark") {
        if (analysis.has_dollar_placeholders) {
          throw ValidationError("dollar placeholders forbidden (expected ?)", "$.placeholders");
        }
      } else if (style == "dollar") {
        if (analysis.has_qmark_placeholders) {
          throw ValidationError("qmark placeholders forbidden (expected $1)", "$.placeholders");
        }
      } else if (style == "either") {
        // ok
      }
    }
  }

  // forbidFunctions
  {
    auto it = sch.find("forbidFunctions");
    if (it != sch.end()) {
      if (it->second.is_bool()) {
        if (it->second.as_bool() && !analysis.called_functions.empty()) {
          throw ValidationError("function calls forbidden", "$.functions");
        }
      } else {
        auto forbid = json_string_list2(sch, "forbidFunctions");
        if (!forbid.empty()) {
          for (const auto& fn : analysis.called_functions) {
            if (list_contains_ci(forbid, fn)) {
              throw ValidationError("function forbidden: " + fn, "$.functions[" + fn + "]");
            }
          }
        }
      }
    }
  }

  // forbid select without limit
  if (json_bool(sch, "forbidSelectWithoutLimit", false)) {
    if (to_lower(parsed.statementType) == "select" && !parsed.hasLimit) {
      throw ValidationError("SELECT without LIMIT forbidden", "$.limit");
    }
  }

  // require order by
  if (json_bool(sch, "requireOrderBy", false)) {
    if (!std::regex_search(lowered, std::regex(R"(\border\s+by\b)", std::regex::icase))) {
      throw ValidationError("ORDER BY required", "$.orderBy");
    }
  }

  // allowed tables
  {
    auto allowed = json_string_list2(sch, "allowedTables");
    if (!allowed.empty()) {
      for (const auto& t : parsed.tables) {
        if (!list_contains_ci(allowed, t)) {
          throw ValidationError("table not allowed: " + t, "$.tables[" + t + "]");
        }
      }
    }
  }

  // allowedColumns (alias-aware)
  {
    auto it = sch.find("allowedColumns");
    if (it != sch.end() && it->second.is_object()) {
      std::map<std::string, std::set<std::string>> allowed;
      for (const auto& kv : it->second.as_object()) {
        std::string table = to_lower(kv.first);
        if (!kv.second.is_array()) continue;
        std::set<std::string> cols;
        for (const auto& c : kv.second.as_array()) {
          if (c.is_string()) cols.insert(to_lower(c.as_string()));
        }
        allowed.emplace(std::move(table), std::move(cols));
      }

      // Qualified columns
      for (const auto& qc : analysis.qualified_columns) {
        const std::string table = to_lower(qc.first);
        const std::string col = to_lower(qc.second);
        auto it2 = allowed.find(table);
        if (it2 == allowed.end() || it2->second.find(col) == it2->second.end()) {
          throw ValidationError("column not allowed: " + table + "." + col, "$.columns[" + table + "." + col + "]");
        }
      }

      // Unqualified columns (best-effort)
      bool allow_unqualified = json_bool(sch, "allowUnqualifiedColumns", false);
      if (!allow_unqualified) {
        std::set<std::string> union_allowed;
        for (const auto& kv : allowed) {
          union_allowed.insert(kv.second.begin(), kv.second.end());
        }
        for (const auto& col : analysis.unqualified_columns) {
          if (union_allowed.find(col) == union_allowed.end()) {
            throw ValidationError("unqualified column not allowed: " + col, "$.columns[" + col + "]");
          }
        }
      }
    }
  }

  // forbid tables
  {
    auto forbid = json_string_list2(sch, "forbidTables");
    if (!forbid.empty()) {
      for (const auto& t : parsed.tables) {
        if (list_contains_ci(forbid, t)) {
          throw ValidationError("table forbidden: " + t, "$.tables[" + t + "]");
        }
      }
    }
  }

  // requireWhereColumns (best-effort)
  {
    auto cols = json_string_list2(sch, "requireWhereColumns");
    if (!cols.empty()) {
      // extract where clause substring (lowered)
      std::smatch m;
      std::string wherePart;
      if (std::regex_search(lowered, m, std::regex(R"(\bwhere\b(.*?)(\border\s+by\b|\blimit\b|$))", std::regex::icase))) {
        wherePart = m[1].str();
      }
      for (const auto& c : cols) {
        std::regex r("\\b" + c + "\\b", std::regex::icase);
        if (!std::regex_search(wherePart, r)) {
          throw ValidationError("WHERE must mention column: " + c, "$.where");
        }
      }
    }
  }

  // requireWherePatterns
  {
    auto it = sch.find("requireWherePatterns");
    if (it != sch.end() && it->second.is_array()) {
      std::string wherePart;
      std::smatch m;
      if (std::regex_search(lowered, m, std::regex(R"(\bwhere\b(.*?)(\border\s+by\b|\blimit\b|$))", std::regex::icase))) {
        wherePart = m[1].str();
      }
      for (const auto& pat : it->second.as_array()) {
        if (!pat.is_string()) continue;
        std::regex r(pat.as_string(), std::regex::icase);
        if (!std::regex_search(wherePart, r)) {
          throw ValidationError("WHERE does not match required pattern", "$.where");
        }
      }
    }
  }
}

SqlParsed parse_and_validate_sql(const std::string& text, const Json& schema) {
  SqlParsed p = parse_sql(text);
  validate_sql(p, schema);
  return p;
}

// ---------------- Streaming incremental parsing ----------------

static StreamLocation compute_location_from_buffer(const std::string& buf) {
  StreamLocation loc;
  loc.offset = buf.size();
  int line = 1;
  int col = 1;
  for (char c : buf) {
    if (c == '\n') {
      ++line;
      col = 1;
    } else {
      ++col;
    }
  }
  loc.line = line;
  loc.col = col;
  return loc;
}

JsonStreamParser::JsonStreamParser(Json schema) : schema_(std::move(schema)) {}

JsonStreamParser::JsonStreamParser(Json schema, size_t max_buffer_bytes)
  : schema_(std::move(schema)), max_buffer_bytes_(max_buffer_bytes) {}

void JsonStreamParser::reset() {
  buf_.clear();
  finished_ = false;
  done_ = false;
  last_ = StreamOutcome<Json>{};
}

void JsonStreamParser::finish() {
  if (done_) return;
  finished_ = true;
}

void JsonStreamParser::append(const std::string& chunk) {
  if (done_) return;
  buf_ += chunk;

  if (max_buffer_bytes_ > 0 && buf_.size() > max_buffer_bytes_) {
    done_ = true;
    last_.done = true;
    last_.ok = false;
    last_.value = std::nullopt;
    last_.error = ValidationError(
        "stream buffer exceeded maxBufferBytes (size=" + std::to_string(buf_.size()) +
            ", max=" + std::to_string(max_buffer_bytes_) + ")",
        "$.stream.maxBufferBytes",
        "limit");
  }
}

StreamLocation JsonStreamParser::location() const { return compute_location_from_buffer(buf_); }

StreamOutcome<Json> JsonStreamParser::poll() {
  if (done_) return last_;

  auto cand = try_extract_json_candidate(buf_);
  if (!cand) {
    if (finished_) {
      done_ = true;
      last_.done = true;
      last_.ok = false;
      last_.value = std::nullopt;
      last_.error = ValidationError("stream finished but JSON is incomplete", "$.stream.incomplete", "parse");
      return last_;
    }
    return StreamOutcome<Json>{false, false, std::nullopt, std::nullopt};
  }

  try {
    Json v = loads_jsonish(*cand);
    validate(v, schema_);
    done_ = true;
    last_.done = true;
    last_.ok = true;
    last_.value = v;
    last_.error = std::nullopt;
    return last_;
  } catch (const ValidationError& e) {
    done_ = true;
    last_.done = true;
    last_.ok = false;
    last_.value = std::nullopt;
    last_.error = e;
    return last_;
  } catch (const std::exception& e) {
    done_ = true;
    last_.done = true;
    last_.ok = false;
    last_.value = std::nullopt;
    last_.error = ValidationError(e.what(), "$", "parse");
    return last_;
  }
}

JsonStreamCollector::JsonStreamCollector(Json item_schema) : schema_(std::move(item_schema)) {}

JsonStreamCollector::JsonStreamCollector(Json item_schema, size_t max_buffer_bytes, size_t max_items)
  : schema_(std::move(item_schema)), max_buffer_bytes_(max_buffer_bytes), max_items_(max_items) {}

void JsonStreamCollector::reset() {
  buf_.clear();
  closed_ = false;
  done_ = false;
  items_.clear();
  last_ = StreamOutcome<JsonArray>{};
}

void JsonStreamCollector::append(const std::string& chunk) {
  if (done_ || closed_) return;
  buf_ += chunk;

  if (max_buffer_bytes_ > 0 && buf_.size() > max_buffer_bytes_) {
    done_ = true;
    last_.done = true;
    last_.ok = false;
    last_.value = std::nullopt;
    last_.error = ValidationError(
        "stream buffer exceeded maxBufferBytes (size=" + std::to_string(buf_.size()) +
            ", max=" + std::to_string(max_buffer_bytes_) + ")",
      "$.stream.maxBufferBytes",
      "limit");
  }
}

  StreamLocation JsonStreamCollector::location() const { return compute_location_from_buffer(buf_); }

void JsonStreamCollector::close() {
  if (done_) return;
  closed_ = true;
}

StreamOutcome<JsonArray> JsonStreamCollector::poll() {
  if (done_) return last_;

  // Parse as many completed JSON candidates as we can.
  for (;;) {
    auto cand = pop_next_json_candidate(buf_);
    if (!cand) break;

    try {
      Json v = loads_jsonish(*cand);
      validate(v, schema_);
      items_.push_back(v);

      if (max_items_ > 0 && items_.size() > max_items_) {
        done_ = true;
        last_.done = true;
        last_.ok = false;
        last_.value = std::nullopt;
        last_.error = ValidationError(
            "stream items exceeded maxItems (items=" + std::to_string(items_.size()) +
                ", max=" + std::to_string(max_items_) + ")",
          "$.stream.maxItems",
          "limit");
        return last_;
      }
    } catch (const ValidationError& e) {
      done_ = true;
      last_.done = true;
      last_.ok = false;
      last_.value = std::nullopt;
      last_.error = e;
      return last_;
    } catch (const std::exception& e) {
      done_ = true;
      last_.done = true;
      last_.ok = false;
      last_.value = std::nullopt;
      last_.error = ValidationError(e.what(), "$", "parse");
      return last_;
    }
  }

  if (!closed_) {
    return StreamOutcome<JsonArray>{false, false, std::nullopt, std::nullopt};
  }

  // Close = success if no error so far.
  done_ = true;
  last_.done = true;
  last_.ok = true;
  last_.value = items_;
  last_.error = std::nullopt;
  return last_;
}

JsonStreamBatchCollector::JsonStreamBatchCollector(Json item_schema) : schema_(std::move(item_schema)) {}

JsonStreamBatchCollector::JsonStreamBatchCollector(Json item_schema, size_t max_buffer_bytes, size_t max_items)
  : schema_(std::move(item_schema)), max_buffer_bytes_(max_buffer_bytes), max_items_(max_items) {}

void JsonStreamBatchCollector::reset() {
  buf_.clear();
  closed_ = false;
  done_ = false;
  emitted_items_ = 0;
  last_ = StreamOutcome<JsonArray>{};
}

void JsonStreamBatchCollector::append(const std::string& chunk) {
  if (done_ || closed_) return;
  buf_ += chunk;

  if (max_buffer_bytes_ > 0 && buf_.size() > max_buffer_bytes_) {
    done_ = true;
    last_.done = true;
    last_.ok = false;
    last_.value = std::nullopt;
    last_.error = ValidationError(
        "stream buffer exceeded maxBufferBytes (size=" + std::to_string(buf_.size()) +
            ", max=" + std::to_string(max_buffer_bytes_) + ")",
      "$.stream.maxBufferBytes",
      "limit");
  }
}

  StreamLocation JsonStreamBatchCollector::location() const { return compute_location_from_buffer(buf_); }

void JsonStreamBatchCollector::close() {
  if (done_) return;
  closed_ = true;
}

StreamOutcome<JsonArray> JsonStreamBatchCollector::poll() {
  if (done_) return last_;

  JsonArray batch;
  for (;;) {
    auto cand = pop_next_json_candidate(buf_);
    if (!cand) break;

    try {
      Json v = loads_jsonish(*cand);
      validate(v, schema_);
      batch.push_back(v);

      ++emitted_items_;
      if (max_items_ > 0 && emitted_items_ > max_items_) {
        done_ = true;
        last_.done = true;
        last_.ok = false;
        last_.value = std::nullopt;
        last_.error = ValidationError(
            "stream items exceeded maxItems (items=" + std::to_string(emitted_items_) +
                ", max=" + std::to_string(max_items_) + ")",
          "$.stream.maxItems",
          "limit");
        return last_;
      }
    } catch (const ValidationError& e) {
      done_ = true;
      last_.done = true;
      last_.ok = false;
      last_.value = std::nullopt;
      last_.error = e;
      return last_;
    } catch (const std::exception& e) {
      done_ = true;
      last_.done = true;
      last_.ok = false;
      last_.value = std::nullopt;
      last_.error = ValidationError(e.what(), "$", "parse");
      return last_;
    }
  }

  if (!batch.empty()) {
    // Emit newly parsed items even if not closed.
    last_.done = false;
    last_.ok = true;
    last_.value = batch;
    last_.error = std::nullopt;
    return last_;
  }

  if (!closed_) {
    return StreamOutcome<JsonArray>{false, false, std::nullopt, std::nullopt};
  }

  // Closed + no more complete items => done.
  done_ = true;
  last_.done = true;
  last_.ok = true;
  last_.value = JsonArray{};
  last_.error = std::nullopt;
  return last_;
}

JsonStreamValidatedBatchCollector::JsonStreamValidatedBatchCollector(Json item_schema) : schema_(std::move(item_schema)) {}

JsonStreamValidatedBatchCollector::JsonStreamValidatedBatchCollector(Json item_schema, size_t max_buffer_bytes, size_t max_items)
  : schema_(std::move(item_schema)), max_buffer_bytes_(max_buffer_bytes), max_items_(max_items) {}

void JsonStreamValidatedBatchCollector::reset() {
  buf_.clear();
  closed_ = false;
  done_ = false;
  emitted_items_ = 0;
  last_ = StreamOutcome<JsonArray>{};
}

void JsonStreamValidatedBatchCollector::append(const std::string& chunk) {
  if (done_ || closed_) return;
  buf_ += chunk;

  if (max_buffer_bytes_ > 0 && buf_.size() > max_buffer_bytes_) {
    done_ = true;
    last_.done = true;
    last_.ok = false;
    last_.value = std::nullopt;
    last_.error = ValidationError(
        "stream buffer exceeded maxBufferBytes (size=" + std::to_string(buf_.size()) +
            ", max=" + std::to_string(max_buffer_bytes_) + ")",
        "$.stream.maxBufferBytes",
        "limit");
  }
}

void JsonStreamValidatedBatchCollector::close() {
  if (done_) return;
  closed_ = true;
}

StreamOutcome<JsonArray> JsonStreamValidatedBatchCollector::poll() {
  if (done_) return last_;

  JsonArray batch;
  for (;;) {
    auto cand = pop_next_json_candidate(buf_);
    if (!cand) break;

    try {
      Json v = loads_jsonish(*cand);
      apply_defaults(v, schema_);
      validate(v, schema_);
      batch.push_back(v);

      ++emitted_items_;
      if (max_items_ > 0 && emitted_items_ > max_items_) {
        done_ = true;
        last_.done = true;
        last_.ok = false;
        last_.value = std::nullopt;
        last_.error = ValidationError(
            "stream items exceeded maxItems (items=" + std::to_string(emitted_items_) +
                ", max=" + std::to_string(max_items_) + ")",
            "$.stream.maxItems",
            "limit");
        return last_;
      }
    } catch (const ValidationError& e) {
      done_ = true;
      last_.done = true;
      last_.ok = false;
      last_.value = std::nullopt;
      last_.error = e;
      return last_;
    } catch (const std::exception& e) {
      done_ = true;
      last_.done = true;
      last_.ok = false;
      last_.value = std::nullopt;
      last_.error = ValidationError(e.what(), "$", "parse");
      return last_;
    }
  }

  if (!batch.empty()) {
    last_.done = false;
    last_.ok = true;
    last_.value = batch;
    last_.error = std::nullopt;
    return last_;
  }

  if (!closed_) {
    return StreamOutcome<JsonArray>{false, false, std::nullopt, std::nullopt};
  }

  done_ = true;
  last_.done = true;
  last_.ok = true;
  last_.value = JsonArray{};
  last_.error = std::nullopt;
  return last_;
}

StreamLocation JsonStreamValidatedBatchCollector::location() const { return compute_location_from_buffer(buf_); }

// ---------------- Schema Inference Implementation ----------------

namespace {

// Helper to detect string formats
std::string detect_string_format(const std::string& s) {
  // ISO 8601 date-time: 2024-01-15T10:30:00Z or 2024-01-15T10:30:00+08:00
  static const std::regex datetime_regex(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?(Z|[+-]\d{2}:\d{2})?$)");
  if (std::regex_match(s, datetime_regex)) return "date-time";
  
  // ISO 8601 date: 2024-01-15
  static const std::regex date_regex(R"(^\d{4}-\d{2}-\d{2}$)");
  if (std::regex_match(s, date_regex)) return "date";
  
  // ISO 8601 time: 10:30:00 or 10:30:00.123
  static const std::regex time_regex(R"(^\d{2}:\d{2}:\d{2}(\.\d+)?$)");
  if (std::regex_match(s, time_regex)) return "time";
  
  // Email (simplified)
  static const std::regex email_regex(R"(^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}$)");
  if (std::regex_match(s, email_regex)) return "email";
  
  // URI
  static const std::regex uri_regex(R"(^(https?|ftp|mailto|file|data)://[^\s]+$)");
  if (std::regex_match(s, uri_regex)) return "uri";
  
  // UUID
  static const std::regex uuid_regex(R"(^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$)");
  if (std::regex_match(s, uuid_regex)) return "uuid";
  
  // IPv4
  static const std::regex ipv4_regex(R"(^(\d{1,3}\.){3}\d{1,3}$)");
  if (std::regex_match(s, ipv4_regex)) return "ipv4";
  
  // Hostname
  static const std::regex hostname_regex(R"(^[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?(\.[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*$)");
  if (std::regex_match(s, hostname_regex) && s.find('.') != std::string::npos) return "hostname";
  
  return "";
}

// Helper to get type name from Json value
std::string get_json_type(const Json& v) {
  if (v.is_null()) return "null";
  if (v.is_bool()) return "boolean";
  if (v.is_number()) {
    double d = v.as_number();
    if (d == std::floor(d) && std::abs(d) <= 9007199254740992.0) return "integer";
    return "number";
  }
  if (v.is_string()) return "string";
  if (v.is_array()) return "array";
  if (v.is_object()) return "object";
  return "null";
}

// Forward declaration
Json infer_schema_internal(const Json& value, const SchemaInferenceConfig& config, std::map<std::string, std::set<std::string>>& enum_candidates);

Json infer_array_schema(const JsonArray& arr, const SchemaInferenceConfig& config, std::map<std::string, std::set<std::string>>& enum_candidates) {
  JsonObject schema_obj;
  schema_obj["type"] = Json("array");
  
  if (arr.empty()) {
    schema_obj["items"] = Json(JsonObject{});
    return Json(schema_obj);
  }
  
  // Infer items schema from all array elements
  Json items_schema;
  bool first = true;
  for (const auto& item : arr) {
    Json item_schema = infer_schema_internal(item, config, enum_candidates);
    if (first) {
      items_schema = item_schema;
      first = false;
    } else {
      // Merge schemas
      items_schema = merge_schemas(items_schema, item_schema, config);
    }
  }
  schema_obj["items"] = items_schema;
  
  if (config.infer_array_lengths) {
    schema_obj["minItems"] = Json(static_cast<double>(arr.size()));
    schema_obj["maxItems"] = Json(static_cast<double>(arr.size()));
  }
  
  return Json(schema_obj);
}

Json infer_object_schema(const JsonObject& obj, const SchemaInferenceConfig& config, std::map<std::string, std::set<std::string>>& enum_candidates) {
  JsonObject schema_obj;
  schema_obj["type"] = Json("object");
  
  JsonObject properties;
  JsonArray required;
  
  for (const auto& [key, value] : obj) {
    properties[key] = infer_schema_internal(value, config, enum_candidates);
    if (config.required_by_default) {
      required.push_back(Json(key));
    }
  }
  
  schema_obj["properties"] = Json(properties);
  if (!required.empty()) {
    schema_obj["required"] = Json(required);
  }
  if (config.strict_additional_properties) {
    schema_obj["additionalProperties"] = Json(false);
  }
  
  return Json(schema_obj);
}

Json infer_schema_internal(const Json& value, const SchemaInferenceConfig& config, std::map<std::string, std::set<std::string>>& enum_candidates) {
  JsonObject schema_obj;
  
  if (value.is_null()) {
    schema_obj["type"] = Json("null");
  } else if (value.is_bool()) {
    schema_obj["type"] = Json("boolean");
    if (config.include_default) {
      schema_obj["default"] = value;
    }
  } else if (value.is_number()) {
    double d = value.as_number();
    bool is_int = (d == std::floor(d) && std::abs(d) <= 9007199254740992.0);
    
    if (config.prefer_integer && is_int) {
      schema_obj["type"] = Json("integer");
    } else {
      schema_obj["type"] = Json("number");
    }
    
    if (config.include_default) {
      schema_obj["default"] = value;
    }
    if (config.infer_numeric_ranges) {
      schema_obj["minimum"] = value;
      schema_obj["maximum"] = value;
    }
  } else if (value.is_string()) {
    schema_obj["type"] = Json("string");
    const std::string& s = value.as_string();
    
    if (config.infer_formats) {
      std::string fmt = detect_string_format(s);
      if (!fmt.empty()) {
        schema_obj["format"] = Json(fmt);
      }
    }
    
    if (config.include_default) {
      schema_obj["default"] = value;
    }
    if (config.infer_string_lengths) {
      schema_obj["minLength"] = Json(static_cast<double>(s.length()));
      schema_obj["maxLength"] = Json(static_cast<double>(s.length()));
    }
    if (config.include_examples) {
      schema_obj["examples"] = Json(JsonArray{value});
    }
  } else if (value.is_array()) {
    return infer_array_schema(value.as_array(), config, enum_candidates);
  } else if (value.is_object()) {
    return infer_object_schema(value.as_object(), config, enum_candidates);
  }
  
  return Json(schema_obj);
}

// Merge two type strings or arrays
Json merge_types(const Json& t1, const Json& t2) {
  std::set<std::string> types;
  
  auto add_type = [&](const Json& t) {
    if (t.is_string()) {
      types.insert(t.as_string());
    } else if (t.is_array()) {
      for (const auto& item : t.as_array()) {
        if (item.is_string()) types.insert(item.as_string());
      }
    }
  };
  
  add_type(t1);
  add_type(t2);
  
  // integer is a subset of number
  if (types.count("integer") && types.count("number")) {
    types.erase("integer");
  }
  
  if (types.size() == 1) {
    return Json(*types.begin());
  }
  
  JsonArray arr;
  for (const auto& t : types) {
    arr.push_back(Json(t));
  }
  return Json(arr);
}

}  // anonymous namespace

Json infer_schema(const Json& value, const SchemaInferenceConfig& config) {
  std::map<std::string, std::set<std::string>> enum_candidates;
  Json schema = infer_schema_internal(value, config, enum_candidates);
  return schema;
}

Json infer_schema_from_values(const JsonArray& values, const SchemaInferenceConfig& config) {
  if (values.empty()) {
    return Json(JsonObject{});
  }
  
  if (values.size() == 1) {
    return infer_schema(values[0], config);
  }
  
  // Infer schema from first value, then merge with others
  Json schema = infer_schema(values[0], config);
  for (size_t i = 1; i < values.size(); ++i) {
    Json other = infer_schema(values[i], config);
    schema = merge_schemas(schema, other, config);
  }
  
  // Detect enums after merging
  if (config.detect_enums) {
    // Check if all values were strings and form a small set
    std::set<std::string> string_values;
    bool all_strings = true;
    for (const auto& v : values) {
      if (v.is_string()) {
        string_values.insert(v.as_string());
      } else {
        all_strings = false;
        break;
      }
    }
    
    if (all_strings && string_values.size() <= static_cast<size_t>(config.max_enum_values) && string_values.size() < values.size()) {
      // Convert to enum
      JsonArray enum_arr;
      for (const auto& s : string_values) {
        enum_arr.push_back(Json(s));
      }
      if (schema.is_object()) {
        JsonObject schema_obj = schema.as_object();
        schema_obj["enum"] = Json(enum_arr);
        return Json(schema_obj);
      }
    }
  }
  
  return schema;
}

Json merge_schemas(const Json& schema1, const Json& schema2, const SchemaInferenceConfig& config) {
  // If either is empty, return the other
  if (!schema1.is_object() || schema1.as_object().empty()) return schema2;
  if (!schema2.is_object() || schema2.as_object().empty()) return schema1;
  
  const JsonObject& s1 = schema1.as_object();
  const JsonObject& s2 = schema2.as_object();
  
  // Get types
  Json type1 = s1.count("type") ? s1.at("type") : Json();
  Json type2 = s2.count("type") ? s2.at("type") : Json();
  
  std::string t1_str = type1.is_string() ? type1.as_string() : "";
  std::string t2_str = type2.is_string() ? type2.as_string() : "";
  
  // Same type - merge type-specific constraints
  if (t1_str == t2_str && !t1_str.empty()) {
    JsonObject res;
    res["type"] = type1;
    
    if (t1_str == "object") {
      // Merge properties
      JsonObject merged_props;
      std::set<std::string> all_keys;
      
      const JsonObject* props1 = s1.count("properties") && s1.at("properties").is_object() 
                                  ? &s1.at("properties").as_object() : nullptr;
      const JsonObject* props2 = s2.count("properties") && s2.at("properties").is_object() 
                                  ? &s2.at("properties").as_object() : nullptr;
      
      if (props1) for (const auto& [k, v] : *props1) all_keys.insert(k);
      if (props2) for (const auto& [k, v] : *props2) all_keys.insert(k);
      
      for (const auto& key : all_keys) {
        bool in1 = props1 && props1->count(key);
        bool in2 = props2 && props2->count(key);
        
        if (in1 && in2) {
          merged_props[key] = merge_schemas(props1->at(key), props2->at(key), config);
        } else if (in1) {
          merged_props[key] = props1->at(key);
        } else {
          merged_props[key] = props2->at(key);
        }
      }
      res["properties"] = Json(merged_props);
      
      // Merge required - intersection if both present, otherwise keep what exists
      std::set<std::string> req1, req2;
      if (s1.count("required") && s1.at("required").is_array()) {
        for (const auto& r : s1.at("required").as_array()) {
          if (r.is_string()) req1.insert(r.as_string());
        }
      }
      if (s2.count("required") && s2.at("required").is_array()) {
        for (const auto& r : s2.at("required").as_array()) {
          if (r.is_string()) req2.insert(r.as_string());
        }
      }
      
      // Intersection of required fields
      std::set<std::string> required_intersection;
      for (const auto& r : req1) {
        if (req2.count(r)) required_intersection.insert(r);
      }
      
      if (!required_intersection.empty()) {
        JsonArray req_arr;
        for (const auto& r : required_intersection) {
          req_arr.push_back(Json(r));
        }
        res["required"] = Json(req_arr);
      }
      
      if (config.strict_additional_properties) {
        res["additionalProperties"] = Json(false);
      }
    } else if (t1_str == "array") {
      // Merge items schemas
      if (s1.count("items") && s2.count("items")) {
        res["items"] = merge_schemas(s1.at("items"), s2.at("items"), config);
      } else if (s1.count("items")) {
        res["items"] = s1.at("items");
      } else if (s2.count("items")) {
        res["items"] = s2.at("items");
      }
      
      // Merge array length constraints - take min of mins, max of maxes
      if (config.infer_array_lengths) {
        double min1 = s1.count("minItems") && s1.at("minItems").is_number() ? s1.at("minItems").as_number() : 0;
        double min2 = s2.count("minItems") && s2.at("minItems").is_number() ? s2.at("minItems").as_number() : 0;
        double max1 = s1.count("maxItems") && s1.at("maxItems").is_number() ? s1.at("maxItems").as_number() : 1e9;
        double max2 = s2.count("maxItems") && s2.at("maxItems").is_number() ? s2.at("maxItems").as_number() : 1e9;
        
        res["minItems"] = Json(std::min(min1, min2));
        res["maxItems"] = Json(std::max(max1, max2));
      }
    } else if (t1_str == "string") {
      // Merge string constraints
      if (config.infer_string_lengths) {
        double min1 = s1.count("minLength") && s1.at("minLength").is_number() ? s1.at("minLength").as_number() : 0;
        double min2 = s2.count("minLength") && s2.at("minLength").is_number() ? s2.at("minLength").as_number() : 0;
        double max1 = s1.count("maxLength") && s1.at("maxLength").is_number() ? s1.at("maxLength").as_number() : 1e9;
        double max2 = s2.count("maxLength") && s2.at("maxLength").is_number() ? s2.at("maxLength").as_number() : 1e9;
        
        res["minLength"] = Json(std::min(min1, min2));
        res["maxLength"] = Json(std::max(max1, max2));
      }
      
      // Keep format only if both have the same format
      if (s1.count("format") && s2.count("format") && 
          s1.at("format").is_string() && s2.at("format").is_string() &&
          s1.at("format").as_string() == s2.at("format").as_string()) {
        res["format"] = s1.at("format");
      }
      
      // Merge examples
      if (config.include_examples) {
        std::set<std::string> examples;
        auto add_examples = [&](const JsonObject& s) {
          if (s.count("examples") && s.at("examples").is_array()) {
            for (const auto& ex : s.at("examples").as_array()) {
              if (ex.is_string() && examples.size() < static_cast<size_t>(config.max_examples)) {
                examples.insert(ex.as_string());
              }
            }
          }
        };
        add_examples(s1);
        add_examples(s2);
        
        if (!examples.empty()) {
          JsonArray ex_arr;
          for (const auto& ex : examples) {
            ex_arr.push_back(Json(ex));
          }
          res["examples"] = Json(ex_arr);
        }
      }
    } else if (t1_str == "number" || t1_str == "integer") {
      // Merge numeric constraints
      if (config.infer_numeric_ranges) {
        double min1 = s1.count("minimum") && s1.at("minimum").is_number() ? s1.at("minimum").as_number() : -1e308;
        double min2 = s2.count("minimum") && s2.at("minimum").is_number() ? s2.at("minimum").as_number() : -1e308;
        double max1 = s1.count("maximum") && s1.at("maximum").is_number() ? s1.at("maximum").as_number() : 1e308;
        double max2 = s2.count("maximum") && s2.at("maximum").is_number() ? s2.at("maximum").as_number() : 1e308;
        
        res["minimum"] = Json(std::min(min1, min2));
        res["maximum"] = Json(std::max(max1, max2));
      }
    }
    
    return Json(res);
  }
  
  // Different types - use anyOf if allowed
  if (config.allow_any_of) {
    // Check if we can merge integer and number
    if ((t1_str == "integer" && t2_str == "number") || (t1_str == "number" && t2_str == "integer")) {
      JsonObject result;
      result["type"] = Json("number");
      return Json(result);
    }
    
    JsonObject result;
    JsonArray any_of;
    any_of.push_back(schema1);
    any_of.push_back(schema2);
    result["anyOf"] = Json(any_of);
    return Json(result);
  }
  
  // Fallback: merged type array
  JsonObject result;
  result["type"] = merge_types(type1, type2);
  return Json(result);
}

SqlStreamParser::SqlStreamParser(Json schema) : schema_(std::move(schema)) {}

SqlStreamParser::SqlStreamParser(Json schema, size_t max_buffer_bytes)
  : schema_(std::move(schema)), max_buffer_bytes_(max_buffer_bytes) {}

void SqlStreamParser::reset() {
  buf_.clear();
  finished_ = false;
  done_ = false;
  last_ = StreamOutcome<SqlParsed>{};
}

void SqlStreamParser::finish() {
  if (done_) return;
  finished_ = true;
}

void SqlStreamParser::append(const std::string& chunk) {
  if (done_) return;
  buf_ += chunk;

  if (max_buffer_bytes_ > 0 && buf_.size() > max_buffer_bytes_) {
    done_ = true;
    last_.done = true;
    last_.ok = false;
    last_.value = std::nullopt;
    last_.error = ValidationError(
        "stream buffer exceeded maxBufferBytes (size=" + std::to_string(buf_.size()) +
            ", max=" + std::to_string(max_buffer_bytes_) + ")",
      "$.stream.maxBufferBytes",
      "limit");
  }
}

  StreamLocation SqlStreamParser::location() const { return compute_location_from_buffer(buf_); }

StreamOutcome<SqlParsed> SqlStreamParser::poll() {
  if (done_) return last_;

  auto stmt = try_extract_sql_statement(buf_);
  if (!stmt) {
    if (finished_) {
      done_ = true;
      last_.done = true;
      last_.ok = false;
      last_.value = std::nullopt;
      last_.error = ValidationError("stream finished but SQL is incomplete", "$.stream.incomplete", "parse");
      return last_;
    }
    return StreamOutcome<SqlParsed>{false, false, std::nullopt, std::nullopt};
  }

  try {
    SqlParsed p = parse_sql_statement_only(*stmt);
    validate_sql(p, schema_);
    done_ = true;
    last_.done = true;
    last_.ok = true;
    last_.value = p;
    last_.error = std::nullopt;
    return last_;
  } catch (const ValidationError& e) {
    done_ = true;
    last_.done = true;
    last_.ok = false;
    last_.value = std::nullopt;
    last_.error = e;
    return last_;
  } catch (const std::exception& e) {
    done_ = true;
    last_.done = true;
    last_.ok = false;
    last_.value = std::nullopt;
    last_.error = ValidationError(e.what(), "$", "parse");
    return last_;
  }
}

}  // namespace llm_structured
