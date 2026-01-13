import {
  buildAnthropicTool,
  buildGeminiFunctionDeclaration,
  buildOpenaiFunctionTool,
  parseAnthropicToolUsesFromResponse,
  parseGeminiFunctionCallsFromResponse,
  parseOpenaiToolCallsFromResponse,
  type JsonSchema,
} from "../src/index";

function main(): void {
  const schema: JsonSchema = {
    type: "object",
    additionalProperties: false,
    required: ["id"],
    properties: { id: { type: "integer" } },
  };

  console.log("=== Build tool schemas ===");
  console.log(JSON.stringify(buildOpenaiFunctionTool("get_user", "Get a user", schema).tool, null, 2));
  console.log(JSON.stringify(buildAnthropicTool("get_user", "Get a user", schema).tool, null, 2));
  console.log(JSON.stringify(buildGeminiFunctionDeclaration("get_user", "Get a user", schema).tool, null, 2));

  const schemasByName = { get_user: schema };

  console.log("\n=== Parse OpenAI response tool_calls ===");
  const openaiResponse = {
    choices: [
      {
        message: {
          tool_calls: [
            {
              id: "call_1",
              type: "function",
              function: { name: "get_user", arguments: "{'id':'123',}" },
            },
          ],
        },
      },
    ],
  };

  const openaiCalls = parseOpenaiToolCallsFromResponse(
    openaiResponse as any,
    schemasByName,
    { coerceTypes: true },
    { allowSingleQuotes: true, dropTrailingCommas: true }
  );
  console.log(JSON.stringify(openaiCalls, null, 2));

  console.log("\n=== Parse Anthropic response tool_use ===");
  const anthropicResponse = {
    content: [
      { type: "text", text: "hi" },
      { type: "tool_use", id: "tu_1", name: "get_user", input: { id: "123", extra: 1 } },
    ],
  };

  const anthropicCalls = parseAnthropicToolUsesFromResponse(
    anthropicResponse as any,
    schemasByName,
    { coerceTypes: true, removeExtraProperties: true }
  );
  console.log(JSON.stringify(anthropicCalls, null, 2));

  console.log("\n=== Parse Gemini response functionCall ===");
  const geminiResponse = {
    candidates: [
      {
        content: {
          parts: [{ functionCall: { name: "get_user", args: { id: "123" } } }],
        },
      },
    ],
  };

  const geminiCalls = parseGeminiFunctionCallsFromResponse(geminiResponse as any, schemasByName, { coerceTypes: true });
  console.log(JSON.stringify(geminiCalls, null, 2));
}

main();
