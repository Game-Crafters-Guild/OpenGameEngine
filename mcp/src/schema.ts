import { z } from "zod";

/**
 * Accepts a JSON object directly or a JSON-encoded string. LLMs sometimes
 * stringify object arguments; the CLI passes a `{...}` token through verbatim
 * for the same reason.
 */
export const flexibleObject = z.preprocess(
  (val) => {
    if (typeof val === "string") {
      try { return JSON.parse(val); } catch { return val; }
    }
    return val;
  },
  z.record(z.unknown()),
);

export const vec3Schema = z.object({
  x: z.number().optional(),
  y: z.number().optional(),
  z: z.number().optional(),
});

export const quatSchema = z.object({
  x: z.number().optional(),
  y: z.number().optional(),
  z: z.number().optional(),
  w: z.number().optional(),
});

/**
 * A positional triple, e.g. camera position [x, y, z].
 *
 * A factory, not a shared const: reusing ONE zod instance for two properties of
 * the same tool makes the JSON Schema emit `$ref: #/properties/<other>` for the
 * second instead of inlining it (look_at's target/direction), which is a worse
 * schema for every consumer. Call it per property.
 */
export function vec3Tuple() {
  return z.array(z.coerce.number()).length(3);
}
