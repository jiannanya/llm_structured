// Thin TS wrapper around the native addon.
// For production, consider a more robust loader (node-gyp-build) and better error mapping.

import path from "node:path";

// Loads build/Release/addon.node (or Debug if you build that way).
// __dirname at runtime is dist/src, so we go up two.
// eslint-disable-next-line @typescript-eslint/no-var-requires
const native = require(path.join(__dirname, "..", "..", "build", "Release", "addon.node")) as {
  parseAndValidateJson(text: string, schemaJson: string): JsonValue;
  parseAndValidateJsonWithDefaults(text: string, schemaJson: string): JsonValue;

  extractJsonCandidates(text: string): string[];
  extractYamlCandidate(text: string): string;
  extractYamlCandidates(text: string): string[];
  extractTomlCandidate(text: string): string;
  extractTomlCandidates(text: string): string[];

  loadsJsonishEx(text: string, repair?: RepairConfig): JsonishParseResult;
  loadsJsonishAllEx(text: string, repair?: RepairConfig): JsonishParseAllResult;
  loadsYamlishEx(text: string, repair?: YamlRepairConfig): YamlishParseResult;
  loadsYamlishAllEx(text: string, repair?: YamlRepairConfig): YamlishParseAllResult;
  loadsTomlishEx(text: string, repair?: TomlRepairConfig): TomlishParseResult;
  loadsTomlishAllEx(text: string, repair?: TomlRepairConfig): TomlishParseAllResult;
  parseAndValidateJsonEx(text: string, schemaJson: string, repair?: RepairConfig): JsonishParseResult;
  parseAndValidateJsonAllEx(text: string, schemaJson: string, repair?: RepairConfig): JsonishParseAllResult;
  parseAndValidateJsonWithDefaultsEx(text: string, schemaJson: string, repair?: RepairConfig): JsonishParseResult;
  parseAndValidateYaml(text: string, schemaJson: string): JsonValue;
  parseAndValidateYamlEx(text: string, schemaJson: string, repair?: YamlRepairConfig): YamlishParseResult;
  parseAndValidateYamlAllEx(text: string, schemaJson: string, repair?: YamlRepairConfig): YamlishParseAllResult;
  parseAndValidateToml(text: string, schemaJson: string): JsonValue;
  parseAndValidateTomlEx(text: string, schemaJson: string, repair?: TomlRepairConfig): TomlishParseResult;
  parseAndValidateTomlAllEx(text: string, schemaJson: string, repair?: TomlRepairConfig): TomlishParseAllResult;
  dumpsYaml(value: JsonValue, indent?: number): string;
  dumpsToml(value: JsonValue): string;

  // XML / HTML functions
  extractXmlCandidate(text: string, rootTag?: string): string;
  extractXmlCandidates(text: string, rootTag?: string): string[];
  loadsXml(xmlString: string): XmlParseResult;
  loadsXmlEx(xmlString: string, repair?: XmlRepairConfig): XmlParseResultEx;
  loadsHtml(htmlString: string): XmlParseResult;
  loadsHtmlEx(htmlString: string, repair?: XmlRepairConfig): XmlParseResultEx;
  loadsXmlAsJson(xmlString: string): JsonValue;
  loadsHtmlAsJson(htmlString: string): JsonValue;
  dumpsXml(node: XmlNode, indent?: number): string;
  dumpsHtml(node: XmlNode, indent?: number): string;
  queryXml(node: XmlNode, selector: string): XmlNode[];
  xmlTextContent(node: XmlNode): string;
  xmlGetAttribute(node: XmlNode, attrName: string): string | null;
  validateXml(node: XmlNode, schemaJson: string): XmlValidationResult;
  parseAndValidateXml(xmlString: string, schemaJson: string): XmlParseAndValidateResult;
  parseAndValidateXmlEx(xmlString: string, schemaJson: string, repair?: XmlRepairConfig): XmlParseAndValidateResultEx;

  validateAllJson(text: string, schemaJson: string): Array<NativeValidationError & { jsonPointer?: string }>;
  validateAllJsonValue(value: JsonValue, schemaJson: string): Array<NativeValidationError & { jsonPointer?: string }>;
  parseAndValidateSql(sqlText: string, schemaJson: string): SqlParsed;
  parseAndValidateMarkdown(
    markdownText: string,
    schemaJson: string
  ): { headingCount: number; codeBlockCount: number; tableCount: number; taskCount: number };
  parseAndValidateKv(text: string, schemaJson: string): Record<string, string>;

  // Streaming externals
  createJsonStreamParser(schemaJson: string, limits?: StreamLimits): unknown;
  jsonStreamParserReset(parser: unknown): void;
  jsonStreamParserFinish(parser: unknown): void;
  jsonStreamParserAppend(parser: unknown, chunk: string): void;
  jsonStreamParserPoll(parser: unknown): { done: boolean; ok: boolean; value: JsonValue | null; error: NativeValidationError | null };
  jsonStreamParserLocation(parser: unknown): StreamLocation;

  createJsonStreamCollector(schemaJson: string, limits?: StreamLimits): unknown;
  jsonStreamCollectorReset(collector: unknown): void;
  jsonStreamCollectorAppend(collector: unknown, chunk: string): void;
  jsonStreamCollectorClose(collector: unknown): void;
  jsonStreamCollectorPoll(collector: unknown): {
    done: boolean;
    ok: boolean;
    value: JsonValue[] | null;
    error: NativeValidationError | null;
  };
  jsonStreamCollectorLocation(collector: unknown): StreamLocation;

  createJsonStreamBatchCollector(schemaJson: string, limits?: StreamLimits): unknown;
  jsonStreamBatchCollectorReset(collector: unknown): void;
  jsonStreamBatchCollectorAppend(collector: unknown, chunk: string): void;
  jsonStreamBatchCollectorClose(collector: unknown): void;
  jsonStreamBatchCollectorPoll(collector: unknown): {
    done: boolean;
    ok: boolean;
    value: JsonValue[] | null;
    error: NativeValidationError | null;
  };
  jsonStreamBatchCollectorLocation(collector: unknown): StreamLocation;

  createJsonStreamValidatedBatchCollector(schemaJson: string, limits?: StreamLimits): unknown;
  jsonStreamValidatedBatchCollectorReset(collector: unknown): void;
  jsonStreamValidatedBatchCollectorAppend(collector: unknown, chunk: string): void;
  jsonStreamValidatedBatchCollectorClose(collector: unknown): void;
  jsonStreamValidatedBatchCollectorPoll(collector: unknown): {
    done: boolean;
    ok: boolean;
    value: JsonValue[] | null;
    error: NativeValidationError | null;
  };
  jsonStreamValidatedBatchCollectorLocation(collector: unknown): StreamLocation;

  createSqlStreamParser(schemaJson: string, limits?: StreamLimits): unknown;
  sqlStreamParserReset(parser: unknown): void;
  sqlStreamParserFinish(parser: unknown): void;
  sqlStreamParserAppend(parser: unknown, chunk: string): void;
  sqlStreamParserPoll(parser: unknown): { done: boolean; ok: boolean; value: SqlParsed | null; error: NativeValidationError | null };
  sqlStreamParserLocation(parser: unknown): StreamLocation;

  // Schema Inference
  inferSchema(value: JsonValue, config?: SchemaInferenceConfig): JsonObject;
  inferSchemaFromValues(values: JsonValue[], config?: SchemaInferenceConfig): JsonObject;
  mergeSchemas(schema1: JsonObject, schema2: JsonObject, config?: SchemaInferenceConfig): JsonObject;
};

