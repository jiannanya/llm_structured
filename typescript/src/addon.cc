#include <node_api.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <utility>

#include "llm_structured.hpp"

using llm_structured::Json;
using llm_structured::JsonObject;
using llm_structured::SqlParsed;
using llm_structured::ValidationError;

static void ThrowTypeError(napi_env env, const char* msg) { napi_throw_type_error(env, nullptr, msg); }

static napi_value MakeString(napi_env env, const std::string& s);

static bool TryParseStreamLimit(const ValidationError& e, std::string& kind, double& current, double& max) {
  if (e.path == "$.stream.maxBufferBytes") {
    kind = "maxBufferBytes";
  } else if (e.path == "$.stream.maxItems") {
    kind = "maxItems";
  } else {
    return false;
  }

  const std::string msg = e.what();
  const char* cur_key = (kind == "maxBufferBytes") ? "size=" : "items=";
  const auto cur_pos = msg.find(cur_key);
  const auto max_pos = msg.find("max=");
  if (cur_pos == std::string::npos || max_pos == std::string::npos) return false;

  try {
    const auto cur_start = cur_pos + std::strlen(cur_key);
    const auto cur_end = msg.find_first_of(",)", cur_start);
    const auto max_start = max_pos + std::strlen("max=");
    const auto max_end = msg.find_first_of(")", max_start);
    current = std::stod(msg.substr(cur_start, cur_end - cur_start));
    max = std::stod(msg.substr(max_start, max_end - max_start));
    return true;
  } catch (...) {
    return false;
  }
}

static void ThrowError(napi_env env, const std::string& msg) {
  napi_throw_error(env, nullptr, msg.c_str());
}

static void ThrowErrorWithKind(napi_env env, const std::string& msg, const std::string& kind) {
  napi_value message;
  napi_create_string_utf8(env, msg.c_str(), msg.size(), &message);

  napi_value err;
  napi_create_error(env, nullptr, message, &err);
  napi_set_named_property(env, err, "message", message);
  napi_set_named_property(env, err, "kind", MakeString(env, kind));
  napi_throw(env, err);
}

static void ThrowValidationError(napi_env env, const ValidationError& e) {
  napi_value msg;
  napi_create_string_utf8(env, e.what(), NAPI_AUTO_LENGTH, &msg);

  napi_value err;
  napi_create_error(env, nullptr, msg, &err);

  // Ensure `err.message` is present and readable.
  napi_set_named_property(env, err, "message", msg);

  napi_value name;
  napi_create_string_utf8(env, "ValidationError", NAPI_AUTO_LENGTH, &name);
  napi_set_named_property(env, err, "name", name);

  napi_value path;
  napi_create_string_utf8(env, e.path.c_str(), e.path.size(), &path);
  napi_set_named_property(env, err, "path", path);

  napi_value ptr;
  auto jp = llm_structured::json_pointer_from_path(e.path);
  napi_create_string_utf8(env, jp.c_str(), jp.size(), &ptr);
  napi_set_named_property(env, err, "jsonPointer", ptr);

  napi_set_named_property(env, err, "kind", MakeString(env, e.kind));

  std::string kind;
  double current = 0;
  double max = 0;
  if (TryParseStreamLimit(e, kind, current, max)) {
    napi_value lim;
    napi_create_object(env, &lim);
    napi_set_named_property(env, lim, "kind", MakeString(env, kind));
    napi_value n;
    napi_create_double(env, current, &n);
    napi_set_named_property(env, lim, "current", n);
    napi_create_double(env, max, &n);
    napi_set_named_property(env, lim, "max", n);
    napi_set_named_property(env, err, "limit", lim);
  }

  napi_throw(env, err);
}

static bool GetStringUtf8(napi_env env, napi_value v, std::string& out) {
  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return false;
  if (t != napi_string) return false;

  size_t len = 0;
  if (napi_get_value_string_utf8(env, v, nullptr, 0, &len) != napi_ok) return false;

  out.resize(len);
  size_t written = 0;
  if (napi_get_value_string_utf8(env, v, out.data(), out.size() + 1, &written) != napi_ok) return false;
  out.resize(written);
  return true;
}

static bool GetOptionalSizeTProperty(napi_env env, napi_value obj, const char* key, size_t& out) {
  bool has = false;
  if (napi_has_named_property(env, obj, key, &has) != napi_ok) return false;
  if (!has) return true;

  napi_value v;
  if (napi_get_named_property(env, obj, key, &v) != napi_ok) return false;

  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return false;
  if (t != napi_number) {
    ThrowTypeError(env, "stream limits must be numbers");
    return false;
  }

  double d = 0;
  if (napi_get_value_double(env, v, &d) != napi_ok) return false;
  if (d < 0) {
    ThrowTypeError(env, "stream limits must be >= 0");
    return false;
  }
  out = static_cast<size_t>(d);
  return true;
}

static bool GetOptionalBoolProperty(napi_env env, napi_value obj, const char* key, bool& out) {
  bool has = false;
  if (napi_has_named_property(env, obj, key, &has) != napi_ok) return false;
  if (!has) return true;

  napi_value v;
  if (napi_get_named_property(env, obj, key, &v) != napi_ok) return false;
  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return false;
  if (t != napi_boolean) {
    ThrowTypeError(env, "repair config fields must be booleans");
    return false;
  }
  bool b = false;
  if (napi_get_value_bool(env, v, &b) != napi_ok) return false;
  out = b;
  return true;
}

static bool GetOptionalStringProperty(napi_env env, napi_value obj, const char* key, std::string& out) {
  bool has = false;
  if (napi_has_named_property(env, obj, key, &has) != napi_ok) return false;
  if (!has) return true;

  napi_value v;
  if (napi_get_named_property(env, obj, key, &v) != napi_ok) return false;

  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return false;
  if (t != napi_string) {
    ThrowTypeError(env, "repair config fields must be booleans (or duplicateKeyPolicy as string)");
    return false;
  }
  return GetStringUtf8(env, v, out);
}

