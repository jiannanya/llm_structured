#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "llm_structured.hpp"

namespace py = pybind11;

using llm_structured::Json;
using llm_structured::JsonArray;
using llm_structured::JsonObject;
using llm_structured::KeyValue;
using llm_structured::MarkdownParsed;
using llm_structured::RepairConfig;
using llm_structured::RepairMetadata;
using llm_structured::StreamLocation;
using llm_structured::SqlParsed;
using llm_structured::ValidationError;

static py::object ToPy(const Json& v);

static bool FromPy(py::handle v, Json& out);

static py::object ToPyObject(const JsonObject& o) {
  py::dict d;
  for (const auto& kv : o) {
    d[py::str(kv.first)] = ToPy(kv.second);
  }
  return std::move(d);
}

static py::object ToPyArray(const JsonArray& a) {
  py::list out;
  for (const auto& el : a) {
    out.append(ToPy(el));
  }
  return std::move(out);
}

static py::object ToPyNumber(double n) {
  if (std::isfinite(n)) {
    const double ip = std::trunc(n);
    if (ip == n && ip >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
        ip <= static_cast<double>(std::numeric_limits<int64_t>::max())) {
      return py::int_(static_cast<int64_t>(ip));
    }
  }
  return py::float_(n);
}

static py::object ToPy(const Json& v) {
  if (v.is_null()) return py::none();
  if (v.is_bool()) return py::bool_(v.as_bool());
  if (v.is_number()) return ToPyNumber(v.as_number());
  if (v.is_string()) return py::str(v.as_string());
  if (v.is_array()) return ToPyArray(v.as_array());
  return ToPyObject(v.as_object());
}

static bool FromPyObject(py::handle v, Json& out) {
  py::dict d = py::reinterpret_borrow<py::dict>(v);
  JsonObject obj;
  for (auto item : d) {
    if (!py::isinstance<py::str>(item.first)) return false;
    std::string key = py::cast<std::string>(item.first);
    Json child;
    if (!FromPy(item.second, child)) return false;
    obj.emplace(std::move(key), std::move(child));
  }
  out = Json(std::move(obj));
  return true;
}

static bool FromPyArray(py::handle v, Json& out) {
  py::sequence seq = py::reinterpret_borrow<py::sequence>(v);
  JsonArray arr;
  arr.reserve(seq.size());
  for (auto item : seq) {
    Json child;
    if (!FromPy(item, child)) return false;
    arr.push_back(std::move(child));
  }
  out = Json(std::move(arr));
  return true;
}

static bool FromPy(py::handle v, Json& out) {
  if (v.is_none()) {
    out = Json(nullptr);
    return true;
  }
  if (py::isinstance<py::bool_>(v)) {
    out = Json(py::cast<bool>(v));
    return true;
  }
  if (py::isinstance<py::int_>(v)) {
    // Json stores numbers as double; keep integer range loss-minimal.
    const int64_t i = py::cast<int64_t>(v);
    out = Json(i);
    return true;
  }
  if (py::isinstance<py::float_>(v)) {
    out = Json(py::cast<double>(v));
    return true;
  }
  if (py::isinstance<py::str>(v)) {
    out = Json(py::cast<std::string>(v));
    return true;
  }
  if (py::isinstance<py::dict>(v)) {
    return FromPyObject(v, out);
  }
  if (py::isinstance<py::list>(v) || py::isinstance<py::tuple>(v)) {
    return FromPyArray(v, out);
  }
  return false;
}

static Json SchemaFromPy(py::handle schema) {
  if (py::isinstance<py::str>(schema)) {
    return llm_structured::loads_jsonish(py::cast<std::string>(schema));
  }
  Json s;
  if (!FromPy(schema, s)) {
    throw std::runtime_error("schema must be a JSON-serializable dict/list/primitive or a JSON string");
  }
  return s;
}

static py::dict MakeErrorObject(const ValidationError& e) {
  py::dict d;
  d["name"] = "ValidationError";
  d["message"] = std::string(e.what());
  d["path"] = e.path;
  d["kind"] = e.kind;
  d["jsonPointer"] = llm_structured::json_pointer_from_path(e.path);

  // Structured limit payload (best-effort parse from our standardized message).
  if (e.path == "$.stream.maxBufferBytes" || e.path == "$.stream.maxItems") {
    const std::string msg = e.what();
    const bool is_buf = (e.path == "$.stream.maxBufferBytes");
    const std::string cur_key = is_buf ? "size=" : "items=";

    const auto cur_pos = msg.find(cur_key);
    const auto max_pos = msg.find("max=");
    if (cur_pos != std::string::npos && max_pos != std::string::npos) {
      try {
        const auto cur_start = cur_pos + cur_key.size();
        const auto cur_end = msg.find_first_of(",)", cur_start);
        const auto max_start = max_pos + std::string("max=").size();
        const auto max_end = msg.find_first_of(")", max_start);

        const double current = std::stod(msg.substr(cur_start, cur_end - cur_start));
        const double max = std::stod(msg.substr(max_start, max_end - max_start));

        py::dict lim;
        lim["kind"] = is_buf ? "maxBufferBytes" : "maxItems";
        lim["current"] = current;
        lim["max"] = max;
        d["limit"] = lim;
      } catch (...) {
        // ignore
      }
    }
  }

  return d;
}

static py::object ValidationErrorType;

static void TranslateValidationError(const ValidationError& e) {
  const std::string msg = std::string(e.what());
  const std::string full = e.path.empty() ? msg : (e.path + ": " + msg);
  py::object exc = ValidationErrorType(py::str(full));
  exc.attr("message") = py::str(msg);
  exc.attr("path") = py::str(e.path);
  exc.attr("kind") = py::str(e.kind);
  exc.attr("jsonPointer") = py::str(llm_structured::json_pointer_from_path(e.path));
  if (e.path == "$.stream.maxBufferBytes" || e.path == "$.stream.maxItems") {
    py::dict err = MakeErrorObject(e);
    if (err.contains("limit")) exc.attr("limit") = err["limit"];
  }
  PyErr_SetObject(ValidationErrorType.ptr(), exc.ptr());
}

static RepairConfig RepairConfigFromPy(py::object o) {
  RepairConfig cfg;
  if (o.is_none()) return cfg;
  py::dict d = o.cast<py::dict>();
  auto set_bool = [&](const char* key, bool& field) {
    if (d.contains(key)) field = d[key].cast<bool>();
  };
  set_bool("fixSmartQuotes", cfg.fix_smart_quotes);
  set_bool("stripJsonComments", cfg.strip_json_comments);
  set_bool("replacePythonLiterals", cfg.replace_python_literals);
  set_bool("convertKvObjectToJson", cfg.convert_kv_object_to_json);
  set_bool("quoteUnquotedKeys", cfg.quote_unquoted_keys);
  set_bool("dropTrailingCommas", cfg.drop_trailing_commas);
  set_bool("allowSingleQuotes", cfg.allow_single_quotes);

  if (d.contains("duplicateKeyPolicy")) {
    const std::string raw = py::cast<std::string>(d["duplicateKeyPolicy"]);
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (s == "error") {
      cfg.duplicate_key_policy = RepairConfig::DuplicateKeyPolicy::Error;
    } else if (s == "firstwins" || s == "first_wins" || s == "first") {
      cfg.duplicate_key_policy = RepairConfig::DuplicateKeyPolicy::FirstWins;
    } else if (s == "lastwins" || s == "last_wins" || s == "last") {
      cfg.duplicate_key_policy = RepairConfig::DuplicateKeyPolicy::LastWins;
    } else {
      throw std::runtime_error("duplicateKeyPolicy must be one of: error | firstWins | lastWins");
    }
  }
  return cfg;
}

