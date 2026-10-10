import * as fs from "fs";
import * as path from "path";
import { z } from "zod";
import { generateComponent, generateEntitySystem, generateGameSystem } from "../ecs/templates.js";
import { scanForSystems } from "../ecs/scan.js";
import { defaultScriptsDir, ensureDir, resolveContentPath } from "../paths.js";
import { defineTool, type ToolDef, ToolError } from "../registry.js";

const kPascalCase = /^[A-Z][A-Za-z0-9]*$/;

export const tools: ToolDef[] = [
  defineTool({
    name: "create_game_system",
    category: "ecs",
    actsOnHost: true,
    description: "Create a new GameSystem C# file. GameSystems are global per-frame systems for spawners, managers, state machines — not per-entity iteration.",
    schema: {
      name: z.string().describe("System name (e.g. 'WaveSpawner'). Produces WaveSpawner.cs"),
      directory: z.string().optional().describe("Target directory (default: Assets/Scripts); relative to the project"),
    },
    run: async ({ name, directory }) => {
      if (!kPascalCase.test(name)) {
        throw new ToolError(`Invalid name '${name}'. Must be PascalCase starting with an uppercase letter (e.g. 'WaveSpawner').`);
      }

      const dir = directory ? resolveContentPath(directory) : defaultScriptsDir();
      ensureDir(dir);

      const filePath = path.join(dir, `${name}.cs`);
      if (fs.existsSync(filePath)) throw new ToolError(`File already exists: ${filePath}`);

      const content = generateGameSystem(name);
      fs.writeFileSync(filePath, content, "utf-8");

      return { data: { path: filePath, content }, summary: `Created GameSystem: ${filePath}\n\n${content}` };
    },
  }),

  defineTool({
    name: "create_entity_system",
    category: "ecs",
    actsOnHost: true,
    description: "Create a new IEntitySystem C# file with source-generated chunk iteration. Entity systems process entities that have specific components each frame.",
    schema: {
      name: z.string().describe("System name (e.g. 'MovementSystem'). Produces MovementSystem.cs"),
      components: z.array(z.object({
        name: z.string().describe("Component type name (e.g. 'Position')"),
        access: z.enum(["ref", "in"]).describe("'ref' for read-write, 'in' for read-only"),
      })).min(1).describe("Components the system processes. Each needs a name and access mode."),
      excludedComponents: z.array(z.string()).optional().describe("Component types to exclude — entities with these components are skipped"),
      order: z.coerce.number().optional().describe("Execution order (lower runs first, default 0)"),
      directory: z.string().optional().describe("Target directory (default: Assets/Scripts); relative to the project"),
    },
    run: async ({ name, components, excludedComponents, order, directory }) => {
      if (!kPascalCase.test(name)) {
        throw new ToolError(`Invalid name '${name}'. Must be PascalCase starting with an uppercase letter (e.g. 'MovementSystem').`);
      }

      for (const comp of components) {
        if (!kPascalCase.test(comp.name)) {
          throw new ToolError(`Invalid component name '${comp.name}'. Must be PascalCase.`);
        }
      }

      const compNames = components.map(c => c.name);
      const dupes = compNames.filter((n, i) => compNames.indexOf(n) !== i);
      if (dupes.length > 0) {
        throw new ToolError(`Duplicate component: ${dupes[0]}. Each component type can only appear once in Execute parameters.`);
      }

      const dir = directory ? resolveContentPath(directory) : defaultScriptsDir();
      ensureDir(dir);

      const filePath = path.join(dir, `${name}.cs`);
      if (fs.existsSync(filePath)) throw new ToolError(`File already exists: ${filePath}`);

      const content = generateEntitySystem(name, components, excludedComponents ?? [], order ?? 0);
      fs.writeFileSync(filePath, content, "utf-8");

      return { data: { path: filePath, content }, summary: `Created IEntitySystem: ${filePath}\n\n${content}` };
    },
  }),

  defineTool({
    name: "create_component",
    category: "ecs",
    actsOnHost: true,
    description: "Create a new IComponent struct C# file. Components are unmanaged structs with sequential layout that hold per-entity data.",
    schema: {
      name: z.string().describe("Component name (e.g. 'Health'). Produces Health.cs"),
      fields: z.array(z.object({
        name: z.string().describe("Field name in PascalCase (e.g. 'Current')"),
        type: z.string().describe("C# unmanaged type (e.g. 'float', 'int', 'byte', 'double')"),
      })).min(1).describe("Component fields"),
      directory: z.string().optional().describe("Target directory (default: Assets/Scripts); relative to the project"),
    },
    run: async ({ name, fields, directory }) => {
      if (!kPascalCase.test(name)) {
        throw new ToolError(`Invalid name '${name}'. Must be PascalCase starting with an uppercase letter (e.g. 'Health').`);
      }

      for (const field of fields) {
        if (!kPascalCase.test(field.name)) {
          throw new ToolError(`Invalid field name '${field.name}'. Must be PascalCase (e.g. 'Current').`);
        }
      }

      // bool is 1 byte in C++ but 4 in C# marshalling — a silent ABI mismatch.
      const boolFields = fields.filter(f => f.type === "bool");
      const warning = boolFields.length > 0
        ? `\nWarning: Field(s) ${boolFields.map(f => f.name).join(", ")} use 'bool'. Use 'byte' instead for C#/C++ ABI safety.\n`
        : "";

      const dir = directory ? resolveContentPath(directory) : defaultScriptsDir();
      ensureDir(dir);

      const filePath = path.join(dir, `${name}.cs`);
      if (fs.existsSync(filePath)) throw new ToolError(`File already exists: ${filePath}`);

      const content = generateComponent(name, fields);
      fs.writeFileSync(filePath, content, "utf-8");

      return {
        data: { path: filePath, content, boolFields: boolFields.map(f => f.name) },
        summary: `Created IComponent: ${filePath}${warning}\n\n${content}`,
      };
    },
  }),

  defineTool({
    name: "list_ecs_systems",
    category: "ecs",
    description: "Scan project C# files for all ECS systems (GameSystem and IEntitySystem). Returns name, type, execution order, component access, and file path.",
    schema: {
      directory: z.string().optional().describe("Directory to scan (default: Assets/Scripts); relative to the project"),
    },
    run: async ({ directory }) => {
      const dir = directory ? resolveContentPath(directory) : defaultScriptsDir();

      if (!fs.existsSync(dir)) {
        return { data: { directory: dir, systems: [] }, summary: `Directory not found: ${dir}\nNo C# scripts to scan.` };
      }

      const systems = scanForSystems(dir);

      if (systems.length === 0) {
        return { data: { directory: dir, systems }, summary: `No ECS systems found in ${dir}` };
      }

      const summary = systems.map(s => {
        const compStr =
          s.type === "IEntitySystem" && s.components.length > 0
            ? ` | components: ${s.components.join(", ")}`
            : "";
        const excludedStr =
          s.excludedComponents.length > 0
            ? ` | excluded: ${s.excludedComponents.join(", ")}`
            : "";
        return `[${s.type}] ${s.name} (order: ${s.order ?? 0})${compStr}${excludedStr}\n  ${s.filePath}`;
      });

      return {
        data: { directory: dir, systems },
        summary: `Found ${systems.length} ECS system(s) in ${dir}:\n\n${summary.join("\n\n")}`,
      };
    },
  }),
];