static bool RepairConfigFromNapi(napi_env env, napi_value v, llm_structured::RepairConfig& out) {
  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return false;
  if (t == napi_null || t == napi_undefined) return true;
  if (t != napi_object) {
    ThrowTypeError(env, "repair config must be an object");
    return false;
  }
  if (!GetOptionalBoolProperty(env, v, "fixSmartQuotes", out.fix_smart_quotes)) return false;
  if (!GetOptionalBoolProperty(env, v, "stripJsonComments", out.strip_json_comments)) return false;
  if (!GetOptionalBoolProperty(env, v, "replacePythonLiterals", out.replace_python_literals)) return false;
  if (!GetOptionalBoolProperty(env, v, "convertKvObjectToJson", out.convert_kv_object_to_json)) return false;
  if (!GetOptionalBoolProperty(env, v, "quoteUnquotedKeys", out.quote_unquoted_keys)) return false;
  if (!GetOptionalBoolProperty(env, v, "dropTrailingCommas", out.drop_trailing_commas)) return false;
  if (!GetOptionalBoolProperty(env, v, "allowSingleQuotes", out.allow_single_quotes)) return false;

  std::string pol;
  if (!GetOptionalStringProperty(env, v, "duplicateKeyPolicy", pol)) return false;
  if (!pol.empty()) {
    std::string s = pol;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "error") {
      out.duplicate_key_policy = llm_structured::RepairConfig::DuplicateKeyPolicy::Error;
    } else if (s == "firstwins" || s == "first_wins" || s == "first") {
      out.duplicate_key_policy = llm_structured::RepairConfig::DuplicateKeyPolicy::FirstWins;
    } else if (s == "lastwins" || s == "last_wins" || s == "last") {
      out.duplicate_key_policy = llm_structured::RepairConfig::DuplicateKeyPolicy::LastWins;
    } else {
      ThrowTypeError(env, "duplicateKeyPolicy must be one of: error | firstWins | lastWins");
      return false;
    }
  }
  return true;
}

static napi_value RepairMetadataToNapi(napi_env env, const llm_structured::RepairMetadata& m) {
  napi_value obj;
  napi_create_object(env, &obj);
  napi_value b;
  napi_get_boolean(env, m.extracted_from_fence, &b);
  napi_set_named_property(env, obj, "extractedFromFence", b);
  napi_get_boolean(env, m.fixed_smart_quotes, &b);
  napi_set_named_property(env, obj, "fixedSmartQuotes", b);
  napi_get_boolean(env, m.stripped_comments, &b);
  napi_set_named_property(env, obj, "strippedComments", b);
  napi_get_boolean(env, m.replaced_python_literals, &b);
  napi_set_named_property(env, obj, "replacedPythonLiterals", b);
  napi_get_boolean(env, m.converted_kv_object, &b);
  napi_set_named_property(env, obj, "convertedKvObject", b);
  napi_get_boolean(env, m.quoted_unquoted_keys, &b);
  napi_set_named_property(env, obj, "quotedUnquotedKeys", b);
  napi_get_boolean(env, m.dropped_trailing_commas, &b);
  napi_set_named_property(env, obj, "droppedTrailingCommas", b);

  napi_value n;
  napi_create_int32(env, m.duplicateKeyCount, &n);
  napi_set_named_property(env, obj, "duplicateKeyCount", n);

  const auto pol = m.duplicateKeyPolicy;
  std::string pols = "firstWins";
  if (pol == llm_structured::RepairConfig::DuplicateKeyPolicy::Error) pols = "error";
  if (pol == llm_structured::RepairConfig::DuplicateKeyPolicy::LastWins) pols = "lastWins";
  napi_set_named_property(env, obj, "duplicateKeyPolicy", MakeString(env, pols));
  return obj;
}

static napi_value StreamLocationToNapi(napi_env env, const llm_structured::StreamLocation& loc) {
  napi_value obj;
  napi_create_object(env, &obj);
  napi_value n;
  napi_create_double(env, static_cast<double>(loc.offset), &n);
  napi_set_named_property(env, obj, "offset", n);
  napi_create_int32(env, loc.line, &n);
  napi_set_named_property(env, obj, "line", n);
  napi_create_int32(env, loc.col, &n);
  napi_set_named_property(env, obj, "col", n);
  return obj;
}

static napi_value MakeString(napi_env env, const std::string& s) {
  napi_value out;
  napi_create_string_utf8(env, s.c_str(), s.size(), &out);
  return out;
}

static napi_value ToNapi(napi_env env, const Json& v);

static bool FromNapi(napi_env env, napi_value v, Json& out);

static bool FromNapiObject(napi_env env, napi_value v, Json& out) {
  napi_value names;
  if (napi_get_property_names(env, v, &names) != napi_ok) return false;

  uint32_t len = 0;
  if (napi_get_array_length(env, names, &len) != napi_ok) return false;

  llm_structured::JsonObject obj;
  for (uint32_t i = 0; i < len; ++i) {
    napi_value keyv;
    if (napi_get_element(env, names, i, &keyv) != napi_ok) return false;
    std::string key;
    if (!GetStringUtf8(env, keyv, key)) return false;

    napi_value val;
    if (napi_get_property(env, v, keyv, &val) != napi_ok) return false;

    Json child;
    if (!FromNapi(env, val, child)) return false;
    obj.emplace(std::move(key), std::move(child));
  }
  out = Json(std::move(obj));
  return true;
}

static bool FromNapiArray(napi_env env, napi_value v, Json& out) {
  uint32_t len = 0;
  if (napi_get_array_length(env, v, &len) != napi_ok) return false;
  llm_structured::JsonArray arr;
  arr.reserve(len);
  for (uint32_t i = 0; i < len; ++i) {
    napi_value el;
    if (napi_get_element(env, v, i, &el) != napi_ok) return false;
    Json child;
    if (!FromNapi(env, el, child)) return false;
    arr.push_back(std::move(child));
  }
  out = Json(std::move(arr));
  return true;
}

