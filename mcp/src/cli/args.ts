import { z } from "zod";
import { zodToJsonSchema } from "zod-to-json-schema";
import { ToolError, type ToolDef } from "../registry.js";

/**
 * CLI flags are derived from each tool's JSON Schema — the same document MCP
 * publishes through tools/list. Deriving from it rather than from zod internals
 * means `--help` cannot describe something different from what an agent sees,
 * and nothing here depends on zod's private `_def` shapes.
 *
 * Tokens are converted to typed JS values HERE, before zod runs. That ordering
 * is load-bearing: several schemas use z.coerce.boolean(), which maps the
 * string "false" to true. A raw argv string must never reach one.
 */

export type ArgKind =
  | "string"
  | "number"
  | "boolean"
  | "enum"
  | "array"        // scalar items, repeatable / comma-separated / JSON
  | "objectArray"  // items are records, `field:field` in declared order
  | "vector"       // {x,y,z[,w]} — accepts "1,2,3" as well as JSON
  | "map"          // free-form object — repeatable k=v, or a JSON object
  | "union";       // string|number, string|string[]

export interface ArgSpec {
  /** Schema key, e.g. `entityId`. */
  key: string;
  /** Primary flag, e.g. `--entity-id`. The camelCase key is also accepted. */
  flag: string;
  kind: ArgKind;
  required: boolean;
  description: string;
  /** enum: the permitted values. */
  choices?: string[];
  /** array: item type; objectArray: field names in declared order. */
  itemType?: "string" | "number" | "boolean";
  itemFields?: string[];
  /** vector: component names, in order. */
  components?: string[];
  /** array with a fixed length, e.g. [x, y, z]. */
  fixedLength?: number;
  /** map: true when the schema pins values to strings, so k=v must not coerce. */
  mapValuesAreStrings?: boolean;
  /** union: whether a numeric token should be passed as a number. */
  unionAcceptsNumber?: boolean;
  /** union/array: whether the value may be a list. */
  acceptsList?: boolean;
}

export function camelToKebab(key: string): string {
  return key.replace(/[A-Z]/g, (c) => `-${c.toLowerCase()}`);
}

function scalarKind(type: unknown): "string" | "number" | "boolean" {
  if (type === "number" || type === "integer") return "number";
  if (type === "boolean") return "boolean";
  return "string";
}

function classify(key: string, prop: any, required: boolean): ArgSpec {
  const base = { key, flag: `--${camelToKebab(key)}`, required, description: prop?.description ?? "" };

  // string|number, string|string[] — expressed as a type array or anyOf.
  const anyOf: any[] | undefined = prop?.anyOf;
  if (anyOf) {
    const arrayBranch = anyOf.find((b) => b?.type === "array");
    return {
      ...base, kind: "union",
      acceptsList: Boolean(arrayBranch),
      itemType: arrayBranch ? scalarKind(arrayBranch.items?.type) : "string",
      unionAcceptsNumber: anyOf.some((b) => b?.type === "number" || b?.type === "integer"),
    };
  }
  if (Array.isArray(prop?.type)) {
    return {
      ...base, kind: "union",
      acceptsList: prop.type.includes("array"),
      unionAcceptsNumber: prop.type.includes("number") || prop.type.includes("integer"),
    };
  }

  if (prop?.enum) return { ...base, kind: "enum", choices: prop.enum.map(String) };

  if (prop?.type === "array") {
    if (prop.items?.type === "object" && prop.items?.properties) {
      return { ...base, kind: "objectArray", itemFields: Object.keys(prop.items.properties) };
    }
    return {
      ...base, kind: "array",
      itemType: scalarKind(prop.items?.type),
      fixedLength: prop.minItems === prop.maxItems ? prop.minItems : undefined,
    };
  }

  if (prop?.type === "object") {
    const props = prop.properties as Record<string, any> | undefined;
    // {x,y,z} / {x,y,z,w} of numbers gets positional sugar; anything else is a map.
    if (props) {
      const names = Object.keys(props);
      const isVector = names.length > 0 && names.length <= 4
        && names.every((n) => "xyzw".includes(n) && props[n]?.type === "number");
      if (isVector) return { ...base, kind: "vector", components: names };
    }
    return { ...base, kind: "map", mapValuesAreStrings: prop.additionalProperties?.type === "string" };
  }

  return { ...base, kind: scalarKind(prop?.type) };
}

