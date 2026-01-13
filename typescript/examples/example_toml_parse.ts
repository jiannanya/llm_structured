/**
 * Example: Parsing TOML from LLM output using llm_structured.
 * 
 * This demonstrates:
 * 1. Basic TOML parsing
 * 2. Parsing with JSON Schema validation
 * 3. Extracting TOML from fenced code blocks
 * 4. Handling arrays of tables
 * 5. Serializing back to TOML
 */

import {
  loadsTomlish,
  loadsTomlishEx,
  extractTomlCandidate,
  parseAndValidateToml,
  dumpsToml,
  JsonSchema,
  JsonValue,
} from "../src/index";

function main() {
  // Example 1: Simple TOML parsing
  console.log("=== Example 1: Simple TOML parsing ===");
  const simpleToml = `
title = "TOML Example"
enabled = true
count = 42

[owner]
name = "Tom Preston-Werner"
dob = 1979-05-27T07:32:00-08:00
`;
  const result = loadsTomlish(simpleToml);
  console.log("Parsed:", JSON.stringify(result, null, 2));
  console.log();

  // Example 2: Parse and validate with JSON Schema
  console.log("=== Example 2: Parse and validate ===");
  const configToml = `
[server]
host = "localhost"
port = 8080

[database]
name = "myapp"
max_connections = 100
`;
  const schema: JsonSchema = {
    type: "object",
    properties: {
      server: {
        type: "object",
        properties: {
          host: { type: "string" },
          port: { type: "number" },
        },
        required: ["host", "port"],
      },
      database: {
        type: "object",
        properties: {
          name: { type: "string" },
          max_connections: { type: "number" },
        },
      },
    },
    required: ["server"],
  };
  const validated = parseAndValidateToml(configToml, schema);
  console.log("Valid:", JSON.stringify(validated, null, 2));
  console.log();

  // Example 3: Extract from fenced code block
  console.log("=== Example 3: Extract from markdown ===");
  const llmOutput = `
Here's the configuration you requested:

\`\`\`toml
[package]
name = "my-app"
version = "1.0.0"

[dependencies]
serde = "1.0"
tokio = { version = "1.0", features = ["full"] }
\`\`\`

Let me know if you need any changes!
`;
  const extracted = extractTomlCandidate(llmOutput);
  console.log("Extracted:", extracted);
  const parsed = loadsTomlish(llmOutput);
  console.log("Parsed:", JSON.stringify(parsed, null, 2));
  console.log();

  // Example 4: Arrays of tables
  console.log("=== Example 4: Arrays of tables ===");
  const productsToml = `
[[products]]
name = "Hammer"
sku = 738594937

[[products]]
name = "Nail"
sku = 284758393
color = "gray"

[[products]]
name = "Screwdriver"
sku = 847382910
`;
  const products = loadsTomlish(productsToml) as { products: Array<{ name: string; sku: number }> };
  console.log("Products:", JSON.stringify(products, null, 2));
  products.products?.forEach((p, i) => {
    console.log(`  Product ${i + 1}: ${p.name} (SKU: ${p.sku})`);
  });
  console.log();

  // Example 5: Serialize back to TOML
  console.log("=== Example 5: Serialize to TOML ===");
  const data = {
    title: "My Config",
    database: {
      host: "localhost",
      port: 5432,
      enabled: true,
    },
    features: ["auth", "cache", "logging"],
  };
  const tomlOutput = dumpsToml(data);
  console.log("Serialized TOML:");
  console.log(tomlOutput);

  // Example 6: Extended parsing with metadata
  console.log("\n=== Example 6: Extended parsing with metadata ===");
  const resultEx = loadsTomlishEx(simpleToml);
  console.log("Value:", JSON.stringify(resultEx.value, null, 2));
  console.log("Metadata:", JSON.stringify(resultEx.metadata, null, 2));
}

main();