static bool FromNapi(napi_env env, napi_value v, Json& out) {
  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return false;

  if (t == napi_null || t == napi_undefined) {
    out = Json(nullptr);
    return true;
  }
  if (t == napi_boolean) {
    bool b = false;
    if (napi_get_value_bool(env, v, &b) != napi_ok) return false;
    out = Json(b);
    return true;
  }
  if (t == napi_number) {
    double n = 0;
    if (napi_get_value_double(env, v, &n) != napi_ok) return false;
    out = Json(n);
    return true;
  }
  if (t == napi_string) {
    std::string s;
    if (!GetStringUtf8(env, v, s)) return false;
    out = Json(std::move(s));
    return true;
  }
  if (t == napi_object) {
    bool is_array = false;
    if (napi_is_array(env, v, &is_array) != napi_ok) return false;
    if (is_array) return FromNapiArray(env, v, out);
    return FromNapiObject(env, v, out);
  }
  return false;
}

static napi_value ToNapiSqlParsed(napi_env env, const SqlParsed& parsed) {
  napi_value obj;
  napi_create_object(env, &obj);

  napi_set_named_property(env, obj, "sql", MakeString(env, parsed.sql));
  napi_set_named_property(env, obj, "statementType", MakeString(env, parsed.statementType));

  napi_value b;
  napi_get_boolean(env, parsed.hasWhere, &b);
  napi_set_named_property(env, obj, "hasWhere", b);
  napi_get_boolean(env, parsed.hasFrom, &b);
  napi_set_named_property(env, obj, "hasFrom", b);
  napi_get_boolean(env, parsed.hasLimit, &b);
  napi_set_named_property(env, obj, "hasLimit", b);
  napi_get_boolean(env, parsed.hasUnion, &b);
  napi_set_named_property(env, obj, "hasUnion", b);
  napi_get_boolean(env, parsed.hasComments, &b);
  napi_set_named_property(env, obj, "hasComments", b);
  napi_get_boolean(env, parsed.hasSubquery, &b);
  napi_set_named_property(env, obj, "hasSubquery", b);

  if (parsed.limit) {
    napi_value n;
    napi_create_int32(env, *parsed.limit, &n);
    napi_set_named_property(env, obj, "limit", n);
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "limit", n);
  }

  napi_value tables;
  napi_create_array_with_length(env, parsed.tables.size(), &tables);
  for (size_t i = 0; i < parsed.tables.size(); ++i) {
    napi_set_element(env, tables, static_cast<uint32_t>(i), MakeString(env, parsed.tables[i]));
  }
  napi_set_named_property(env, obj, "tables", tables);
  return obj;
}

static napi_value MakeValidationErrorObject(napi_env env, const ValidationError& e) {
  napi_value obj;
  napi_create_object(env, &obj);

  napi_set_named_property(env, obj, "name", MakeString(env, "ValidationError"));
  napi_set_named_property(env, obj, "message", MakeString(env, e.what()));
  napi_set_named_property(env, obj, "path", MakeString(env, e.path));
  napi_set_named_property(env, obj, "kind", MakeString(env, e.kind));
  napi_set_named_property(env, obj, "jsonPointer", MakeString(env, llm_structured::json_pointer_from_path(e.path)));

  std::string kind;
  double current = 0;
  double max = 0;
  if (TryParseStreamLimit(e, kind, current, max)) {
    napi_value lim;
    napi_create_object(env, &lim);
    napi_set_named_property(env, lim, "kind", MakeString(env, kind));
    napi_value n;
    napi_create_double(env, current, &n);
    napi_set_named_property(env, lim, "current", n);
    napi_create_double(env, max, &n);
    napi_set_named_property(env, lim, "max", n);
    napi_set_named_property(env, obj, "limit", lim);
  }
  return obj;
}

template <typename T>
static bool GetExternalPtr(napi_env env, napi_value v, T*& out) {
  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return false;
  if (t != napi_external) return false;
  void* ptr = nullptr;
  if (napi_get_value_external(env, v, &ptr) != napi_ok) return false;
  out = static_cast<T*>(ptr);
  return out != nullptr;
}

static napi_value ToNapiObject(napi_env env, const llm_structured::JsonObject& o) {
  napi_value obj;
  napi_create_object(env, &obj);
  for (const auto& kv : o) {
    napi_value val = ToNapi(env, kv.second);
    napi_set_named_property(env, obj, kv.first.c_str(), val);
  }
  return obj;
}

static napi_value ToNapiArray(napi_env env, const llm_structured::JsonArray& a) {
  napi_value arr;
  napi_create_array_with_length(env, a.size(), &arr);
  for (size_t i = 0; i < a.size(); ++i) {
    napi_value val = ToNapi(env, a[i]);
    napi_set_element(env, arr, static_cast<uint32_t>(i), val);
  }
  return arr;
}

static napi_value ToNapi(napi_env env, const Json& v) {
  if (v.is_null()) {
    napi_value n;
    napi_get_null(env, &n);
    return n;
  }
  if (v.is_bool()) {
    napi_value b;
    napi_get_boolean(env, v.as_bool(), &b);
    return b;
  }
  if (v.is_number()) {
    napi_value n;
    napi_create_double(env, v.as_number(), &n);
    return n;
  }
  if (v.is_string()) {
    return MakeString(env, v.as_string());
  }
  if (v.is_array()) {
    return ToNapiArray(env, v.as_array());
  }
  return ToNapiObject(env, v.as_object());
}

