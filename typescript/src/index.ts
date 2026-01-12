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

  loadsJsonishEx(text: string, repair?: RepairConfig): JsonishParseResult;
  loadsJsonishAllEx(text: string, repair?: RepairConfig): JsonishParseAllResult;
  parseAndValidateJsonEx(text: string, schemaJson: string, repair?: RepairConfig): JsonishParseResult;
  parseAndValidateJsonAllEx(text: string, schemaJson: string, repair?: RepairConfig): JsonishParseAllResult;
  parseAndValidateJsonWithDefaultsEx(text: string, schemaJson: string, repair?: RepairConfig): JsonishParseResult;

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