export type JsonPrimitive = null | boolean | number | string;

export interface JsonObject {
  [k: string]: JsonValue;
}

export interface JsonArray extends ReadonlyArray<JsonValue> {}

export type JsonValue = JsonPrimitive | JsonArray | JsonObject;

// ---- Schema types (mirrors test5/test1/llm_structured.py expectations) ----

export type ErrorKind = "schema" | "type" | "limit" | "parse";

export interface StreamLocation {
  // Byte offset within the current internal buffer.
  offset: number;
  // 1-based.
  line: number;
  // 1-based.
  col: number;
}

export interface RepairConfig {
  fixSmartQuotes?: boolean;
  stripJsonComments?: boolean;
  replacePythonLiterals?: boolean;
  convertKvObjectToJson?: boolean;
  quoteUnquotedKeys?: boolean;
  dropTrailingCommas?: boolean;
  allowSingleQuotes?: boolean;

  // How to handle duplicate keys within JSON objects.
  // - "firstWins" (default): keep the first value
  // - "lastWins": overwrite with the last value
  // - "error": reject the payload
  duplicateKeyPolicy?: "firstWins" | "lastWins" | "error";
}

export interface RepairMetadata {
  extractedFromFence: boolean;
  fixedSmartQuotes: boolean;
  strippedComments: boolean;
  replacedPythonLiterals: boolean;
  convertedKvObject: boolean;
  quotedUnquotedKeys: boolean;
  droppedTrailingCommas: boolean;