static napi_value ParseAndValidateJson(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "parseAndValidateJson(text, schemaJson) expects 2 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateJson(text, schemaJson) expects strings");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    Json result = llm_structured::parse_and_validate(text, schema);
    return ToNapi(env, result);
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ParseAndValidateJsonWithDefaults(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "parseAndValidateJsonWithDefaults(text, schemaJson) expects 2 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateJsonWithDefaults(text, schemaJson) expects strings");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    Json result = llm_structured::parse_and_validate_with_defaults(text, schema);
    return ToNapi(env, result);
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value LoadsJsonishEx(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "loadsJsonishEx(text, repair?) expects 1-2 arguments");
    return nullptr;
  }

  std::string text;
  if (!GetStringUtf8(env, argv[0], text)) {
    ThrowTypeError(env, "loadsJsonishEx(text, repair?) expects text as string");
    return nullptr;
  }

  llm_structured::RepairConfig repair;
  if (argc >= 2) {
    if (!RepairConfigFromNapi(env, argv[1], repair)) return nullptr;
  }

  try {
    auto r = llm_structured::loads_jsonish_ex(text, repair);
    napi_value obj;
    napi_create_object(env, &obj);
    napi_set_named_property(env, obj, "value", ToNapi(env, r.value));
    napi_set_named_property(env, obj, "fixed", MakeString(env, r.fixed));
    napi_set_named_property(env, obj, "metadata", RepairMetadataToNapi(env, r.metadata));
    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ExtractJsonCandidates(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "extractJsonCandidates(text) expects 1 argument");
    return nullptr;
  }

  std::string text;
  if (!GetStringUtf8(env, argv[0], text)) {
    ThrowTypeError(env, "extractJsonCandidates(text) expects text as string");
    return nullptr;
  }

  try {
    const auto cands = llm_structured::extract_json_candidates(text);
    napi_value arr;
    napi_create_array_with_length(env, cands.size(), &arr);
    for (size_t i = 0; i < cands.size(); ++i) {
      napi_set_element(env, arr, static_cast<uint32_t>(i), MakeString(env, cands[i]));
    }
    return arr;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value LoadsJsonishAllEx(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "loadsJsonishAllEx(text, repair?) expects 1-2 arguments");
    return nullptr;
  }

  std::string text;
  if (!GetStringUtf8(env, argv[0], text)) {
    ThrowTypeError(env, "loadsJsonishAllEx(text, repair?) expects text as string");
    return nullptr;
  }

  llm_structured::RepairConfig repair;
  if (argc >= 2) {
    if (!RepairConfigFromNapi(env, argv[1], repair)) return nullptr;
  }

  try {
    auto r = llm_structured::loads_jsonish_all_ex(text, repair);
    napi_value obj;
    napi_create_object(env, &obj);

    napi_value values;
    napi_create_array_with_length(env, r.values.size(), &values);
    for (size_t i = 0; i < r.values.size(); ++i) {
      napi_set_element(env, values, static_cast<uint32_t>(i), ToNapi(env, r.values[i]));
    }
    napi_set_named_property(env, obj, "values", values);

    napi_value fixed;
    napi_create_array_with_length(env, r.fixed.size(), &fixed);
    for (size_t i = 0; i < r.fixed.size(); ++i) {
      napi_set_element(env, fixed, static_cast<uint32_t>(i), MakeString(env, r.fixed[i]));
    }
    napi_set_named_property(env, obj, "fixed", fixed);

    napi_value meta;
    napi_create_array_with_length(env, r.metadata.size(), &meta);
    for (size_t i = 0; i < r.metadata.size(); ++i) {
      napi_set_element(env, meta, static_cast<uint32_t>(i), RepairMetadataToNapi(env, r.metadata[i]));
    }
    napi_set_named_property(env, obj, "metadata", meta);

    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ParseAndValidateJsonAllEx(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 2 || argc > 3) {
    ThrowTypeError(env, "parseAndValidateJsonAllEx(text, schemaJson, repair?) expects 2-3 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateJsonAllEx(text, schemaJson, repair?) expects strings for text and schemaJson");
    return nullptr;
  }

  llm_structured::RepairConfig repair;
  if (argc >= 3) {
    if (!RepairConfigFromNapi(env, argv[2], repair)) return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    auto r = llm_structured::parse_and_validate_all_ex(text, schema, repair);
    napi_value obj;
    napi_create_object(env, &obj);

    napi_value values;
    napi_create_array_with_length(env, r.values.size(), &values);
    for (size_t i = 0; i < r.values.size(); ++i) {
      napi_set_element(env, values, static_cast<uint32_t>(i), ToNapi(env, r.values[i]));
    }
    napi_set_named_property(env, obj, "values", values);

    napi_value fixed;
    napi_create_array_with_length(env, r.fixed.size(), &fixed);
    for (size_t i = 0; i < r.fixed.size(); ++i) {
      napi_set_element(env, fixed, static_cast<uint32_t>(i), MakeString(env, r.fixed[i]));
    }
    napi_set_named_property(env, obj, "fixed", fixed);

    napi_value meta;
    napi_create_array_with_length(env, r.metadata.size(), &meta);
    for (size_t i = 0; i < r.metadata.size(); ++i) {
      napi_set_element(env, meta, static_cast<uint32_t>(i), RepairMetadataToNapi(env, r.metadata[i]));
    }
    napi_set_named_property(env, obj, "metadata", meta);

    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ParseAndValidateJsonEx(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 2 || argc > 3) {
    ThrowTypeError(env, "parseAndValidateJsonEx(text, schemaJson, repair?) expects 2-3 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateJsonEx(text, schemaJson, repair?) expects strings for text and schemaJson");
    return nullptr;
  }

  llm_structured::RepairConfig repair;
  if (argc >= 3) {
    if (!RepairConfigFromNapi(env, argv[2], repair)) return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    auto r = llm_structured::parse_and_validate_ex(text, schema, repair);
    napi_value obj;
    napi_create_object(env, &obj);
    napi_set_named_property(env, obj, "value", ToNapi(env, r.value));
    napi_set_named_property(env, obj, "fixed", MakeString(env, r.fixed));
    napi_set_named_property(env, obj, "metadata", RepairMetadataToNapi(env, r.metadata));
    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ParseAndValidateJsonWithDefaultsEx(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 2 || argc > 3) {
    ThrowTypeError(env, "parseAndValidateJsonWithDefaultsEx(text, schemaJson, repair?) expects 2-3 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateJsonWithDefaultsEx(text, schemaJson, repair?) expects strings for text and schemaJson");
    return nullptr;
  }

  llm_structured::RepairConfig repair;
  if (argc >= 3) {
    if (!RepairConfigFromNapi(env, argv[2], repair)) return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    auto r = llm_structured::parse_and_validate_with_defaults_ex(text, schema, repair);
    napi_value obj;
    napi_create_object(env, &obj);
    napi_set_named_property(env, obj, "value", ToNapi(env, r.value));
    napi_set_named_property(env, obj, "fixed", MakeString(env, r.fixed));
    napi_set_named_property(env, obj, "metadata", RepairMetadataToNapi(env, r.metadata));
    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ValidateAllJson(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "validateAllJson(text, schemaJson) expects 2 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "validateAllJson(text, schemaJson) expects strings");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    Json value = llm_structured::loads_jsonish(text);

    auto errs = llm_structured::validate_all(value, schema, "$");
    napi_value arr;
    napi_create_array_with_length(env, errs.size(), &arr);
    for (size_t i = 0; i < errs.size(); ++i) {
      napi_value obj;
      napi_create_object(env, &obj);
      napi_set_named_property(env, obj, "message", MakeString(env, errs[i].what()));
      napi_set_named_property(env, obj, "path", MakeString(env, errs[i].path));
      napi_set_named_property(env, obj, "kind", MakeString(env, errs[i].kind));
      napi_set_named_property(env, obj, "jsonPointer", MakeString(env, llm_structured::json_pointer_from_path(errs[i].path)));
      napi_set_element(env, arr, static_cast<uint32_t>(i), obj);
    }
    return arr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ValidateAllJsonValue(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "validateAllJsonValue(value, schemaJson) expects 2 arguments");
    return nullptr;
  }

  std::string schema_text;
  if (!GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "validateAllJsonValue(value, schemaJson) expects schemaJson as string");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    Json value;
    if (!FromNapi(env, argv[0], value)) {
      ThrowTypeError(env, "validateAllJsonValue(value, schemaJson) received an unsupported JS value");
      return nullptr;
    }

    auto errs = llm_structured::validate_all(value, schema, "$");
    napi_value arr;
    napi_create_array_with_length(env, errs.size(), &arr);
    for (size_t i = 0; i < errs.size(); ++i) {
      napi_value obj;
      napi_create_object(env, &obj);
      napi_set_named_property(env, obj, "message", MakeString(env, errs[i].what()));
      napi_set_named_property(env, obj, "path", MakeString(env, errs[i].path));
      napi_set_named_property(env, obj, "kind", MakeString(env, errs[i].kind));
      napi_set_named_property(env, obj, "jsonPointer", MakeString(env, llm_structured::json_pointer_from_path(errs[i].path)));
      napi_set_element(env, arr, static_cast<uint32_t>(i), obj);
    }
    return arr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ParseAndValidateSql(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "parseAndValidateSql(sqlText, schemaJson) expects 2 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateSql(sqlText, schemaJson) expects strings");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    auto parsed = llm_structured::parse_and_validate_sql(text, schema);

    return ToNapiSqlParsed(env, parsed);
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static void FinalizeJsonStreamParser(napi_env /*env*/, void* data, void* /*hint*/) {
  delete static_cast<llm_structured::JsonStreamParser*>(data);
}

static void FinalizeJsonStreamCollector(napi_env /*env*/, void* data, void* /*hint*/) {
  delete static_cast<llm_structured::JsonStreamCollector*>(data);
}

static void FinalizeJsonStreamBatchCollector(napi_env /*env*/, void* data, void* /*hint*/) {
  delete static_cast<llm_structured::JsonStreamBatchCollector*>(data);
}

static void FinalizeJsonStreamValidatedBatchCollector(napi_env /*env*/, void* data, void* /*hint*/) {
  delete static_cast<llm_structured::JsonStreamValidatedBatchCollector*>(data);
}

static void FinalizeSqlStreamParser(napi_env /*env*/, void* data, void* /*hint*/) {
  delete static_cast<llm_structured::SqlStreamParser*>(data);
}

static napi_value CreateJsonStreamParser(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "createJsonStreamParser(schemaJson, limits?) expects 1-2 arguments");
    return nullptr;
  }

  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], schema_text)) {
    ThrowTypeError(env, "createJsonStreamParser(schemaJson) expects a string");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);

    size_t max_buffer_bytes = 0;
    if (argc == 2) {
      napi_valuetype t;
      if (napi_typeof(env, argv[1], &t) != napi_ok) return nullptr;
      if (t != napi_undefined && t != napi_null) {
        if (t != napi_object) {
          ThrowTypeError(env, "createJsonStreamParser(..., limits) expects limits as object");
          return nullptr;
        }
        if (!GetOptionalSizeTProperty(env, argv[1], "maxBufferBytes", max_buffer_bytes)) return nullptr;
      }
    }

    auto* p = (max_buffer_bytes > 0)
                  ? new llm_structured::JsonStreamParser(std::move(schema), max_buffer_bytes)
                  : new llm_structured::JsonStreamParser(std::move(schema));
    napi_value ext;
    napi_create_external(env, p, FinalizeJsonStreamParser, nullptr, &ext);
    return ext;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value JsonStreamParserFinish(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamParserFinish(parser) expects 1 argument");
    return nullptr;
  }

  llm_structured::JsonStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "jsonStreamParserFinish(parser) expects a parser external");
    return nullptr;
  }

  p->finish();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamParserLocation(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamParserLocation(parser) expects 1 argument");
    return nullptr;
  }

  llm_structured::JsonStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "jsonStreamParserLocation(parser) expects a parser external");
    return nullptr;
  }

  return StreamLocationToNapi(env, p->location());
}