export function argSpecs(tool: ToolDef): ArgSpec[] {
  const schema = zodToJsonSchema(z.object(tool.schema), { $refStrategy: "none" }) as any;
  const props: Record<string, any> = schema.properties ?? {};
  const required: string[] = schema.required ?? [];
  return Object.entries(props).map(([key, prop]) => classify(key, prop, required.includes(key)));
}

// --- token conversion ---

function badArg(message: string): never {
  throw new ToolError(message, "BAD_ARGUMENT");
}

function toNumber(spec: ArgSpec, token: string): number {
  const n = Number(token);
  if (!Number.isFinite(n)) badArg(`${spec.flag} expects a number, got '${token}'.`);
  return n;
}

/**
 * Strict boolean parsing. Never delegate this to z.coerce.boolean(), which
 * treats every non-empty string — including "false" — as true.
 */
function toBoolean(spec: ArgSpec, token: string): boolean {
  const t = token.toLowerCase();
  if (["true", "1", "yes", "on"].includes(t)) return true;
  if (["false", "0", "no", "off"].includes(t)) return false;
  badArg(`${spec.flag} expects true or false, got '${token}'.`);
}

function looksLikeJson(token: string): boolean {
  const t = token.trim();
  return t.startsWith("{") || t.startsWith("[");
}

function splitList(token: string): string[] {
  return token.split(",").map((s) => s.trim()).filter((s) => s.length > 0);
}

/** Best-effort typing for a free-form `key=value` token. */
function mapScalar(raw: string): unknown {
  if (raw === "true") return true;
  if (raw === "false") return false;
  if (raw.trim() !== "" && Number.isFinite(Number(raw))) return Number(raw);
  return raw;
}

function scalarToken(kind: "string" | "number" | "boolean", spec: ArgSpec, token: string): unknown {
  if (kind === "number") return toNumber(spec, token);
  if (kind === "boolean") return toBoolean(spec, token);
  return token;
}

/** Fold one occurrence of a flag into the accumulating params object. */
function applyToken(spec: ArgSpec, token: string, params: Record<string, unknown>): void {
  switch (spec.kind) {
    case "string": params[spec.key] = token; return;
    case "number": params[spec.key] = toNumber(spec, token); return;
    case "boolean": params[spec.key] = toBoolean(spec, token); return;

    case "enum":
      if (!spec.choices!.includes(token)) {
        badArg(`${spec.flag} expects one of: ${spec.choices!.join(", ")} — got '${token}'.`);
      }
      params[spec.key] = token;
      return;

    case "array": {
      if (looksLikeJson(token)) { params[spec.key] = JSON.parse(token); return; }
      const values = splitList(token).map((t) => scalarToken(spec.itemType ?? "string", spec, t));
      const prev = (params[spec.key] as unknown[]) ?? [];
      params[spec.key] = [...prev, ...values];
      return;
    }

    case "objectArray": {
      if (looksLikeJson(token)) { params[spec.key] = JSON.parse(token); return; }
      // `Position:ref` — fields in the order the schema declares them.
      const parts = token.split(":");
      const fields = spec.itemFields!;
      if (parts.length > fields.length) {
        badArg(`${spec.flag} expects ${fields.join(":")} — got '${token}'.`);
      }
      const item: Record<string, string> = {};
      parts.forEach((p, i) => { item[fields[i]] = p; });
      const prev = (params[spec.key] as unknown[]) ?? [];
      params[spec.key] = [...prev, item];
      return;
    }

    case "vector": {
      if (looksLikeJson(token)) { params[spec.key] = JSON.parse(token); return; }
      const parts = splitList(token);
      const names = spec.components!;
      if (parts.length > names.length) {
        badArg(`${spec.flag} expects up to ${names.length} components (${names.join(",")}) — got '${token}'.`);
      }
      const vec: Record<string, number> = {};
      parts.forEach((p, i) => { vec[names[i]] = toNumber(spec, p); });
      params[spec.key] = vec;
      return;
    }

    case "map": {
      // A `{...}` token is parsed here, so flexibleObject and plain record
      // schemas both receive the object form.
      if (looksLikeJson(token)) { params[spec.key] = JSON.parse(token); return; }
      const eq = token.indexOf("=");
      if (eq < 0) badArg(`${spec.flag} expects key=value or a JSON object — got '${token}'.`);
      const prev = (params[spec.key] as Record<string, unknown>) ?? {};
      const raw = token.slice(eq + 1);
      // Component fields are typed, so `--values Intensity=3` has to arrive as a
      // number. Where the schema pins values to strings (graph node
      // parameters), leave them alone or validation rejects them.
      const value = spec.mapValuesAreStrings ? raw : mapScalar(raw);
      params[spec.key] = { ...prev, [token.slice(0, eq)]: value };
      return;
    }

    case "union": {
      if (spec.acceptsList && looksLikeJson(token)) { params[spec.key] = JSON.parse(token); return; }
      if (spec.acceptsList && token.includes(",")) { params[spec.key] = splitList(token); return; }
      if (spec.unionAcceptsNumber && token.trim() !== "" && Number.isFinite(Number(token))) {
        params[spec.key] = Number(token);
        return;
      }
      params[spec.key] = token;
      return;
    }
  }
}