  // Number of duplicate keys encountered while parsing objects.
  duplicateKeyCount: number;

  // Which duplicate key policy was applied during parsing.
  duplicateKeyPolicy: "firstWins" | "lastWins" | "error";
}

export interface YamlRepairConfig {
  fixTabs?: boolean;
  normalizeIndentation?: boolean;
  fixUnquotedValues?: boolean;
  allowInlineJson?: boolean;
  quoteAmbiguousStrings?: boolean;
}

export interface YamlRepairMetadata {
  extractedFromFence: boolean;
  fixedTabs: boolean;
  normalizedIndentation: boolean;
  fixedUnquotedValues: boolean;
  convertedInlineJson: boolean;
  quotedAmbiguousStrings: boolean;
}

export interface YamlishParseResult {
  value: JsonValue;
  fixed: string;
  metadata: YamlRepairMetadata;
}

export interface YamlishParseAllResult {
  values: JsonValue[];
  fixed: string[];
  metadata: YamlRepairMetadata[];
}

export interface TomlRepairConfig {
  fixUnquotedStrings?: boolean;
  allowSingleQuotes?: boolean;
  normalizeWhitespace?: boolean;
  fixTableNames?: boolean;
  allowMultilineInlineTables?: boolean;
}

export interface TomlRepairMetadata {
  extractedFromFence: boolean;
  fixedUnquotedStrings: boolean;
  convertedSingleQuotes: boolean;
  normalizedWhitespace: boolean;
  fixedTableNames: boolean;
  convertedMultilineInline: boolean;
}

export interface TomlishParseResult {
  value: JsonValue;
  fixed: string;
  metadata: TomlRepairMetadata;
}

export interface TomlishParseAllResult {
  values: JsonValue[];
  fixed: string[];
  metadata: TomlRepairMetadata[];
}

// ---- XML / HTML types ----

export type XmlNodeType = 'element' | 'text' | 'comment' | 'cdata' | 'processing_instruction' | 'doctype';

export interface XmlNode {
  type: XmlNodeType;
  name: string;
  text: string;
  attributes: Record<string, string>;
  children: XmlNode[];
}

export interface XmlRepairConfig {
  html_mode?: boolean;
  fix_unquoted_attributes?: boolean;
  auto_close_tags?: boolean;
  normalize_whitespace?: boolean;
  lowercase_names?: boolean;
  decode_entities?: boolean;
}

export interface XmlRepairMetadata {
  auto_closed_tags: number;
  fixed_attributes: number;
  decoded_entities: number;
  normalized_whitespace: number;
}

export interface XmlParseResult {
  ok: boolean;
  error: string;
  root: XmlNode | null;
}

export interface XmlParseResultEx {
  ok: boolean;
  error: string;
  root: XmlNode | null;
  metadata: XmlRepairMetadata;
}

export interface XmlValidationError {
  path: string;
  message: string;
}

export interface XmlValidationResult {
  ok: boolean;
  errors: XmlValidationError[];
}

export interface XmlParseAndValidateResult {
  ok: boolean;
  error: string;
  root: XmlNode | null;
  validation_errors: XmlValidationError[];
}

export interface XmlParseAndValidateResultEx {
  ok: boolean;
  error: string;
  root: XmlNode | null;
  validation_errors: XmlValidationError[];
  metadata: XmlRepairMetadata;
}

export interface XmlSchema {
  element?: string;
  requiredAttributes?: string[];
  attributes?: Record<string, {
    pattern?: string;
    enum?: string[];
  }>;
  children?: {
    minItems?: number;
    maxItems?: number;
    required?: string[];
  };
  childSchema?: Record<string, XmlSchema>;
}

// ---- Schema Inference types ----