static napi_value CreateJsonStreamCollector(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "createJsonStreamCollector(schemaJson, limits?) expects 1-2 arguments");
    return nullptr;
  }

  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], schema_text)) {
    ThrowTypeError(env, "createJsonStreamCollector(schemaJson) expects a string");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);

    size_t max_buffer_bytes = 0;
    size_t max_items = 0;
    if (argc == 2) {
      napi_valuetype t;
      if (napi_typeof(env, argv[1], &t) != napi_ok) return nullptr;
      if (t != napi_undefined && t != napi_null) {
        if (t != napi_object) {
          ThrowTypeError(env, "createJsonStreamCollector(..., limits) expects limits as object");
          return nullptr;
        }
        if (!GetOptionalSizeTProperty(env, argv[1], "maxBufferBytes", max_buffer_bytes)) return nullptr;
        if (!GetOptionalSizeTProperty(env, argv[1], "maxItems", max_items)) return nullptr;
      }
    }

    auto* c = (max_buffer_bytes > 0 || max_items > 0)
                  ? new llm_structured::JsonStreamCollector(std::move(schema), max_buffer_bytes, max_items)
                  : new llm_structured::JsonStreamCollector(std::move(schema));
    napi_value ext;
    napi_create_external(env, c, FinalizeJsonStreamCollector, nullptr, &ext);
    return ext;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value JsonStreamCollectorLocation(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamCollectorLocation(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamCollectorLocation(collector) expects a collector external");
    return nullptr;
  }
  return StreamLocationToNapi(env, c->location());
}