export interface ParsedInvocation {
  params: Record<string, unknown>;
}

/**
 * Turn a tool's argv tail into validated parameters.
 *
 * A lone `{...}` argument is the whole parameter object — the form MCP
 * tool-call JSON already has, so a call can be pasted from a transcript. It may
 * not be combined with flags: half-edited invocations should fail loudly rather
 * than silently pick a precedence.
 */
export function parseToolArgs(tool: ToolDef, argv: string[]): ParsedInvocation {
  const specs = argSpecs(tool);
  const byFlag = new Map<string, ArgSpec>();
  for (const spec of specs) {
    byFlag.set(spec.flag, spec);
    byFlag.set(`--${spec.key}`, spec);
  }

  // A leading `{` means the whole parameter object was passed as JSON. Only a
  // LEADING one: in flag mode a `{...}` token is a flag's value, which is the
  // documented spelling for --values / --components.
  if (argv.length > 0 && looksLikeJson(argv[0])) {
    if (argv.length > 1) {
      badArg(
        `A JSON parameter object must be the only argument (got ${argv.length}). ` +
        `If you meant to pass flags, drop the JSON; if the shell split an unquoted object, quote it: '{"key":"value"}'.`);
    }
    let parsed: unknown;
    try {
      parsed = JSON.parse(argv[0]);
    } catch (e: any) {
      badArg(`Parameter JSON is not valid: ${e.message}\nGot: ${argv[0]}`);
    }
    if (parsed === null || typeof parsed !== "object" || Array.isArray(parsed)) {
      badArg(`Parameter JSON must be an object, got: ${argv[0]}`);
    }
    return { params: validate(tool, parsed as Record<string, unknown>) };
  }

  const params: Record<string, unknown> = {};
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (!arg.startsWith("--")) {
      badArg(`Unexpected argument '${arg}'. Parameters are passed as flags (see --help).`);
    }

    const eq = arg.indexOf("=");
    const name = eq >= 0 ? arg.slice(0, eq) : arg;
    const inlineValue = eq >= 0 ? arg.slice(eq + 1) : undefined;

    // `--no-x` sets a boolean false; absence leaves it unset, which several
    // tools distinguish from false (set_gizmos_visibility keeps a group's
    // current state when its flag is omitted).
    if (!byFlag.has(name) && name.startsWith("--no-")) {
      const positive = `--${name.slice(5)}`;
      const spec = byFlag.get(positive);
      if (spec && (spec.kind === "boolean" || spec.kind === "union")) {
        params[spec.key] = false;
        continue;
      }
    }

    const spec = byFlag.get(name);
    if (!spec) {
      badArg(`Unknown flag '${name}' for ${tool.name}. Run 'ge ${tool.name} --help' for the parameter list.`);
    }

    if (spec.kind === "boolean" && inlineValue === undefined) {
      // A bare boolean flag means true; a following token is a value only if
      // it is not itself a flag.
      const next = argv[i + 1];
      if (next !== undefined && !next.startsWith("--")) { params[spec.key] = toBoolean(spec, next); i++; }
      else params[spec.key] = true;
      continue;
    }

    const value = inlineValue ?? argv[++i];
    if (value === undefined) badArg(`${spec.flag} expects a value.`);
    applyToken(spec, value, params);
  }

  return { params: validate(tool, params) };
}

function validate(tool: ToolDef, params: Record<string, unknown>): Record<string, unknown> {
  // strict(): a JSON parameter object is documented as a verbatim round-trip,
  // so a key the schema does not know must be a loud error, not a silent drop —
  // stripping it would send a request that no-ops and still answers ok. For a
  // handler key that is deliberately not exposed on the tool, `raw` is the path.
  const result = z.object(tool.schema).strict().safeParse(params);
  if (!result.success) {
    const issues = result.error.issues
      .map((i) => `  ${i.path.length ? i.path.join(".") : "(root)"}: ${i.message}`)
      .join("\n");
    badArg(`Invalid arguments for ${tool.name}:\n${issues}`);
  }
  return result.data as Record<string, unknown>;
}