static py::dict RepairMetadataToPy(const RepairMetadata& m) {
  py::dict d;
  d["extractedFromFence"] = m.extracted_from_fence;
  d["fixedSmartQuotes"] = m.fixed_smart_quotes;
  d["strippedComments"] = m.stripped_comments;
  d["replacedPythonLiterals"] = m.replaced_python_literals;
  d["convertedKvObject"] = m.converted_kv_object;
  d["quotedUnquotedKeys"] = m.quoted_unquoted_keys;
  d["droppedTrailingCommas"] = m.dropped_trailing_commas;
  d["duplicateKeyCount"] = m.duplicateKeyCount;

  const auto pol = m.duplicateKeyPolicy;
  if (pol == RepairConfig::DuplicateKeyPolicy::Error) {
    d["duplicateKeyPolicy"] = "error";
  } else if (pol == RepairConfig::DuplicateKeyPolicy::LastWins) {
    d["duplicateKeyPolicy"] = "lastWins";
  } else {
    d["duplicateKeyPolicy"] = "firstWins";
  }
  return d;
}

static py::dict StreamLocationToPy(const StreamLocation& loc) {
  py::dict d;
  d["offset"] = py::int_(loc.offset);
  d["line"] = py::int_(loc.line);
  d["col"] = py::int_(loc.col);
  return d;
}

static py::dict MarkdownToDict(const MarkdownParsed& p) {
  py::dict out;
  out["text"] = p.text;

  py::list lines;
  for (const auto& l : p.lines) lines.append(py::str(l));
  out["lines"] = std::move(lines);

  py::list headings;
  for (const auto& h : p.headings) {
    py::dict hd;
    hd["level"] = h.level;
    hd["title"] = h.title;
    hd["line"] = h.line;
    headings.append(std::move(hd));
  }
  out["headings"] = std::move(headings);

  py::dict sections;
  for (const auto& sec : p.sections) {
    py::list parts;
    for (const auto& part : sec.second) parts.append(py::str(part));
    sections[py::str(sec.first)] = std::move(parts);
  }
  out["sections"] = std::move(sections);

  py::list codeBlocks;
  for (const auto& b : p.codeBlocks) {
    py::dict bd;
    bd["lang"] = b.lang;
    bd["body"] = b.body;
    codeBlocks.append(std::move(bd));
  }
  out["codeBlocks"] = std::move(codeBlocks);

  py::list bulletLines;
  for (int i : p.bulletLineNumbers) bulletLines.append(py::int_(i));
  out["bulletLineNumbers"] = std::move(bulletLines);

  py::list taskLines;
  for (int i : p.taskLineNumbers) taskLines.append(py::int_(i));
  out["taskLineNumbers"] = std::move(taskLines);

  py::list tables;
  for (const auto& t : p.tables) {
    py::dict td;
    td["startLine"] = t.startLine;
    td["raw"] = t.raw;
    tables.append(std::move(td));
  }
  out["tables"] = std::move(tables);

  return out;
}

static py::dict OutcomeToDict(const llm_structured::StreamOutcome<Json>& out) {
  py::dict d;
  d["done"] = out.done;
  d["ok"] = out.ok;
  if (out.value.has_value()) {
    d["value"] = ToPy(*out.value);
  } else {
    d["value"] = py::none();
  }
  if (out.error.has_value()) {
    d["error"] = MakeErrorObject(*out.error);
  } else {
    d["error"] = py::none();
  }
  return d;
}

static py::dict OutcomeToDict(const llm_structured::StreamOutcome<JsonArray>& out) {
  py::dict d;
  d["done"] = out.done;
  d["ok"] = out.ok;
  if (out.value.has_value()) {
    py::list arr;
    for (const auto& el : *out.value) arr.append(ToPy(el));
    d["value"] = std::move(arr);
  } else {
    d["value"] = py::none();
  }
  if (out.error.has_value()) {
    d["error"] = MakeErrorObject(*out.error);
  } else {
    d["error"] = py::none();
  }
  return d;
}

static py::dict OutcomeToDict(const llm_structured::StreamOutcome<SqlParsed>& out) {
  py::dict d;
  d["done"] = out.done;
  d["ok"] = out.ok;
  if (out.value.has_value()) {
    const auto& p = *out.value;
    py::dict obj;
    obj["sql"] = p.sql;
    obj["statementType"] = p.statementType;
    obj["hasWhere"] = p.hasWhere;
    obj["hasFrom"] = p.hasFrom;
    obj["hasLimit"] = p.hasLimit;
    obj["hasUnion"] = p.hasUnion;
    obj["hasComments"] = p.hasComments;
    obj["hasSubquery"] = p.hasSubquery;
    obj["limit"] = p.limit ? py::int_(*p.limit) : py::none();
    py::list tables;
    for (const auto& t : p.tables) tables.append(py::str(t));
    obj["tables"] = std::move(tables);
    d["value"] = std::move(obj);
  } else {
    d["value"] = py::none();
  }
  if (out.error.has_value()) {
    d["error"] = MakeErrorObject(*out.error);
  } else {
    d["error"] = py::none();
  }
  return d;
}