static napi_value CreateJsonStreamBatchCollector(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "createJsonStreamBatchCollector(schemaJson, limits?) expects 1-2 arguments");
    return nullptr;
  }

  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], schema_text)) {
    ThrowTypeError(env, "createJsonStreamBatchCollector(schemaJson) expects a string");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);

    size_t max_buffer_bytes = 0;
    size_t max_items = 0;
    if (argc == 2) {
      napi_valuetype t;
      if (napi_typeof(env, argv[1], &t) != napi_ok) return nullptr;
      if (t != napi_undefined && t != napi_null) {
        if (t != napi_object) {
          ThrowTypeError(env, "createJsonStreamBatchCollector(..., limits) expects limits as object");
          return nullptr;
        }
        if (!GetOptionalSizeTProperty(env, argv[1], "maxBufferBytes", max_buffer_bytes)) return nullptr;
        if (!GetOptionalSizeTProperty(env, argv[1], "maxItems", max_items)) return nullptr;
      }
    }

    auto* c = (max_buffer_bytes > 0 || max_items > 0)
                  ? new llm_structured::JsonStreamBatchCollector(std::move(schema), max_buffer_bytes, max_items)
                  : new llm_structured::JsonStreamBatchCollector(std::move(schema));
    napi_value ext;
    napi_create_external(env, c, FinalizeJsonStreamBatchCollector, nullptr, &ext);
    return ext;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value JsonStreamBatchCollectorLocation(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamBatchCollectorLocation(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamBatchCollectorLocation(collector) expects a collector external");
    return nullptr;
  }
  return StreamLocationToNapi(env, c->location());
}

static napi_value CreateJsonStreamValidatedBatchCollector(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "createJsonStreamValidatedBatchCollector(schemaJson, limits?) expects 1-2 arguments");
    return nullptr;
  }

  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], schema_text)) {
    ThrowTypeError(env, "createJsonStreamValidatedBatchCollector(schemaJson) expects a string");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);

    size_t max_buffer_bytes = 0;
    size_t max_items = 0;
    if (argc == 2) {
      napi_valuetype t;
      if (napi_typeof(env, argv[1], &t) != napi_ok) return nullptr;
      if (t != napi_undefined && t != napi_null) {
        if (t != napi_object) {
          ThrowTypeError(env, "createJsonStreamValidatedBatchCollector(..., limits) expects limits as object");
          return nullptr;
        }
        if (!GetOptionalSizeTProperty(env, argv[1], "maxBufferBytes", max_buffer_bytes)) return nullptr;
        if (!GetOptionalSizeTProperty(env, argv[1], "maxItems", max_items)) return nullptr;
      }
    }

    auto* c = (max_buffer_bytes > 0 || max_items > 0)
                  ? new llm_structured::JsonStreamValidatedBatchCollector(std::move(schema), max_buffer_bytes, max_items)
                  : new llm_structured::JsonStreamValidatedBatchCollector(std::move(schema));
    napi_value ext;
    napi_create_external(env, c, FinalizeJsonStreamValidatedBatchCollector, nullptr, &ext);
    return ext;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value JsonStreamValidatedBatchCollectorReset(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorReset(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamValidatedBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorReset(collector) expects a collector external");
    return nullptr;
  }
  c->reset();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamValidatedBatchCollectorAppend(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorAppend(collector, chunk) expects 2 arguments");
    return nullptr;
  }
  llm_structured::JsonStreamValidatedBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorAppend(collector, chunk) expects a collector external");
    return nullptr;
  }
  std::string chunk;
  if (!GetStringUtf8(env, argv[1], chunk)) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorAppend(collector, chunk) expects chunk as string");
    return nullptr;
  }
  c->append(chunk);
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamValidatedBatchCollectorClose(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorClose(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamValidatedBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorClose(collector) expects a collector external");
    return nullptr;
  }
  c->close();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamValidatedBatchCollectorPoll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorPoll(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamValidatedBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorPoll(collector) expects a collector external");
    return nullptr;
  }

  auto out = c->poll();
  napi_value obj;
  napi_create_object(env, &obj);

  napi_value b;
  napi_get_boolean(env, out.done, &b);
  napi_set_named_property(env, obj, "done", b);
  napi_get_boolean(env, out.ok, &b);
  napi_set_named_property(env, obj, "ok", b);

  if (out.value) {
    napi_value arr = ToNapiArray(env, *out.value);
    napi_set_named_property(env, obj, "value", arr);
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "value", n);
  }

  if (out.error) {
    napi_set_named_property(env, obj, "error", MakeValidationErrorObject(env, *out.error));
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "error", n);
  }

  return obj;
}

static napi_value JsonStreamValidatedBatchCollectorLocation(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorLocation(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamValidatedBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorLocation(collector) expects a collector external");
    return nullptr;
  }
  return StreamLocationToNapi(env, c->location());
}

static napi_value JsonStreamParserReset(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamParserReset(parser) expects 1 argument");
    return nullptr;
  }

  llm_structured::JsonStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "jsonStreamParserReset(parser) expects a parser external");
    return nullptr;
  }
  p->reset();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamParserAppend(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "jsonStreamParserAppend(parser, chunk) expects 2 arguments");
    return nullptr;
  }

  llm_structured::JsonStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "jsonStreamParserAppend(parser, chunk) expects a parser external");
    return nullptr;
  }

  std::string chunk;
  if (!GetStringUtf8(env, argv[1], chunk)) {
    ThrowTypeError(env, "jsonStreamParserAppend(parser, chunk) expects chunk as string");
    return nullptr;
  }

  p->append(chunk);
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamParserPoll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamParserPoll(parser) expects 1 argument");
    return nullptr;
  }

  llm_structured::JsonStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "jsonStreamParserPoll(parser) expects a parser external");
    return nullptr;
  }

  auto out = p->poll();
  napi_value obj;
  napi_create_object(env, &obj);

  napi_value b;
  napi_get_boolean(env, out.done, &b);
  napi_set_named_property(env, obj, "done", b);
  napi_get_boolean(env, out.ok, &b);
  napi_set_named_property(env, obj, "ok", b);

  if (out.value) {
    napi_set_named_property(env, obj, "value", ToNapi(env, *out.value));
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "value", n);
  }

  if (out.error) {
    napi_set_named_property(env, obj, "error", MakeValidationErrorObject(env, *out.error));
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "error", n);
  }

  return obj;
}

