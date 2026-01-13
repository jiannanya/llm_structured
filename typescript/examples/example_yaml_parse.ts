/**
 * Example: Parse and validate YAML from LLM output
 */

import {
  parseAndValidateYaml,
  loadsYamlish,
  dumpsYaml,
  extractYamlCandidates,
  type JsonSchema,
  type ValidationError,
} from "../src/index";

function main() {
  // Example 1: Simple YAML parsing
  console.log("=== Example 1: Simple YAML parsing ===");
  const yamlText = `
\`\`\`yaml
name: Alice
age: 30
active: true
\`\`\`
  `;

  try {
    const value = loadsYamlish(yamlText);
    console.log("Parsed:", value);
    console.log("Serialized back:", dumpsYaml(value));
  } catch (e) {
    console.error("Error:", e);
  }

  // Example 2: Parse and validate
  console.log("\n=== Example 2: Parse and validate ===");
  const yamlWithSchema = `
\`\`\`yaml
users:
  - name: Alice
    age: 30
  - name: Bob
    age: 25
\`\`\`
  `;

  const schema: JsonSchema = {
    type: "object",
    required: ["users"],
    properties: {
      users: {
        type: "array",
        items: {
          type: "object",
          required: ["name", "age"],
          properties: {
            name: { type: "string" },
            age: { type: "integer", minimum: 0 },
          },
        },
      },
    },
  };

  try {
    const result = parseAndValidateYaml(yamlWithSchema, schema);
    console.log("Valid:", result);
  } catch (e) {
    const err = e as ValidationError;
    console.error(`Validation error at ${err.path}: ${err.message}`);
  }

  // Example 3: Multiple YAML documents
  console.log("\n=== Example 3: Multiple YAML documents ===");
  const multiYaml = `
Here are two configs:

\`\`\`yaml
service: api
port: 8080
\`\`\`

And another one:

\`\`\`yaml
service: db
port: 5432
\`\`\`
  `;

  const candidates = extractYamlCandidates(multiYaml);
  console.log(`Found ${candidates.length} YAML documents:`);
  for (let i = 0; i < candidates.length; i++) {
    console.log(`\nDocument ${i + 1}:`);
    console.log(candidates[i]);
    console.log("Parsed:", loadsYamlish(candidates[i]));
  }

  // Example 4: YAML with repairs
  console.log("\n=== Example 4: YAML with tab fixes ===");
  const yamlWithTabs = `
\`\`\`yaml
config:
\tname: test
\tvalue: 123
\`\`\`
  `;

  try {
    const result = loadsYamlish(yamlWithTabs);
    console.log("Parsed (tabs fixed):", result);
  } catch (e) {
    console.error("Error:", e);
  }
}

main();
