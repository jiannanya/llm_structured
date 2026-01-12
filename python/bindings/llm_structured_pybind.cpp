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
}
