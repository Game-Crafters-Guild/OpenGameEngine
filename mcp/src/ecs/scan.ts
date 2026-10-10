import * as fs from "fs";
import * as path from "path";

// Discover ECS systems by reading C# source. Regex-level parsing: it reports
// what a declaration looks like, not what the compiler resolves it to.

export interface SystemInfo {
  name: string;
  type: "GameSystem" | "IEntitySystem";
  filePath: string;
  order: number | null;
  components: string[];
  excludedComponents: string[];
}

export function scanForSystems(dir: string): SystemInfo[] {
  const results: SystemInfo[] = [];
  if (!fs.existsSync(dir)) return results;

  function walk(d: string): void {
    for (const entry of fs.readdirSync(d, { withFileTypes: true })) {
      const full = path.join(d, entry.name);
      if (entry.isDirectory()) {
        walk(full);
      } else if (entry.name.endsWith(".cs")) {
        const content = fs.readFileSync(full, "utf-8");

        // Match GameSystem subclasses: class X : GameSystem
        const gameSystemMatch = content.match(/class\s+(\w+)\s*:\s*GameSystem\b/);
        if (gameSystemMatch) {
          const name = gameSystemMatch[1];
          const orderMatch = content.match(/Order\s*=>\s*(\d+)/);
          results.push({
            name,
            type: "GameSystem",
            filePath: full,
            order: orderMatch ? parseInt(orderMatch[1], 10) : 0,
            components: [],
            excludedComponents: [],
          });
        }

        // Match IEntitySystem implementations: partial struct X : IEntitySystem
        const entitySystemMatch = content.match(/partial\s+struct\s+(\w+)\s*:\s*IEntitySystem\b/);
        if (entitySystemMatch) {
          const name = entitySystemMatch[1];
          const orderMatch = content.match(/Order\s*=>\s*(\d+)/);

          // Extract component access from Execute method parameters.
          const executeMatch = content.match(/void\s+Execute\s*\(([^)]*)\)/);
          const components: string[] = [];
          if (executeMatch) {
            const paramStr = executeMatch[1];
            // Match ref/in component parameters (e.g., "ref Position pos", "in Velocity vel").
            const paramRegex = /(?:ref|in)\s+(\w+)\s+\w+/g;
            let m: RegExpExecArray | null;
            while ((m = paramRegex.exec(paramStr)) !== null) {
              components.push(m[1]);
            }
          }

          // Extract [Without(...)] excluded components.
          const excludedComponents: string[] = [];
          const withoutMatch = content.match(/\[Without\(([^)]*)\)\]/);
          if (withoutMatch) {
            const typeofRegex = /typeof\((\w+)\)/g;
            let m: RegExpExecArray | null;
            while ((m = typeofRegex.exec(withoutMatch[1])) !== null) {
              excludedComponents.push(m[1]);
            }
          }

          results.push({
            name,
            type: "IEntitySystem",
            filePath: full,
            order: orderMatch ? parseInt(orderMatch[1], 10) : 0,
            components,
            excludedComponents,
          });
        }
      }
    }
  }

  walk(dir);
  results.sort((a, b) => (a.order ?? 0) - (b.order ?? 0));
  return results;
}