export interface SchemaInferenceConfig {
  /** Include "examples" array with sample values (up to maxExamples) */
  includeExamples?: boolean;
  /** Maximum number of examples to include (default: 3) */
  maxExamples?: number;
  /** Include "default" from the first seen value */
  includeDefault?: boolean;
  /** Infer "format" for strings (e.g., "date-time", "email", "uri") */
  inferFormats?: boolean;
  /** Infer "pattern" for strings that look like specific formats */
  inferPatterns?: boolean;
  /** Infer numeric constraints (minimum, maximum) from seen values */
  inferNumericRanges?: boolean;
  /** Infer string constraints (minLength, maxLength) from seen values */
  inferStringLengths?: boolean;
  /** Infer array constraints (minItems, maxItems) from seen values */
  inferArrayLengths?: boolean;
  /** Make all object properties required by default (default: true) */
  requiredByDefault?: boolean;
  /** Set additionalProperties to false by default (default: true) */
  strictAdditionalProperties?: boolean;
  /** Prefer "integer" over "number" when all values are whole numbers (default: true) */
  preferInteger?: boolean;
  /** Merge multiple types into anyOf when values have different types (default: true) */
  allowAnyOf?: boolean;
  /** Include "description" placeholders for properties */
  includeDescriptions?: boolean;
  /** Detect enum values when all values are from a small set of strings (default: true) */
  detectEnums?: boolean;
  /** Max distinct values for enum detection (default: 10) */
  maxEnumValues?: number;
}

export interface JsonishParseResult {
  value: JsonValue;
  fixed: string;
  metadata: RepairMetadata;
}

export interface JsonishParseAllResult {
  values: JsonValue[];
  fixed: string[];
  metadata: RepairMetadata[];
}

export interface ValidationError {
  kind: ErrorKind;
  message: string;
  path: string; // e.g. $.items[0].id
  jsonPointer?: string; // e.g. /items/0/id
  limit?: { kind: "maxBufferBytes" | "maxItems"; current: number; max: number };
}

export interface NativeValidationError extends ValidationError {
  name?: string;
}

export interface StreamOutcome<T> {
  done: boolean;
  ok: boolean;
  value: T | null;
  error: ValidationError | null;
}

export interface StreamLimits {
  // 0/undefined means unlimited.
  maxBufferBytes?: number;
  // Applies to emit-all collectors; ignored by emit-first parsers.
  // 0/undefined means unlimited.
  maxItems?: number;
}

export type JsonSchema = {
  type?: "object" | "array" | "string" | "number" | "integer" | "boolean" | "null";
  enum?: JsonValue[];
  const?: JsonValue;
  allOf?: JsonSchema[];
  anyOf?: JsonSchema[];
  oneOf?: JsonSchema[];
  if?: JsonSchema;
  then?: JsonSchema;
  else?: JsonSchema;
  dependentRequired?: Record<string, string[]>;
  properties?: Record<string, JsonSchema>;
  required?: string[];
  default?: JsonValue;
  additionalProperties?: boolean | JsonSchema;
  propertyNames?: JsonSchema;
  items?: JsonSchema;
  minItems?: number;
  maxItems?: number;
  contains?: JsonSchema;
  minContains?: number;
  maxContains?: number;
  minLength?: number;
  maxLength?: number;
  pattern?: string;
  format?: "email" | "uuid" | "date-time" | (string & {});
  minimum?: number;
  maximum?: number;
  multipleOf?: number;
  minProperties?: number;
  maxProperties?: number;
};

export interface MarkdownValidationSchema {
  requiredHeadings?: string[];
  forbidHtml?: boolean;
  maxLineLength?: number;
  minCodeBlocks?: number;
  maxCodeBlocks?: number;
  requiredCodeFences?: string[];
  minTables?: number;
  requireTaskList?: boolean;
  sections?: Record<
    string,
    {
      minLength?: number;
      maxLength?: number;
      requireBullets?: boolean;
      minBullets?: number;
      maxBullets?: number;
    }
  >;
}

export interface KeyValueSchema {
  required?: string[];
  allowExtra?: boolean;
  patterns?: Record<string, string>; // key -> regex
  enum?: Record<string, string[]>;
}