static napi_value JsonStreamCollectorReset(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamCollectorReset(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamCollectorReset(collector) expects a collector external");
    return nullptr;
  }
  c->reset();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamCollectorAppend(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "jsonStreamCollectorAppend(collector, chunk) expects 2 arguments");
    return nullptr;
  }
  llm_structured::JsonStreamCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamCollectorAppend(collector, chunk) expects a collector external");
    return nullptr;
  }
  std::string chunk;
  if (!GetStringUtf8(env, argv[1], chunk)) {
    ThrowTypeError(env, "jsonStreamCollectorAppend(collector, chunk) expects chunk as string");
    return nullptr;
  }
  c->append(chunk);
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamCollectorClose(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamCollectorClose(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamCollectorClose(collector) expects a collector external");
    return nullptr;
  }
  c->close();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamCollectorPoll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamCollectorPoll(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamCollectorPoll(collector) expects a collector external");
    return nullptr;
  }

  auto out = c->poll();
  napi_value obj;
  napi_create_object(env, &obj);

  napi_value b;
  napi_get_boolean(env, out.done, &b);
  napi_set_named_property(env, obj, "done", b);
  napi_get_boolean(env, out.ok, &b);
  napi_set_named_property(env, obj, "ok", b);

  if (out.value) {
    napi_value arr = ToNapiArray(env, *out.value);
    napi_set_named_property(env, obj, "value", arr);
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "value", n);
  }

  if (out.error) {
    napi_set_named_property(env, obj, "error", MakeValidationErrorObject(env, *out.error));
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "error", n);
  }

  return obj;
}

static napi_value JsonStreamBatchCollectorReset(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamBatchCollectorReset(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamBatchCollectorReset(collector) expects a collector external");
    return nullptr;
  }
  c->reset();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamBatchCollectorAppend(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "jsonStreamBatchCollectorAppend(collector, chunk) expects 2 arguments");
    return nullptr;
  }
  llm_structured::JsonStreamBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamBatchCollectorAppend(collector, chunk) expects a collector external");
    return nullptr;
  }
  std::string chunk;
  if (!GetStringUtf8(env, argv[1], chunk)) {
    ThrowTypeError(env, "jsonStreamBatchCollectorAppend(collector, chunk) expects chunk as string");
    return nullptr;
  }
  c->append(chunk);
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamBatchCollectorClose(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamBatchCollectorClose(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamBatchCollectorClose(collector) expects a collector external");
    return nullptr;
  }
  c->close();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamBatchCollectorPoll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamBatchCollectorPoll(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamBatchCollectorPoll(collector) expects a collector external");
    return nullptr;
  }

  auto out = c->poll();
  napi_value obj;
  napi_create_object(env, &obj);

  napi_value b;
  napi_get_boolean(env, out.done, &b);
  napi_set_named_property(env, obj, "done", b);
  napi_get_boolean(env, out.ok, &b);
  napi_set_named_property(env, obj, "ok", b);

  if (out.value) {
    napi_value arr = ToNapiArray(env, *out.value);
    napi_set_named_property(env, obj, "value", arr);
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "value", n);
  }

  if (out.error) {
    napi_set_named_property(env, obj, "error", MakeValidationErrorObject(env, *out.error));
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "error", n);
  }

  return obj;
}

static napi_value CreateSqlStreamParser(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "createSqlStreamParser(schemaJson, limits?) expects 1-2 arguments");
    return nullptr;
  }

  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], schema_text)) {
    ThrowTypeError(env, "createSqlStreamParser(schemaJson) expects a string");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);

    size_t max_buffer_bytes = 0;
    if (argc == 2) {
      napi_valuetype t;
      if (napi_typeof(env, argv[1], &t) != napi_ok) return nullptr;
      if (t != napi_undefined && t != napi_null) {
        if (t != napi_object) {
          ThrowTypeError(env, "createSqlStreamParser(..., limits) expects limits as object");
          return nullptr;
        }
        if (!GetOptionalSizeTProperty(env, argv[1], "maxBufferBytes", max_buffer_bytes)) return nullptr;
      }
    }

    auto* p = (max_buffer_bytes > 0)
                  ? new llm_structured::SqlStreamParser(std::move(schema), max_buffer_bytes)
                  : new llm_structured::SqlStreamParser(std::move(schema));
    napi_value ext;
    napi_create_external(env, p, FinalizeSqlStreamParser, nullptr, &ext);
    return ext;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value SqlStreamParserFinish(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "sqlStreamParserFinish(parser) expects 1 argument");
    return nullptr;
  }
  llm_structured::SqlStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "sqlStreamParserFinish(parser) expects a parser external");
    return nullptr;
  }
  p->finish();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value SqlStreamParserLocation(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "sqlStreamParserLocation(parser) expects 1 argument");
    return nullptr;
  }
  llm_structured::SqlStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "sqlStreamParserLocation(parser) expects a parser external");
    return nullptr;
  }
  return StreamLocationToNapi(env, p->location());
}