PYBIND11_MODULE(_native, m) {
  m.doc() = "C++17-backed structured output parsing/validation (pybind11)";

  ValidationErrorType = py::reinterpret_steal<py::object>(PyErr_NewException("llm_structured.ValidationError", PyExc_Exception, nullptr));
  m.attr("ValidationError") = ValidationErrorType;

  py::register_exception_translator([](std::exception_ptr p) {
    try {
      if (p) std::rethrow_exception(p);
    } catch (const ValidationError& e) {
      TranslateValidationError(e);
    }
  });

  m.def("json_pointer_from_path", &llm_structured::json_pointer_from_path);
  m.def("extract_json_candidate", &llm_structured::extract_json_candidate);
  m.def("extract_json_candidates", &llm_structured::extract_json_candidates);
  m.def("extract_sql_candidate", &llm_structured::extract_sql_candidate);

  m.def("loads_jsonish", [](const std::string& text) { return ToPy(llm_structured::loads_jsonish(text)); });

  m.def("loads_jsonish_all", [](const std::string& text) {
    return ToPy(Json(llm_structured::loads_jsonish_all(text)));
  });

  m.def("loads_jsonish_ex", [](const std::string& text, py::object repair) {
    RepairConfig cfg = RepairConfigFromPy(std::move(repair));
    auto r = llm_structured::loads_jsonish_ex(text, cfg);
    py::dict out;
    out["value"] = ToPy(r.value);
    out["fixed"] = r.fixed;
    out["metadata"] = RepairMetadataToPy(r.metadata);
    return out;
  }, py::arg("text"), py::arg("repair") = py::none());

  m.def("loads_jsonish_all_ex", [](const std::string& text, py::object repair) {
    RepairConfig cfg = RepairConfigFromPy(std::move(repair));
    auto r = llm_structured::loads_jsonish_all_ex(text, cfg);
    py::dict out;
    py::list values;
    py::list fixed;
    py::list meta;
    for (size_t i = 0; i < r.values.size(); ++i) {
      values.append(ToPy(r.values[i]));
      fixed.append(py::str(r.fixed[i]));
      meta.append(RepairMetadataToPy(r.metadata[i]));
    }
    out["values"] = std::move(values);
    out["fixed"] = std::move(fixed);
    out["metadata"] = std::move(meta);
    return out;
  }, py::arg("text"), py::arg("repair") = py::none());
  m.def("dumps_json", [](py::handle v) {
    Json j;
    if (!FromPy(v, j)) throw std::runtime_error("value must be JSON-serializable");
    return llm_structured::dumps_json(j);
  });

  m.def("validate_json_value", [](py::handle value, py::handle schema, const std::string& path) {
    Json v;
    if (!FromPy(value, v)) throw std::runtime_error("value must be JSON-serializable");
    Json s = SchemaFromPy(schema);
    llm_structured::validate(v, s, path);
  });

  m.def("validate_all_json_value", [](py::handle value, py::handle schema, const std::string& path) {
    Json v;
    if (!FromPy(value, v)) throw std::runtime_error("value must be JSON-serializable");
    Json s = SchemaFromPy(schema);
    auto errs = llm_structured::validate_all(v, s, path);
    py::list out;
    for (const auto& e : errs) out.append(MakeErrorObject(e));
    return out;
  });

  // ---- Validation repair suggestions ----

  auto ValidationRepairConfigFromPy = [](py::object obj) -> llm_structured::ValidationRepairConfig {
    llm_structured::ValidationRepairConfig cfg;
    if (!obj.is_none()) {
      py::dict d = obj.cast<py::dict>();
      if (d.contains("coerce_types")) cfg.coerce_types = d["coerce_types"].cast<bool>();
      if (d.contains("use_defaults")) cfg.use_defaults = d["use_defaults"].cast<bool>();
      if (d.contains("clamp_numbers")) cfg.clamp_numbers = d["clamp_numbers"].cast<bool>();
      if (d.contains("truncate_strings")) cfg.truncate_strings = d["truncate_strings"].cast<bool>();
      if (d.contains("truncate_arrays")) cfg.truncate_arrays = d["truncate_arrays"].cast<bool>();
      if (d.contains("remove_extra_properties")) cfg.remove_extra_properties = d["remove_extra_properties"].cast<bool>();
      if (d.contains("fix_enums")) cfg.fix_enums = d["fix_enums"].cast<bool>();
      if (d.contains("fix_formats")) cfg.fix_formats = d["fix_formats"].cast<bool>();
      if (d.contains("max_suggestions")) cfg.max_suggestions = d["max_suggestions"].cast<int>();
    }
    return cfg;
  };

  auto RepairSuggestionToPy = [](const llm_structured::RepairSuggestion& s) {
    py::dict d;
    d["path"] = s.path;
    d["error_kind"] = s.error_kind;
    d["message"] = s.message;
    d["suggestion"] = s.suggestion;
    d["original_value"] = ToPy(s.original_value);
    d["suggested_value"] = ToPy(s.suggested_value);
    d["auto_fixable"] = s.auto_fixable;
    return d;
  };

  auto ValidationRepairResultToPy = [&](const llm_structured::ValidationRepairResult& r) {
    py::dict out;
    out["valid"] = r.valid;
    out["fully_repaired"] = r.fully_repaired;
    out["repaired_value"] = ToPy(r.repaired_value);
    py::list sugg;
    for (const auto& s : r.suggestions) sugg.append(RepairSuggestionToPy(s));
    out["suggestions"] = std::move(sugg);
    py::list errs;
    for (const auto& e : r.unfixable_errors) errs.append(MakeErrorObject(e));
    out["unfixable_errors"] = std::move(errs);
    return out;
  };

  m.def("validate_with_repair", [ValidationRepairConfigFromPy, ValidationRepairResultToPy](
                                   py::handle value, py::handle schema, py::object config) {
    Json v;
    if (!FromPy(value, v)) throw std::runtime_error("value must be JSON-serializable");
    Json s = SchemaFromPy(schema);
    auto cfg = ValidationRepairConfigFromPy(std::move(config));
    auto r = llm_structured::validate_with_repair(v, s, cfg);
    return ValidationRepairResultToPy(r);
  },
        py::arg("value"),
        py::arg("schema"),
        py::arg("config") = py::none());

  m.def("parse_and_repair", [ValidationRepairConfigFromPy, ValidationRepairResultToPy](
                               const std::string& text, py::handle schema, py::object config, py::object parse_repair) {
    Json s = SchemaFromPy(schema);
    auto cfg = ValidationRepairConfigFromPy(std::move(config));
    RepairConfig pr = RepairConfigFromPy(std::move(parse_repair));
    auto r = llm_structured::parse_and_repair(text, s, cfg, pr);
    return ValidationRepairResultToPy(r);
  },
        py::arg("text"),
        py::arg("schema"),
        py::arg("config") = py::none(),
        py::arg("parse_repair") = py::none());

  m.def("parse_and_validate_json", [](const std::string& text, py::handle schema) {
    Json s = SchemaFromPy(schema);
    return ToPy(llm_structured::parse_and_validate(text, s));
  });

  m.def("parse_and_validate_json_all", [](const std::string& text, py::handle schema) {
    Json s = SchemaFromPy(schema);
    return ToPy(Json(llm_structured::parse_and_validate_all(text, s)));
  });

  m.def("parse_and_validate_json_ex", [](const std::string& text, py::handle schema, py::object repair) {
    Json s = SchemaFromPy(schema);
    RepairConfig cfg = RepairConfigFromPy(std::move(repair));
    auto r = llm_structured::parse_and_validate_ex(text, s, cfg);
    py::dict out;
    out["value"] = ToPy(r.value);
    out["fixed"] = r.fixed;
    out["metadata"] = RepairMetadataToPy(r.metadata);
    return out;
  }, py::arg("text"), py::arg("schema"), py::arg("repair") = py::none());

  m.def("parse_and_validate_json_all_ex", [](const std::string& text, py::handle schema, py::object repair) {
    Json s = SchemaFromPy(schema);
    RepairConfig cfg = RepairConfigFromPy(std::move(repair));
    auto r = llm_structured::parse_and_validate_all_ex(text, s, cfg);
    py::dict out;
    py::list values;
    py::list fixed;
    py::list meta;
    for (size_t i = 0; i < r.values.size(); ++i) {
      values.append(ToPy(r.values[i]));
      fixed.append(py::str(r.fixed[i]));
      meta.append(RepairMetadataToPy(r.metadata[i]));
    }
    out["values"] = std::move(values);
    out["fixed"] = std::move(fixed);
    out["metadata"] = std::move(meta);
    return out;
  }, py::arg("text"), py::arg("schema"), py::arg("repair") = py::none());

  m.def("parse_and_validate_json_with_defaults", [](const std::string& text, py::handle schema) {
    Json s = SchemaFromPy(schema);
    return ToPy(llm_structured::parse_and_validate_with_defaults(text, s));
  });

  m.def("parse_and_validate_json_with_defaults_ex", [](const std::string& text, py::handle schema, py::object repair) {
    Json s = SchemaFromPy(schema);
    RepairConfig cfg = RepairConfigFromPy(std::move(repair));
    auto r = llm_structured::parse_and_validate_with_defaults_ex(text, s, cfg);
    py::dict out;
    out["value"] = ToPy(r.value);
    out["fixed"] = r.fixed;
    out["metadata"] = RepairMetadataToPy(r.metadata);
    return out;
  }, py::arg("text"), py::arg("schema"), py::arg("repair") = py::none());

  m.def("validate_all_json", [](const std::string& text, py::handle schema) {
    Json s = SchemaFromPy(schema);
    Json v = llm_structured::loads_jsonish(text);
    auto errs = llm_structured::validate_all(v, s, "$");
    py::list out;
    for (const auto& e : errs) out.append(MakeErrorObject(e));
    return out;
  });

  m.def("parse_and_validate_sql", [](const std::string& text, py::handle schema) {
    Json s = SchemaFromPy(schema);
    SqlParsed p = llm_structured::parse_and_validate_sql(text, s);
    py::dict obj;
    obj["sql"] = p.sql;
    obj["statementType"] = p.statementType;
    obj["hasWhere"] = p.hasWhere;
    obj["hasFrom"] = p.hasFrom;
    obj["hasLimit"] = p.hasLimit;
    obj["hasUnion"] = p.hasUnion;
    obj["hasComments"] = p.hasComments;
    obj["hasSubquery"] = p.hasSubquery;
    obj["limit"] = p.limit ? py::int_(*p.limit) : py::none();
    py::list tables;
    for (const auto& t : p.tables) tables.append(py::str(t));
    obj["tables"] = std::move(tables);
    return obj;
  });

  m.def("parse_and_validate_kv", [](const std::string& text, py::handle schema) {
    Json s = SchemaFromPy(schema);
    KeyValue kv = llm_structured::parse_and_validate_kv(text, s);
    py::dict out;
    for (const auto& it : kv) out[py::str(it.first)] = py::str(it.second);
    return out;
  });

  m.def("parse_and_validate_markdown", [](const std::string& text, py::handle schema) {
    Json s = SchemaFromPy(schema);
    MarkdownParsed p = llm_structured::parse_and_validate_markdown(text, s);
    return MarkdownToDict(p);
  });

  // ---- YAML APIs ----

  m.def("extract_yaml_candidate", &llm_structured::extract_yaml_candidate);
  m.def("extract_yaml_candidates", &llm_structured::extract_yaml_candidates);

  m.def("loads_yamlish", [](const std::string& text) {
    return ToPy(llm_structured::loads_yamlish(text));
  });

  m.def("loads_yamlish_all", [](const std::string& text) {
    return ToPy(Json(llm_structured::loads_yamlish_all(text)));
  });

  m.def("loads_yamlish_ex", [](const std::string& text, py::object repair) {
    llm_structured::YamlRepairConfig cfg;
    if (!repair.is_none()) {
      py::dict d = repair.cast<py::dict>();
      auto set_bool = [&](const char* key, bool& field) {
        if (d.contains(key)) field = d[key].cast<bool>();
      };
      set_bool("fixTabs", cfg.fix_tabs);
      set_bool("normalizeIndentation", cfg.normalize_indentation);
      set_bool("fixUnquotedValues", cfg.fix_unquoted_values);
      set_bool("allowInlineJson", cfg.allow_inline_json);
      set_bool("quoteAmbiguousStrings", cfg.quote_ambiguous_strings);
    }
    auto r = llm_structured::loads_yamlish_ex(text, cfg);
    py::dict out;
    out["value"] = ToPy(r.value);
    out["fixed"] = r.fixed;
    py::dict meta;
    meta["extractedFromFence"] = r.metadata.extracted_from_fence;
    meta["fixedTabs"] = r.metadata.fixed_tabs;
    meta["normalizedIndentation"] = r.metadata.normalized_indentation;
    meta["fixedUnquotedValues"] = r.metadata.fixed_unquoted_values;
    meta["convertedInlineJson"] = r.metadata.converted_inline_json;
    meta["quotedAmbiguousStrings"] = r.metadata.quoted_ambiguous_strings;
    out["metadata"] = std::move(meta);
    return out;
  }, py::arg("text"), py::arg("repair") = py::none());

  m.def("loads_yamlish_all_ex", [](const std::string& text, py::object repair) {
    llm_structured::YamlRepairConfig cfg;
    if (!repair.is_none()) {
      py::dict d = repair.cast<py::dict>();
      auto set_bool = [&](const char* key, bool& field) {
        if (d.contains(key)) field = d[key].cast<bool>();
      };
      set_bool("fixTabs", cfg.fix_tabs);
      set_bool("normalizeIndentation", cfg.normalize_indentation);
      set_bool("fixUnquotedValues", cfg.fix_unquoted_values);
      set_bool("allowInlineJson", cfg.allow_inline_json);
      set_bool("quoteAmbiguousStrings", cfg.quote_ambiguous_strings);
    }
    auto r = llm_structured::loads_yamlish_all_ex(text, cfg);
    py::dict out;
    py::list values;
    py::list fixed;
    py::list meta_list;
    for (size_t i = 0; i < r.values.size(); ++i) {
      values.append(ToPy(r.values[i]));
      fixed.append(py::str(r.fixed[i]));
      py::dict meta;
      meta["extractedFromFence"] = r.metadata[i].extracted_from_fence;
      meta["fixedTabs"] = r.metadata[i].fixed_tabs;
      meta["normalizedIndentation"] = r.metadata[i].normalized_indentation;
      meta["fixedUnquotedValues"] = r.metadata[i].fixed_unquoted_values;
      meta["convertedInlineJson"] = r.metadata[i].converted_inline_json;
      meta["quotedAmbiguousStrings"] = r.metadata[i].quoted_ambiguous_strings;
      meta_list.append(std::move(meta));
    }
    out["values"] = std::move(values);
    out["fixed"] = std::move(fixed);
    out["metadata"] = std::move(meta_list);
    return out;
  }, py::arg("text"), py::arg("repair") = py::none());

  m.def("dumps_yaml", [](py::handle v, int indent) {
    Json j;
    if (!FromPy(v, j)) throw std::runtime_error("value must be JSON-serializable");
    return llm_structured::dumps_yaml(j, indent);
  }, py::arg("value"), py::arg("indent") = 2);

  m.def("parse_and_validate_yaml", [](const std::string& text, py::handle schema) {
    Json s = SchemaFromPy(schema);
    return ToPy(llm_structured::parse_and_validate_yaml(text, s));
  });

  m.def("parse_and_validate_yaml_all", [](const std::string& text, py::handle schema) {
    Json s = SchemaFromPy(schema);
    return ToPy(Json(llm_structured::parse_and_validate_yaml_all(text, s)));
  });

  m.def("parse_and_validate_yaml_ex", [](const std::string& text, py::handle schema, py::object repair) {
    Json s = SchemaFromPy(schema);
    llm_structured::YamlRepairConfig cfg;
    if (!repair.is_none()) {
      py::dict d = repair.cast<py::dict>();
      auto set_bool = [&](const char* key, bool& field) {
        if (d.contains(key)) field = d[key].cast<bool>();
      };
      set_bool("fixTabs", cfg.fix_tabs);
      set_bool("normalizeIndentation", cfg.normalize_indentation);
      set_bool("fixUnquotedValues", cfg.fix_unquoted_values);
      set_bool("allowInlineJson", cfg.allow_inline_json);
      set_bool("quoteAmbiguousStrings", cfg.quote_ambiguous_strings);
    }
    auto r = llm_structured::parse_and_validate_yaml_ex(text, s, cfg);
    py::dict out;
    out["value"] = ToPy(r.value);
    out["fixed"] = r.fixed;
    py::dict meta;
    meta["extractedFromFence"] = r.metadata.extracted_from_fence;
    meta["fixedTabs"] = r.metadata.fixed_tabs;
    meta["normalizedIndentation"] = r.metadata.normalized_indentation;
    meta["fixedUnquotedValues"] = r.metadata.fixed_unquoted_values;
    meta["convertedInlineJson"] = r.metadata.converted_inline_json;
    meta["quotedAmbiguousStrings"] = r.metadata.quoted_ambiguous_strings;
    out["metadata"] = std::move(meta);
    return out;
  }, py::arg("text"), py::arg("schema"), py::arg("repair") = py::none());

  m.def("parse_and_validate_yaml_all_ex", [](const std::string& text, py::handle schema, py::object repair) {
    Json s = SchemaFromPy(schema);
    llm_structured::YamlRepairConfig cfg;
    if (!repair.is_none()) {
      py::dict d = repair.cast<py::dict>();
      auto set_bool = [&](const char* key, bool& field) {
        if (d.contains(key)) field = d[key].cast<bool>();
      };
      set_bool("fixTabs", cfg.fix_tabs);
      set_bool("normalizeIndentation", cfg.normalize_indentation);
      set_bool("fixUnquotedValues", cfg.fix_unquoted_values);
      set_bool("allowInlineJson", cfg.allow_inline_json);
      set_bool("quoteAmbiguousStrings", cfg.quote_ambiguous_strings);
    }
    auto r = llm_structured::parse_and_validate_yaml_all_ex(text, s, cfg);
    py::dict out;
    py::list values;
    py::list fixed;
    py::list meta_list;
    for (size_t i = 0; i < r.values.size(); ++i) {
      values.append(ToPy(r.values[i]));
      fixed.append(py::str(r.fixed[i]));
      py::dict meta;
      meta["extractedFromFence"] = r.metadata[i].extracted_from_fence;
      meta["fixedTabs"] = r.metadata[i].fixed_tabs;
      meta["normalizedIndentation"] = r.metadata[i].normalized_indentation;
      meta["fixedUnquotedValues"] = r.metadata[i].fixed_unquoted_values;
      meta["convertedInlineJson"] = r.metadata[i].converted_inline_json;
      meta["quotedAmbiguousStrings"] = r.metadata[i].quoted_ambiguous_strings;
      meta_list.append(std::move(meta));
    }
    out["values"] = std::move(values);
    out["fixed"] = std::move(fixed);
    out["metadata"] = std::move(meta_list);
    return out;
  }, py::arg("text"), py::arg("schema"), py::arg("repair") = py::none());

  // ---- TOML-ish ----

  m.def("extract_toml_candidate", &llm_structured::extract_toml_candidate);
  m.def("extract_toml_candidates", &llm_structured::extract_toml_candidates);

  m.def("loads_tomlish", [](const std::string& text) {
    return ToPy(llm_structured::loads_tomlish(text));
  });

  m.def("loads_tomlish_all", [](const std::string& text) {
    return ToPy(Json(llm_structured::loads_tomlish_all(text)));
  });

  m.def("loads_tomlish_ex", [](const std::string& text, py::object repair) {
    llm_structured::TomlRepairConfig cfg;
    if (!repair.is_none()) {
      py::dict d = repair.cast<py::dict>();
      auto set_bool = [&](const char* key, bool& field) {
        if (d.contains(key)) field = d[key].cast<bool>();
      };
      set_bool("fixUnquotedStrings", cfg.fix_unquoted_strings);
      set_bool("allowSingleQuotes", cfg.allow_single_quotes);
      set_bool("normalizeWhitespace", cfg.normalize_whitespace);
      set_bool("fixTableNames", cfg.fix_table_names);
      set_bool("allowMultilineInlineTables", cfg.allow_multiline_inline_tables);
    }
    auto r = llm_structured::loads_tomlish_ex(text, cfg);
    py::dict out;
    out["value"] = ToPy(r.value);
    out["fixed"] = r.fixed;
    py::dict meta;
    meta["extractedFromFence"] = r.metadata.extracted_from_fence;
    meta["fixedUnquotedStrings"] = r.metadata.fixed_unquoted_strings;
    meta["convertedSingleQuotes"] = r.metadata.converted_single_quotes;
    meta["normalizedWhitespace"] = r.metadata.normalized_whitespace;
    meta["fixedTableNames"] = r.metadata.fixed_table_names;
    meta["convertedMultilineInline"] = r.metadata.converted_multiline_inline;
    out["metadata"] = std::move(meta);
    return out;
  }, py::arg("text"), py::arg("repair") = py::none());

  m.def("loads_tomlish_all_ex", [](const std::string& text, py::object repair) {
    llm_structured::TomlRepairConfig cfg;
    if (!repair.is_none()) {
      py::dict d = repair.cast<py::dict>();
      auto set_bool = [&](const char* key, bool& field) {
        if (d.contains(key)) field = d[key].cast<bool>();
      };
      set_bool("fixUnquotedStrings", cfg.fix_unquoted_strings);
      set_bool("allowSingleQuotes", cfg.allow_single_quotes);
      set_bool("normalizeWhitespace", cfg.normalize_whitespace);
      set_bool("fixTableNames", cfg.fix_table_names);
      set_bool("allowMultilineInlineTables", cfg.allow_multiline_inline_tables);
    }
    auto r = llm_structured::loads_tomlish_all_ex(text, cfg);
    py::dict out;
    py::list values;
    py::list fixed;
    py::list meta_list;
    for (size_t i = 0; i < r.values.size(); ++i) {
      values.append(ToPy(r.values[i]));
      fixed.append(py::str(r.fixed[i]));
      py::dict meta;
      meta["extractedFromFence"] = r.metadata[i].extracted_from_fence;
      meta["fixedUnquotedStrings"] = r.metadata[i].fixed_unquoted_strings;
      meta["convertedSingleQuotes"] = r.metadata[i].converted_single_quotes;
      meta["normalizedWhitespace"] = r.metadata[i].normalized_whitespace;
      meta["fixedTableNames"] = r.metadata[i].fixed_table_names;
      meta["convertedMultilineInline"] = r.metadata[i].converted_multiline_inline;
      meta_list.append(std::move(meta));
    }
    out["values"] = std::move(values);
    out["fixed"] = std::move(fixed);
    out["metadata"] = std::move(meta_list);
    return out;
  }, py::arg("text"), py::arg("repair") = py::none());

  m.def("dumps_toml", [](py::handle v) {
    Json j;
    if (!FromPy(v, j)) throw std::runtime_error("value must be JSON-serializable");
    return llm_structured::dumps_toml(j);
  }, py::arg("value"));

  m.def("parse_and_validate_toml", [](const std::string& text, py::handle schema) {
    Json s = SchemaFromPy(schema);
    return ToPy(llm_structured::parse_and_validate_toml(text, s));
  });

  m.def("parse_and_validate_toml_all", [](const std::string& text, py::handle schema) {
    Json s = SchemaFromPy(schema);
    return ToPy(Json(llm_structured::parse_and_validate_toml_all(text, s)));
  });

  m.def("parse_and_validate_toml_ex", [](const std::string& text, py::handle schema, py::object repair) {
    Json s = SchemaFromPy(schema);
    llm_structured::TomlRepairConfig cfg;
    if (!repair.is_none()) {
      py::dict d = repair.cast<py::dict>();
      auto set_bool = [&](const char* key, bool& field) {
        if (d.contains(key)) field = d[key].cast<bool>();
      };
      set_bool("fixUnquotedStrings", cfg.fix_unquoted_strings);
      set_bool("allowSingleQuotes", cfg.allow_single_quotes);
      set_bool("normalizeWhitespace", cfg.normalize_whitespace);
      set_bool("fixTableNames", cfg.fix_table_names);
      set_bool("allowMultilineInlineTables", cfg.allow_multiline_inline_tables);
    }
    auto r = llm_structured::parse_and_validate_toml_ex(text, s, cfg);
    py::dict out;
    out["value"] = ToPy(r.value);
    out["fixed"] = r.fixed;
    py::dict meta;
    meta["extractedFromFence"] = r.metadata.extracted_from_fence;
    meta["fixedUnquotedStrings"] = r.metadata.fixed_unquoted_strings;
    meta["convertedSingleQuotes"] = r.metadata.converted_single_quotes;
    meta["normalizedWhitespace"] = r.metadata.normalized_whitespace;
    meta["fixedTableNames"] = r.metadata.fixed_table_names;
    meta["convertedMultilineInline"] = r.metadata.converted_multiline_inline;
    out["metadata"] = std::move(meta);
    return out;
  }, py::arg("text"), py::arg("schema"), py::arg("repair") = py::none());

  m.def("parse_and_validate_toml_all_ex", [](const std::string& text, py::handle schema, py::object repair) {
    Json s = SchemaFromPy(schema);
    llm_structured::TomlRepairConfig cfg;
    if (!repair.is_none()) {
      py::dict d = repair.cast<py::dict>();
      auto set_bool = [&](const char* key, bool& field) {
        if (d.contains(key)) field = d[key].cast<bool>();
      };
      set_bool("fixUnquotedStrings", cfg.fix_unquoted_strings);
      set_bool("allowSingleQuotes", cfg.allow_single_quotes);
      set_bool("normalizeWhitespace", cfg.normalize_whitespace);
      set_bool("fixTableNames", cfg.fix_table_names);
      set_bool("allowMultilineInlineTables", cfg.allow_multiline_inline_tables);
    }
    auto r = llm_structured::parse_and_validate_toml_all_ex(text, s, cfg);
    py::dict out;
    py::list values;
    py::list fixed;
    py::list meta_list;
    for (size_t i = 0; i < r.values.size(); ++i) {
      values.append(ToPy(r.values[i]));
      fixed.append(py::str(r.fixed[i]));
      py::dict meta;
      meta["extractedFromFence"] = r.metadata[i].extracted_from_fence;
      meta["fixedUnquotedStrings"] = r.metadata[i].fixed_unquoted_strings;
      meta["convertedSingleQuotes"] = r.metadata[i].converted_single_quotes;
      meta["normalizedWhitespace"] = r.metadata[i].normalized_whitespace;
      meta["fixedTableNames"] = r.metadata[i].fixed_table_names;
      meta["convertedMultilineInline"] = r.metadata[i].converted_multiline_inline;
      meta_list.append(std::move(meta));
    }
    out["values"] = std::move(values);
    out["fixed"] = std::move(fixed);
    out["metadata"] = std::move(meta_list);
    return out;
  }, py::arg("text"), py::arg("schema"), py::arg("repair") = py::none());

  // ---- XML / HTML functions ----

  // Helper to convert XmlNode to Python dict
  auto XmlNodeToPy = [](const llm_structured::XmlNode& node, auto& self_ref) -> py::dict {
    py::dict d;
    std::string type_str;
    switch (node.type) {
      case llm_structured::XmlNode::Type::Element: type_str = "element"; break;
      case llm_structured::XmlNode::Type::Text: type_str = "text"; break;
      case llm_structured::XmlNode::Type::Comment: type_str = "comment"; break;
      case llm_structured::XmlNode::Type::CData: type_str = "cdata"; break;
      case llm_structured::XmlNode::Type::ProcessingInstruction: type_str = "processing_instruction"; break;
      case llm_structured::XmlNode::Type::Doctype: type_str = "doctype"; break;
    }
    d["type"] = type_str;
    d["name"] = node.name;
    d["text"] = node.text;
    py::dict attrs;
    for (const auto& kv : node.attributes) {
      attrs[py::str(kv.first)] = kv.second;
    }
    d["attributes"] = attrs;
    py::list children_list;
    for (const auto& child : node.children) {
      children_list.append(self_ref(child, self_ref));
    }
    d["children"] = children_list;
    return d;
  };

  // Helper to convert Python dict to XmlRepairConfig
  auto XmlRepairConfigFromPy = [](py::object obj) -> llm_structured::XmlRepairConfig {
    llm_structured::XmlRepairConfig cfg;
    if (!obj.is_none()) {
      py::dict d = obj.cast<py::dict>();
      if (d.contains("html_mode")) cfg.html_mode = d["html_mode"].cast<bool>();
      if (d.contains("fix_unquoted_attributes")) cfg.fix_unquoted_attributes = d["fix_unquoted_attributes"].cast<bool>();
      if (d.contains("auto_close_tags")) cfg.auto_close_tags = d["auto_close_tags"].cast<bool>();
      if (d.contains("normalize_whitespace")) cfg.normalize_whitespace = d["normalize_whitespace"].cast<bool>();
      if (d.contains("lowercase_names")) cfg.lowercase_names = d["lowercase_names"].cast<bool>();
      if (d.contains("decode_entities")) cfg.decode_entities = d["decode_entities"].cast<bool>();
    }
    return cfg;
  };

  // Helper to convert XmlRepairMetadata to Python dict
  auto XmlRepairMetadataToPy = [](const llm_structured::XmlRepairMetadata& meta) -> py::dict {
    py::dict d;
    d["extracted_from_fence"] = meta.extracted_from_fence;
    d["fixed_unquoted_attributes"] = meta.fixed_unquoted_attributes;
    d["auto_closed_tags"] = meta.auto_closed_tags;
    d["normalized_whitespace"] = meta.normalized_whitespace;
    d["lowercased_names"] = meta.lowercased_names;
    d["decoded_entities"] = meta.decoded_entities;
    d["unclosed_tag_count"] = meta.unclosed_tag_count;
    return d;
  };

  // Helper to convert Python dict to XmlNode (with self-reference for recursion)
  auto PyToXmlNode = [](py::dict d, auto& self_ref) -> llm_structured::XmlNode {
    llm_structured::XmlNode node;
    std::string type_str = d["type"].cast<std::string>();
    if (type_str == "element") node.type = llm_structured::XmlNode::Type::Element;
    else if (type_str == "text") node.type = llm_structured::XmlNode::Type::Text;
    else if (type_str == "comment") node.type = llm_structured::XmlNode::Type::Comment;
    else if (type_str == "cdata") node.type = llm_structured::XmlNode::Type::CData;
    else if (type_str == "processing_instruction") node.type = llm_structured::XmlNode::Type::ProcessingInstruction;
    else if (type_str == "doctype") node.type = llm_structured::XmlNode::Type::Doctype;
    
    if (d.contains("name")) node.name = d["name"].cast<std::string>();
    if (d.contains("text")) node.text = d["text"].cast<std::string>();
    if (d.contains("attributes")) {
      py::dict attrs = d["attributes"].cast<py::dict>();
      for (auto item : attrs) {
        node.attributes[item.first.cast<std::string>()] = item.second.cast<std::string>();
      }
    }
    if (d.contains("children")) {
      py::list children = d["children"].cast<py::list>();
      for (auto child : children) {
        node.children.push_back(self_ref(child.cast<py::dict>(), self_ref));
      }
    }
    return node;
  };

  m.def("extract_xml_candidate", [](const std::string& text) {
    return llm_structured::extract_xml_candidate(text);
  }, py::arg("text"));

  m.def("extract_xml_candidates", [](const std::string& text) {
    return llm_structured::extract_xml_candidates(text);
  }, py::arg("text"));

  m.def("loads_xml", [XmlNodeToPy](const std::string& xml_string) {
    try {
      auto result = llm_structured::loads_xml(xml_string);
      py::dict out;
      out["ok"] = true;
      out["error"] = "";
      out["root"] = XmlNodeToPy(result, XmlNodeToPy);
      return out;
    } catch (const std::exception& e) {
      py::dict out;
      out["ok"] = false;
      out["error"] = e.what();
      out["root"] = py::none();
      return out;
    }
  }, py::arg("xml_string"));

  m.def("loads_xml_ex", [XmlNodeToPy, XmlRepairConfigFromPy, XmlRepairMetadataToPy](const std::string& xml_string, py::object repair) {
    try {
      auto cfg = XmlRepairConfigFromPy(repair);
      auto result = llm_structured::loads_xml_ex(xml_string, cfg);
      py::dict out;
      out["ok"] = true;
      out["error"] = "";
      out["root"] = XmlNodeToPy(result.root, XmlNodeToPy);
      out["fixed"] = result.fixed;
      out["metadata"] = XmlRepairMetadataToPy(result.metadata);
      return out;
    } catch (const std::exception& e) {
      py::dict out;
      out["ok"] = false;
      out["error"] = e.what();
      out["root"] = py::none();
      out["fixed"] = "";
      out["metadata"] = py::dict();
      return out;
    }
  }, py::arg("xml_string"), py::arg("repair") = py::none());

  m.def("loads_html", [XmlNodeToPy](const std::string& html_string) {
    try {
      auto result = llm_structured::loads_html(html_string);
      py::dict out;
      out["ok"] = true;
      out["error"] = "";
      out["root"] = XmlNodeToPy(result, XmlNodeToPy);
      return out;
    } catch (const std::exception& e) {
      py::dict out;
      out["ok"] = false;
      out["error"] = e.what();
      out["root"] = py::none();
      return out;
    }
  }, py::arg("html_string"));

  m.def("loads_html_ex", [XmlNodeToPy, XmlRepairConfigFromPy, XmlRepairMetadataToPy](const std::string& html_string, py::object repair) {
    try {
      auto cfg = XmlRepairConfigFromPy(repair);
      auto result = llm_structured::loads_html_ex(html_string, cfg);
      py::dict out;
      out["ok"] = true;
      out["error"] = "";
      out["root"] = XmlNodeToPy(result.root, XmlNodeToPy);
      out["fixed"] = result.fixed;
      out["metadata"] = XmlRepairMetadataToPy(result.metadata);
      return out;
    } catch (const std::exception& e) {
      py::dict out;
      out["ok"] = false;
      out["error"] = e.what();
      out["root"] = py::none();
      out["fixed"] = "";
      out["metadata"] = py::dict();
      return out;
    }
  }, py::arg("html_string"), py::arg("repair") = py::none());

  m.def("xml_to_json", [](const std::string& xml_string) -> py::object {
    try {
      auto node = llm_structured::loads_xml(xml_string);
      Json j = llm_structured::xml_to_json(node);
      return ToPy(j);
    } catch (...) {
      return py::none();
    }
  }, py::arg("xml_string"));

  m.def("loads_xml_as_json", [](const std::string& xml_string) -> py::object {
    auto result = llm_structured::loads_xml_as_json(xml_string);
    if (result.is_null()) return py::none();
    return ToPy(result);
  }, py::arg("xml_string"));

  m.def("loads_html_as_json", [](const std::string& html_string) -> py::object {
    auto result = llm_structured::loads_html_as_json(html_string);
    if (result.is_null()) return py::none();
    return ToPy(result);
  }, py::arg("html_string"));

  m.def("dumps_xml", [PyToXmlNode](py::handle node, int indent) {
    llm_structured::XmlNode xml_node = PyToXmlNode(node.cast<py::dict>(), PyToXmlNode);
    return llm_structured::dumps_xml(xml_node, indent);
  }, py::arg("node"), py::arg("indent") = 2);

  m.def("dumps_html", [PyToXmlNode](py::handle node, int indent) {
    llm_structured::XmlNode xml_node = PyToXmlNode(node.cast<py::dict>(), PyToXmlNode);
    return llm_structured::dumps_html(xml_node, indent);
  }, py::arg("node"), py::arg("indent") = 2);

  m.def("query_xml", [XmlNodeToPy, PyToXmlNode](py::handle node, const std::string& selector) {
    llm_structured::XmlNode xml_node = PyToXmlNode(node.cast<py::dict>(), PyToXmlNode);
    auto results = llm_structured::query_xml(xml_node, selector);
    py::list out;
    for (const auto* r : results) {
      out.append(XmlNodeToPy(*r, XmlNodeToPy));
    }
    return out;
  }, py::arg("node"), py::arg("selector"));

  m.def("xml_text_content", [PyToXmlNode](py::handle node) {
    llm_structured::XmlNode xml_node = PyToXmlNode(node.cast<py::dict>(), PyToXmlNode);
    return llm_structured::xml_text_content(xml_node);
  }, py::arg("node"));

  m.def("xml_get_attribute", [PyToXmlNode](py::handle node, const std::string& attr_name) -> py::object {
    llm_structured::XmlNode xml_node = PyToXmlNode(node.cast<py::dict>(), PyToXmlNode);
    std::string result = llm_structured::xml_get_attribute(xml_node, attr_name);
    if (!result.empty()) return py::cast(result);
    // Check if attribute exists but is empty
    auto it = xml_node.attributes.find(attr_name);
    if (it != xml_node.attributes.end()) return py::cast(result);
    return py::none();
  }, py::arg("node"), py::arg("attr_name"));

  m.def("validate_xml", [PyToXmlNode](py::handle node, py::handle schema) {
    llm_structured::XmlNode xml_node = PyToXmlNode(node.cast<py::dict>(), PyToXmlNode);
    Json s = SchemaFromPy(schema);
    py::dict out;
    py::list errors;
    try {
      llm_structured::validate_xml(xml_node, s, "$");
      out["ok"] = true;
    } catch (const ValidationError& e) {
      out["ok"] = false;
      py::dict err;
      err["path"] = e.path;
      err["message"] = e.what();
      errors.append(std::move(err));
    } catch (const std::exception& e) {
      out["ok"] = false;
      py::dict err;
      err["path"] = "$";
      err["message"] = e.what();
      errors.append(std::move(err));
    }
    out["errors"] = errors;
    return out;
  }, py::arg("node"), py::arg("schema"));

  m.def("parse_and_validate_xml", [XmlNodeToPy](const std::string& xml_string, py::handle schema) {
    Json s = SchemaFromPy(schema);
    py::dict out;
    py::list errors;
    try {
      auto result = llm_structured::parse_and_validate_xml(xml_string, s);
      out["ok"] = true;
      out["error"] = "";
      out["root"] = XmlNodeToPy(result, XmlNodeToPy);
    } catch (const ValidationError& e) {
      out["ok"] = false;
      out["error"] = "";
      // Parse succeeded, validation failed
      try {
        auto node = llm_structured::loads_xml(xml_string);
        out["root"] = XmlNodeToPy(node, XmlNodeToPy);
      } catch (...) {
        out["root"] = py::none();
      }
      py::dict err;
      err["path"] = e.path;
      err["message"] = e.what();
      errors.append(std::move(err));
    } catch (const std::exception& e) {
      out["ok"] = false;
      out["error"] = e.what();
      out["root"] = py::none();
    }
    out["validation_errors"] = errors;
    return out;
  }, py::arg("xml_string"), py::arg("schema"));

  m.def("parse_and_validate_xml_ex", [XmlNodeToPy, XmlRepairConfigFromPy, XmlRepairMetadataToPy](const std::string& xml_string, py::handle schema, py::object repair) {
    Json s = SchemaFromPy(schema);
    auto cfg = XmlRepairConfigFromPy(repair);
    py::dict out;
    py::list errors;
    try {
      auto result = llm_structured::parse_and_validate_xml_ex(xml_string, s, cfg);
      out["ok"] = true;
      out["error"] = "";
      out["root"] = XmlNodeToPy(result.root, XmlNodeToPy);
      out["metadata"] = XmlRepairMetadataToPy(result.metadata);
    } catch (const ValidationError& e) {
      out["ok"] = false;
      out["error"] = "";
      // Parse succeeded, validation failed
      try {
        auto parse_result = llm_structured::loads_xml_ex(xml_string, cfg);
        out["root"] = XmlNodeToPy(parse_result.root, XmlNodeToPy);
        out["metadata"] = XmlRepairMetadataToPy(parse_result.metadata);
      } catch (...) {
        out["root"] = py::none();
        out["metadata"] = py::dict();
      }
      py::dict err;
      err["path"] = e.path;
      err["message"] = e.what();
      errors.append(std::move(err));
    } catch (const std::exception& e) {
      out["ok"] = false;
      out["error"] = e.what();
      out["root"] = py::none();
      out["metadata"] = py::dict();
    }
    out["validation_errors"] = errors;
    return out;
  }, py::arg("xml_string"), py::arg("schema"), py::arg("repair") = py::none());

  // ---- Streaming classes ----

  py::class_<llm_structured::JsonStreamParser>(m, "JsonStreamParser")
      .def(py::init([](py::handle schema, py::object limits) {
        Json s = SchemaFromPy(schema);
        size_t max_buffer = 0;
        if (!limits.is_none()) {
          py::dict d = limits.cast<py::dict>();
          if (d.contains("maxBufferBytes")) max_buffer = d["maxBufferBytes"].cast<size_t>();
        }
        if (max_buffer > 0) return new llm_structured::JsonStreamParser(std::move(s), max_buffer);
        return new llm_structured::JsonStreamParser(std::move(s));
      }), py::arg("schema"), py::arg("limits") = py::none())
      .def("reset", &llm_structured::JsonStreamParser::reset)
      .def("finish", &llm_structured::JsonStreamParser::finish)
      .def("append", &llm_structured::JsonStreamParser::append)
      .def("poll", [](llm_structured::JsonStreamParser& self) { return OutcomeToDict(self.poll()); })
      .def("location", [](llm_structured::JsonStreamParser& self) { return StreamLocationToPy(self.location()); });

  py::class_<llm_structured::JsonStreamCollector>(m, "JsonStreamCollector")
      .def(py::init([](py::handle schema, py::object limits) {
        Json s = SchemaFromPy(schema);
        size_t max_buffer = 0;
        size_t max_items = 0;
        if (!limits.is_none()) {
          py::dict d = limits.cast<py::dict>();
          if (d.contains("maxBufferBytes")) max_buffer = d["maxBufferBytes"].cast<size_t>();
          if (d.contains("maxItems")) max_items = d["maxItems"].cast<size_t>();
        }
        if (max_buffer > 0 || max_items > 0) return new llm_structured::JsonStreamCollector(std::move(s), max_buffer, max_items);
        return new llm_structured::JsonStreamCollector(std::move(s));
      }), py::arg("item_schema"), py::arg("limits") = py::none())
      .def("reset", &llm_structured::JsonStreamCollector::reset)
      .def("append", &llm_structured::JsonStreamCollector::append)
      .def("close", &llm_structured::JsonStreamCollector::close)
      .def("poll", [](llm_structured::JsonStreamCollector& self) { return OutcomeToDict(self.poll()); })
      .def("location", [](llm_structured::JsonStreamCollector& self) { return StreamLocationToPy(self.location()); });

  py::class_<llm_structured::JsonStreamBatchCollector>(m, "JsonStreamBatchCollector")
      .def(py::init([](py::handle schema, py::object limits) {
        Json s = SchemaFromPy(schema);
        size_t max_buffer = 0;
        size_t max_items = 0;
        if (!limits.is_none()) {
          py::dict d = limits.cast<py::dict>();
          if (d.contains("maxBufferBytes")) max_buffer = d["maxBufferBytes"].cast<size_t>();
          if (d.contains("maxItems")) max_items = d["maxItems"].cast<size_t>();
        }
        if (max_buffer > 0 || max_items > 0) return new llm_structured::JsonStreamBatchCollector(std::move(s), max_buffer, max_items);
        return new llm_structured::JsonStreamBatchCollector(std::move(s));
      }), py::arg("item_schema"), py::arg("limits") = py::none())
      .def("reset", &llm_structured::JsonStreamBatchCollector::reset)
      .def("append", &llm_structured::JsonStreamBatchCollector::append)
      .def("close", &llm_structured::JsonStreamBatchCollector::close)
      .def("poll", [](llm_structured::JsonStreamBatchCollector& self) { return OutcomeToDict(self.poll()); })
      .def("location", [](llm_structured::JsonStreamBatchCollector& self) { return StreamLocationToPy(self.location()); });

  py::class_<llm_structured::JsonStreamValidatedBatchCollector>(m, "JsonStreamValidatedBatchCollector")
      .def(py::init([](py::handle schema, py::object limits) {
        Json s = SchemaFromPy(schema);
        size_t max_buffer = 0;
        size_t max_items = 0;
        if (!limits.is_none()) {
          py::dict d = limits.cast<py::dict>();
          if (d.contains("maxBufferBytes")) max_buffer = d["maxBufferBytes"].cast<size_t>();
          if (d.contains("maxItems")) max_items = d["maxItems"].cast<size_t>();
        }
        if (max_buffer > 0 || max_items > 0)
          return new llm_structured::JsonStreamValidatedBatchCollector(std::move(s), max_buffer, max_items);
        return new llm_structured::JsonStreamValidatedBatchCollector(std::move(s));
      }), py::arg("item_schema"), py::arg("limits") = py::none())
      .def("reset", &llm_structured::JsonStreamValidatedBatchCollector::reset)
      .def("append", &llm_structured::JsonStreamValidatedBatchCollector::append)
      .def("close", &llm_structured::JsonStreamValidatedBatchCollector::close)
      .def("poll", [](llm_structured::JsonStreamValidatedBatchCollector& self) { return OutcomeToDict(self.poll()); })
      .def("location", [](llm_structured::JsonStreamValidatedBatchCollector& self) { return StreamLocationToPy(self.location()); });

  py::class_<llm_structured::SqlStreamParser>(m, "SqlStreamParser")
      .def(py::init([](py::handle schema, py::object limits) {
        Json s = SchemaFromPy(schema);
        size_t max_buffer = 0;
        if (!limits.is_none()) {
          py::dict d = limits.cast<py::dict>();
          if (d.contains("maxBufferBytes")) max_buffer = d["maxBufferBytes"].cast<size_t>();
        }
        if (max_buffer > 0) return new llm_structured::SqlStreamParser(std::move(s), max_buffer);
        return new llm_structured::SqlStreamParser(std::move(s));
      }), py::arg("schema"), py::arg("limits") = py::none())
      .def("reset", &llm_structured::SqlStreamParser::reset)
      .def("finish", &llm_structured::SqlStreamParser::finish)
      .def("append", &llm_structured::SqlStreamParser::append)
      .def("poll", [](llm_structured::SqlStreamParser& self) { return OutcomeToDict(self.poll()); })
      .def("location", [](llm_structured::SqlStreamParser& self) { return StreamLocationToPy(self.location()); });

  // ---- Schema Inference ----

  // Helper to convert Python dict to SchemaInferenceConfig
  auto SchemaInferenceConfigFromPy = [](py::object obj) -> llm_structured::SchemaInferenceConfig {
    llm_structured::SchemaInferenceConfig cfg;
    if (!obj.is_none()) {
      py::dict d = obj.cast<py::dict>();
      if (d.contains("include_examples")) cfg.include_examples = d["include_examples"].cast<bool>();
      if (d.contains("max_examples")) cfg.max_examples = d["max_examples"].cast<int>();
      if (d.contains("include_default")) cfg.include_default = d["include_default"].cast<bool>();
      if (d.contains("infer_formats")) cfg.infer_formats = d["infer_formats"].cast<bool>();
      if (d.contains("infer_patterns")) cfg.infer_patterns = d["infer_patterns"].cast<bool>();
      if (d.contains("infer_numeric_ranges")) cfg.infer_numeric_ranges = d["infer_numeric_ranges"].cast<bool>();
      if (d.contains("infer_string_lengths")) cfg.infer_string_lengths = d["infer_string_lengths"].cast<bool>();
      if (d.contains("infer_array_lengths")) cfg.infer_array_lengths = d["infer_array_lengths"].cast<bool>();
      if (d.contains("required_by_default")) cfg.required_by_default = d["required_by_default"].cast<bool>();
      if (d.contains("strict_additional_properties")) cfg.strict_additional_properties = d["strict_additional_properties"].cast<bool>();
      if (d.contains("prefer_integer")) cfg.prefer_integer = d["prefer_integer"].cast<bool>();
      if (d.contains("allow_any_of")) cfg.allow_any_of = d["allow_any_of"].cast<bool>();
      if (d.contains("include_descriptions")) cfg.include_descriptions = d["include_descriptions"].cast<bool>();
      if (d.contains("detect_enums")) cfg.detect_enums = d["detect_enums"].cast<bool>();
      if (d.contains("max_enum_values")) cfg.max_enum_values = d["max_enum_values"].cast<int>();
    }
    return cfg;
  };

  m.def("infer_schema", [SchemaInferenceConfigFromPy](py::handle value, py::object config) {
    Json v;
    if (!FromPy(value, v)) {
      throw std::runtime_error("value must be a JSON-serializable dict/list/primitive");
    }
    auto cfg = SchemaInferenceConfigFromPy(config);
    Json schema = llm_structured::infer_schema(v, cfg);
    return ToPy(schema);
  }, py::arg("value"), py::arg("config") = py::none());

  m.def("infer_schema_from_values", [SchemaInferenceConfigFromPy](py::handle values, py::object config) {
    JsonArray arr;
    for (auto item : values.cast<py::list>()) {
      Json v;
      if (!FromPy(item, v)) {
        throw std::runtime_error("all values must be JSON-serializable");
      }
      arr.push_back(v);
    }
    auto cfg = SchemaInferenceConfigFromPy(config);
    Json schema = llm_structured::infer_schema_from_values(arr, cfg);
    return ToPy(schema);
  }, py::arg("values"), py::arg("config") = py::none());

  m.def("merge_schemas", [SchemaInferenceConfigFromPy](py::handle schema1, py::handle schema2, py::object config) {
    Json s1 = SchemaFromPy(schema1);
    Json s2 = SchemaFromPy(schema2);
    auto cfg = SchemaInferenceConfigFromPy(config);
    Json merged = llm_structured::merge_schemas(s1, s2, cfg);
    return ToPy(merged);
  }, py::arg("schema1"), py::arg("schema2"), py::arg("config") = py::none());
}