export interface SqlValidationSchema {
  allowedStatements?: string[];
  forbidKeywords?: string[];
  forbidComments?: boolean;
  forbidSemicolon?: boolean;
  requireFrom?: boolean;
  requireWhere?: boolean;
  requireLimit?: boolean;
  maxLimit?: number;
  forbidUnion?: boolean;
  forbidSubqueries?: boolean;
  allowedTables?: string[];
  forbidSelectStar?: boolean;
  requireWhereColumns?: string[];
  forbidSchemas?: string[];
  forbidCrossJoin?: boolean;
  forbidSelectWithoutLimit?: boolean;
  requireOrderBy?: boolean;
  requireWherePatterns?: string[];
  forbidTables?: string[];

  // Hardened SQL safety options.
  maxJoins?: number;
  allowedJoinTypes?: string[];
  forbidOrTrue?: boolean;
  placeholderStyle?: "qmark" | "dollar" | "either";
  // true => forbid any function call; string[] => forbid specific names (case-insensitive)
  forbidFunctions?: boolean | string[];
  allowedColumns?: string[];
  allowUnqualifiedColumns?: boolean;
}

export interface SqlParsed {
  sql: string;
  statementType: string;
  hasWhere: boolean;
  hasFrom: boolean;
  hasLimit: boolean;
  limit?: number;
  hasUnion: boolean;
  hasComments: boolean;
  hasSubquery: boolean;
  tables: string[];
}

export function parseAndValidateJson(text: string, schema: JsonSchema): JsonValue {
  return native.parseAndValidateJson(text, JSON.stringify(schema));
}

export function parseAndValidateJsonWithDefaults(text: string, schema: JsonSchema): JsonValue {
  return native.parseAndValidateJsonWithDefaults(text, JSON.stringify(schema));
}

export function validateAllJson(text: string, schema: JsonSchema): ValidationError[] {
  return native
    .validateAllJson(text, JSON.stringify(schema))
    .map((e) => ({ kind: (e as any).kind, message: e.message, path: e.path, jsonPointer: e.jsonPointer }));
}

export function validateAllJsonValue(value: JsonValue, schema: JsonSchema): ValidationError[] {
  return native
    .validateAllJsonValue(value, JSON.stringify(schema))
    .map((e) => ({ kind: (e as any).kind, message: e.message, path: e.path, jsonPointer: e.jsonPointer }));
}

export function loadsJsonishEx(text: string, repair?: RepairConfig): JsonishParseResult {
  return native.loadsJsonishEx(text, repair);
}

export function extractJsonCandidates(text: string): string[] {
  return native.extractJsonCandidates(text);
}

export function loadsJsonishAll(text: string, repair?: RepairConfig): JsonValue[] {
  return native.loadsJsonishAllEx(text, repair).values;
}

export function loadsJsonishAllEx(text: string, repair?: RepairConfig): JsonishParseAllResult {
  return native.loadsJsonishAllEx(text, repair);
}

export function parseAndValidateJsonEx(text: string, schema: JsonSchema, repair?: RepairConfig): JsonishParseResult {
  return native.parseAndValidateJsonEx(text, JSON.stringify(schema), repair);
}

export function parseAndValidateJsonAll(text: string, schema: JsonSchema, repair?: RepairConfig): JsonValue[] {
  return native.parseAndValidateJsonAllEx(text, JSON.stringify(schema), repair).values;
}

export function parseAndValidateJsonAllEx(text: string, schema: JsonSchema, repair?: RepairConfig): JsonishParseAllResult {
  return native.parseAndValidateJsonAllEx(text, JSON.stringify(schema), repair);
}

export function parseAndValidateJsonWithDefaultsEx(text: string, schema: JsonSchema, repair?: RepairConfig): JsonishParseResult {
  return native.parseAndValidateJsonWithDefaultsEx(text, JSON.stringify(schema), repair);
}

// ---- YAML APIs ----

export function extractYamlCandidate(text: string): string {
  return native.extractYamlCandidate(text);
}

export function extractYamlCandidates(text: string): string[] {
  return native.extractYamlCandidates(text);
}

export function loadsYamlish(text: string, repair?: YamlRepairConfig): JsonValue {
  return native.loadsYamlishEx(text, repair).value;
}

export function loadsYamlishEx(text: string, repair?: YamlRepairConfig): YamlishParseResult {
  return native.loadsYamlishEx(text, repair);
}

export function loadsYamlishAll(text: string, repair?: YamlRepairConfig): JsonValue[] {
  return native.loadsYamlishAllEx(text, repair).values;
}

export function loadsYamlishAllEx(text: string, repair?: YamlRepairConfig): YamlishParseAllResult {
  return native.loadsYamlishAllEx(text, repair);
}

