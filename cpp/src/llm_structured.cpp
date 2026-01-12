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