static napi_value SqlStreamParserReset(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "sqlStreamParserReset(parser) expects 1 argument");
    return nullptr;
  }

  llm_structured::SqlStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "sqlStreamParserReset(parser) expects a parser external");
    return nullptr;
  }
  p->reset();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value SqlStreamParserAppend(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "sqlStreamParserAppend(parser, chunk) expects 2 arguments");
    return nullptr;
  }

  llm_structured::SqlStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "sqlStreamParserAppend(parser, chunk) expects a parser external");
    return nullptr;
  }

  std::string chunk;
  if (!GetStringUtf8(env, argv[1], chunk)) {
    ThrowTypeError(env, "sqlStreamParserAppend(parser, chunk) expects chunk as string");
    return nullptr;
  }

  p->append(chunk);
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value SqlStreamParserPoll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "sqlStreamParserPoll(parser) expects 1 argument");
    return nullptr;
  }

  llm_structured::SqlStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "sqlStreamParserPoll(parser) expects a parser external");
    return nullptr;
  }

  auto out = p->poll();
  napi_value obj;
  napi_create_object(env, &obj);

  napi_value b;
  napi_get_boolean(env, out.done, &b);
  napi_set_named_property(env, obj, "done", b);
  napi_get_boolean(env, out.ok, &b);
  napi_set_named_property(env, obj, "ok", b);

  if (out.value) {
    napi_set_named_property(env, obj, "value", ToNapiSqlParsed(env, *out.value));
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "value", n);
  }

  if (out.error) {
    napi_set_named_property(env, obj, "error", MakeValidationErrorObject(env, *out.error));
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "error", n);
  }

  return obj;
}

static napi_value ParseAndValidateMarkdown(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "parseAndValidateMarkdown(markdownText, schemaJson) expects 2 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateMarkdown(markdownText, schemaJson) expects strings");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    auto parsed = llm_structured::parse_and_validate_markdown(text, schema);

    napi_value obj;
    napi_create_object(env, &obj);

    napi_value n;
    napi_create_int32(env, static_cast<int32_t>(parsed.headings.size()), &n);
    napi_set_named_property(env, obj, "headingCount", n);
    napi_create_int32(env, static_cast<int32_t>(parsed.codeBlocks.size()), &n);
    napi_set_named_property(env, obj, "codeBlockCount", n);
    napi_create_int32(env, static_cast<int32_t>(parsed.tables.size()), &n);
    napi_set_named_property(env, obj, "tableCount", n);
    napi_create_int32(env, static_cast<int32_t>(parsed.taskLineNumbers.size()), &n);
    napi_set_named_property(env, obj, "taskCount", n);

    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ParseAndValidateKv(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "parseAndValidateKv(text, schemaJson) expects 2 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateKv(text, schemaJson) expects strings");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    auto kv = llm_structured::parse_and_validate_kv(text, schema);
    napi_value obj;
    napi_create_object(env, &obj);
    for (const auto& it : kv) {
      napi_set_named_property(env, obj, it.first.c_str(), MakeString(env, it.second));
    }
    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value Init(napi_env env, napi_value exports) {
  napi_property_descriptor props[] = {
      {"parseAndValidateJson", nullptr, ParseAndValidateJson, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"parseAndValidateJsonWithDefaults", nullptr, ParseAndValidateJsonWithDefaults, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"loadsJsonishEx", nullptr, LoadsJsonishEx, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"extractJsonCandidates", nullptr, ExtractJsonCandidates, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"loadsJsonishAllEx", nullptr, LoadsJsonishAllEx, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"parseAndValidateJsonEx", nullptr, ParseAndValidateJsonEx, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"parseAndValidateJsonAllEx", nullptr, ParseAndValidateJsonAllEx, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"parseAndValidateJsonWithDefaultsEx", nullptr, ParseAndValidateJsonWithDefaultsEx, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"validateAllJson", nullptr, ValidateAllJson, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"validateAllJsonValue", nullptr, ValidateAllJsonValue, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"parseAndValidateSql", nullptr, ParseAndValidateSql, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"parseAndValidateMarkdown", nullptr, ParseAndValidateMarkdown, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"parseAndValidateKv", nullptr, ParseAndValidateKv, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"createJsonStreamParser", nullptr, CreateJsonStreamParser, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"jsonStreamParserReset", nullptr, JsonStreamParserReset, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"jsonStreamParserFinish", nullptr, JsonStreamParserFinish, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"jsonStreamParserAppend", nullptr, JsonStreamParserAppend, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"jsonStreamParserPoll", nullptr, JsonStreamParserPoll, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"jsonStreamParserLocation", nullptr, JsonStreamParserLocation, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"createJsonStreamCollector", nullptr, CreateJsonStreamCollector, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamCollectorReset", nullptr, JsonStreamCollectorReset, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamCollectorAppend", nullptr, JsonStreamCollectorAppend, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamCollectorClose", nullptr, JsonStreamCollectorClose, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamCollectorPoll", nullptr, JsonStreamCollectorPoll, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamCollectorLocation", nullptr, JsonStreamCollectorLocation, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"createJsonStreamBatchCollector", nullptr, CreateJsonStreamBatchCollector, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamBatchCollectorReset", nullptr, JsonStreamBatchCollectorReset, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamBatchCollectorAppend", nullptr, JsonStreamBatchCollectorAppend, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamBatchCollectorClose", nullptr, JsonStreamBatchCollectorClose, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamBatchCollectorPoll", nullptr, JsonStreamBatchCollectorPoll, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamBatchCollectorLocation", nullptr, JsonStreamBatchCollectorLocation, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"createJsonStreamValidatedBatchCollector", nullptr, CreateJsonStreamValidatedBatchCollector, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamValidatedBatchCollectorReset", nullptr, JsonStreamValidatedBatchCollectorReset, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamValidatedBatchCollectorAppend", nullptr, JsonStreamValidatedBatchCollectorAppend, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamValidatedBatchCollectorClose", nullptr, JsonStreamValidatedBatchCollectorClose, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamValidatedBatchCollectorPoll", nullptr, JsonStreamValidatedBatchCollectorPoll, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamValidatedBatchCollectorLocation", nullptr, JsonStreamValidatedBatchCollectorLocation, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"createSqlStreamParser", nullptr, CreateSqlStreamParser, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"sqlStreamParserReset", nullptr, SqlStreamParserReset, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"sqlStreamParserFinish", nullptr, SqlStreamParserFinish, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"sqlStreamParserAppend", nullptr, SqlStreamParserAppend, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"sqlStreamParserPoll", nullptr, SqlStreamParserPoll, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"sqlStreamParserLocation", nullptr, SqlStreamParserLocation, nullptr, nullptr, nullptr, napi_default, nullptr},
  };

  napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props);
  return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