export function dumpsYaml(value: JsonValue, indent: number = 2): string {
  return native.dumpsYaml(value, indent);
}

export function parseAndValidateYaml(text: string, schema: JsonSchema, repair?: YamlRepairConfig): JsonValue {
  if (repair) {
    return native.parseAndValidateYamlEx(text, JSON.stringify(schema), repair).value;
  }
  return native.parseAndValidateYaml(text, JSON.stringify(schema));
}

export function parseAndValidateYamlEx(text: string, schema: JsonSchema, repair?: YamlRepairConfig): YamlishParseResult {
  return native.parseAndValidateYamlEx(text, JSON.stringify(schema), repair);
}

export function parseAndValidateYamlAll(text: string, schema: JsonSchema, repair?: YamlRepairConfig): JsonValue[] {
  return native.parseAndValidateYamlAllEx(text, JSON.stringify(schema), repair).values;
}

export function parseAndValidateYamlAllEx(text: string, schema: JsonSchema, repair?: YamlRepairConfig): YamlishParseAllResult {
  return native.parseAndValidateYamlAllEx(text, JSON.stringify(schema), repair);
}

// ---- TOML ----

export function extractTomlCandidate(text: string): string {
  return native.extractTomlCandidate(text);
}

export function extractTomlCandidates(text: string): string[] {
  return native.extractTomlCandidates(text);
}

export function loadsTomlish(text: string, repair?: TomlRepairConfig): JsonValue {
  return native.loadsTomlishEx(text, repair).value;
}

export function loadsTomlishEx(text: string, repair?: TomlRepairConfig): TomlishParseResult {
  return native.loadsTomlishEx(text, repair);
}

export function loadsTomlishAll(text: string, repair?: TomlRepairConfig): JsonValue[] {
  return native.loadsTomlishAllEx(text, repair).values;
}

export function loadsTomlishAllEx(text: string, repair?: TomlRepairConfig): TomlishParseAllResult {
  return native.loadsTomlishAllEx(text, repair);
}

export function dumpsToml(value: JsonValue): string {
  return native.dumpsToml(value);
}

export function parseAndValidateToml(text: string, schema: JsonSchema, repair?: TomlRepairConfig): JsonValue {
  if (repair) {
    return native.parseAndValidateTomlEx(text, JSON.stringify(schema), repair).value;
  }
  return native.parseAndValidateToml(text, JSON.stringify(schema));
}

export function parseAndValidateTomlEx(text: string, schema: JsonSchema, repair?: TomlRepairConfig): TomlishParseResult {
  return native.parseAndValidateTomlEx(text, JSON.stringify(schema), repair);
}

export function parseAndValidateTomlAll(text: string, schema: JsonSchema, repair?: TomlRepairConfig): JsonValue[] {
  return native.parseAndValidateTomlAllEx(text, JSON.stringify(schema), repair).values;
}

export function parseAndValidateTomlAllEx(text: string, schema: JsonSchema, repair?: TomlRepairConfig): TomlishParseAllResult {
  return native.parseAndValidateTomlAllEx(text, JSON.stringify(schema), repair);
}

// ---- XML / HTML functions ----

export function extractXmlCandidate(text: string, rootTag?: string): string {
  return native.extractXmlCandidate(text, rootTag);
}

export function extractXmlCandidates(text: string, rootTag?: string): string[] {
  return native.extractXmlCandidates(text, rootTag);
}

export function loadsXml(xmlString: string): XmlParseResult {
  return native.loadsXml(xmlString);
}

export function loadsXmlEx(xmlString: string, repair?: XmlRepairConfig): XmlParseResultEx {
  return native.loadsXmlEx(xmlString, repair);
}

export function loadsHtml(htmlString: string): XmlParseResult {
  return native.loadsHtml(htmlString);
}

export function loadsHtmlEx(htmlString: string, repair?: XmlRepairConfig): XmlParseResultEx {
  return native.loadsHtmlEx(htmlString, repair);
}

export function loadsXmlAsJson(xmlString: string): JsonValue {
  return native.loadsXmlAsJson(xmlString);
}

export function loadsHtmlAsJson(htmlString: string): JsonValue {
  return native.loadsHtmlAsJson(htmlString);
}

export function dumpsXml(node: XmlNode, indent?: number): string {
  return native.dumpsXml(node, indent);
}

export function dumpsHtml(node: XmlNode, indent?: number): string {
  return native.dumpsHtml(node, indent);
}

export function queryXml(node: XmlNode, selector: string): XmlNode[] {
  return native.queryXml(node, selector);
}

export function xmlTextContent(node: XmlNode): string {
  return native.xmlTextContent(node);
}

export function xmlGetAttribute(node: XmlNode, attrName: string): string | null {
  return native.xmlGetAttribute(node, attrName);
}

export function validateXml(node: XmlNode, schema: XmlSchema): XmlValidationResult {
  return native.validateXml(node, JSON.stringify(schema));
}

export function parseAndValidateXml(xmlString: string, schema: XmlSchema): XmlParseAndValidateResult {
  return native.parseAndValidateXml(xmlString, JSON.stringify(schema));
}

export function parseAndValidateXmlEx(xmlString: string, schema: XmlSchema, repair?: XmlRepairConfig): XmlParseAndValidateResultEx {
  return native.parseAndValidateXmlEx(xmlString, JSON.stringify(schema), repair);
}

export function parseAndValidateSql(sqlText: string, schema: SqlValidationSchema): SqlParsed {
  return native.parseAndValidateSql(sqlText, JSON.stringify(schema));
}

export function parseAndValidateMarkdown(markdownText: string, schema: MarkdownValidationSchema): {
  headingCount: number;
  codeBlockCount: number;
  tableCount: number;
  taskCount: number;
} {
  return native.parseAndValidateMarkdown(markdownText, JSON.stringify(schema));
}

export function parseAndValidateKv(text: string, schema: KeyValueSchema): Record<string, string> {
  return native.parseAndValidateKv(text, JSON.stringify(schema));
}

export class JsonStreamParser {
  private handle: unknown;

  constructor(schema: JsonSchema, limits?: StreamLimits) {
    this.handle = native.createJsonStreamParser(JSON.stringify(schema), limits);
  }

  reset(): void {
    native.jsonStreamParserReset(this.handle);
  }

  finish(): void {
    native.jsonStreamParserFinish(this.handle);
  }

  location(): StreamLocation {
    return native.jsonStreamParserLocation(this.handle);
  }

  append(chunk: string): void {
    native.jsonStreamParserAppend(this.handle, chunk);
  }

  poll(): StreamOutcome<JsonValue> {
    const out = native.jsonStreamParserPoll(this.handle);
    return {
      done: out.done,
      ok: out.ok,
      value: out.value,
      error: out.error
        ? {
            kind: (out.error as any).kind,
            message: out.error.message,
            path: out.error.path,
            jsonPointer: (out.error as any).jsonPointer,
            limit: (out.error as any).limit,
          }
        : null,
    };
  }
}

export class SqlStreamParser {
  private handle: unknown;

  constructor(schema: SqlValidationSchema, limits?: StreamLimits) {
    this.handle = native.createSqlStreamParser(JSON.stringify(schema), limits);
  }

  reset(): void {
    native.sqlStreamParserReset(this.handle);
  }

  finish(): void {
    native.sqlStreamParserFinish(this.handle);
  }

  location(): StreamLocation {
    return native.sqlStreamParserLocation(this.handle);
  }

  append(chunk: string): void {
    native.sqlStreamParserAppend(this.handle, chunk);
  }

  poll(): StreamOutcome<SqlParsed> {
    const out = native.sqlStreamParserPoll(this.handle);
    return {
      done: out.done,
      ok: out.ok,
      value: out.value,
      error: out.error
        ? {
            kind: (out.error as any).kind,
            message: out.error.message,
            path: out.error.path,
            jsonPointer: (out.error as any).jsonPointer,
            limit: (out.error as any).limit,
          }
        : null,
    };
  }
}

// Emit-all JSON: collect multiple JSON values from a single stream.
// Call close() once no more chunks will arrive.
export class JsonStreamCollector {
  private handle: unknown;

  constructor(itemSchema: JsonSchema, limits?: StreamLimits) {
    this.handle = native.createJsonStreamCollector(JSON.stringify(itemSchema), limits);
  }

  reset(): void {
    native.jsonStreamCollectorReset(this.handle);
  }

  append(chunk: string): void {
    native.jsonStreamCollectorAppend(this.handle, chunk);
  }

  close(): void {
    native.jsonStreamCollectorClose(this.handle);
  }

  location(): StreamLocation {
    return native.jsonStreamCollectorLocation(this.handle);
  }

  poll(): StreamOutcome<JsonValue[]> {
    const out = native.jsonStreamCollectorPoll(this.handle);
    return {
      done: out.done,
      ok: out.ok,
      value: out.value,
      error: out.error
        ? {
            kind: (out.error as any).kind,
            message: out.error.message,
            path: out.error.path,
            jsonPointer: (out.error as any).jsonPointer,
            limit: (out.error as any).limit,
          }
        : null,
    };
  }
}

// Emit-all incrementally: returns new items as soon as they are parseable.
export class JsonStreamBatchCollector {
  private handle: unknown;

  constructor(itemSchema: JsonSchema, limits?: StreamLimits) {
    this.handle = native.createJsonStreamBatchCollector(JSON.stringify(itemSchema), limits);
  }

  reset(): void {
    native.jsonStreamBatchCollectorReset(this.handle);
  }

  append(chunk: string): void {
    native.jsonStreamBatchCollectorAppend(this.handle, chunk);
  }

  close(): void {
    native.jsonStreamBatchCollectorClose(this.handle);
  }

  location(): StreamLocation {
    return native.jsonStreamBatchCollectorLocation(this.handle);
  }

  poll(): StreamOutcome<JsonValue[]> {
    const out = native.jsonStreamBatchCollectorPoll(this.handle);
    return {
      done: out.done,
      ok: out.ok,
      value: out.value,
      error: out.error
        ? {
            kind: (out.error as any).kind,
            message: out.error.message,
            path: out.error.path,
            jsonPointer: (out.error as any).jsonPointer,
            limit: (out.error as any).limit,
          }
        : null,
    };
  }
}

// Emit-all incrementally + defaults-per-item: returns new items as soon as they are parseable.
export class JsonStreamValidatedBatchCollector {
  private handle: unknown;

  constructor(itemSchema: JsonSchema, limits?: StreamLimits) {
    this.handle = native.createJsonStreamValidatedBatchCollector(JSON.stringify(itemSchema), limits);
  }

  reset(): void {
    native.jsonStreamValidatedBatchCollectorReset(this.handle);
  }

  append(chunk: string): void {
    native.jsonStreamValidatedBatchCollectorAppend(this.handle, chunk);
  }

  close(): void {
    native.jsonStreamValidatedBatchCollectorClose(this.handle);
  }

  location(): StreamLocation {
    return native.jsonStreamValidatedBatchCollectorLocation(this.handle);
  }

  poll(): StreamOutcome<JsonValue[]> {
    const out = native.jsonStreamValidatedBatchCollectorPoll(this.handle);
    return {
      done: out.done,
      ok: out.ok,
      value: out.value,
      error: out.error
        ? {
            kind: (out.error as any).kind,
            message: out.error.message,
            path: out.error.path,
            jsonPointer: (out.error as any).jsonPointer,
            limit: (out.error as any).limit,
          }
        : null,
    };
  }
}

// ---- Schema Inference ----

/**
 * Infer JSON Schema from a single value.
 * @param value - A JSON-compatible value
 * @param config - Optional configuration for inference behavior
 * @returns A JSON Schema object
 */
export function inferSchema(value: JsonValue, config?: SchemaInferenceConfig): JsonObject {
  return native.inferSchema(value, config);
}

/**
 * Infer JSON Schema from multiple values (merges schemas).
 * This is useful for inferring a schema from multiple example values,
 * where the schema should accept all of them.
 * @param values - Array of JSON-compatible values
 * @param config - Optional configuration for inference behavior
 * @returns A merged JSON Schema object that accepts all input values
 */
export function inferSchemaFromValues(values: JsonValue[], config?: SchemaInferenceConfig): JsonObject {
  return native.inferSchemaFromValues(values, config);
}

/**
 * Merge two JSON Schemas into one that accepts values valid for either.
 * @param schema1 - First JSON Schema
 * @param schema2 - Second JSON Schema
 * @param config - Optional configuration for merge behavior
 * @returns A merged JSON Schema
 */
export function mergeSchemas(
  schema1: JsonObject,
  schema2: JsonObject,
  config?: SchemaInferenceConfig
): JsonObject {
  return native.mergeSchemas(schema1, schema2, config);
}
