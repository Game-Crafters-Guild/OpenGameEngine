// C# source templates for the scripting ECS. Generation only — callers own
// name validation and file placement.

// Generate a GameSystem C# file from a name.
export function generateGameSystem(name: string): string {
  return `using GameEngine.Scripting;

public class ${name} : GameSystem
{
    public override int Order => 0;

    public override void OnCreate()
    {
    }

    public override void OnUpdate(float deltaTime)
    {
    }

    public override void OnDestroy()
    {
    }
}
`;
}

// Component descriptor for IEntitySystem generation.
export interface ComponentParam {
  name: string;
  access: "ref" | "in";
}

// Generate an IEntitySystem C# file.
export function generateEntitySystem(
  name: string,
  components: ComponentParam[],
  excludedComponents: string[],
  order: number,
): string {
  const withoutAttr =
    excludedComponents.length > 0
      ? `[Without(${excludedComponents.map(c => `typeof(${c})`).join(", ")})]\n`
      : "";

  const orderProp = order !== 0 ? `\n    public int Order => ${order};\n` : "";

  const executeParams = components
    .map(c => `${c.access} ${c.name} ${c.name[0].toLowerCase()}${c.name.slice(1)}`)
    .concat(["float deltaTime"])
    .join(", ");

  return `using GameEngine.Scripting;

${withoutAttr}public partial struct ${name} : IEntitySystem
{${orderProp}
    void Execute(${executeParams})
    {
    }
}
`;
}

// Field descriptor for IComponent generation.
export interface ComponentField {
  name: string;
  type: string;
}

// Generate an IComponent C# file.
export function generateComponent(name: string, fields: ComponentField[]): string {
  const fieldLines = fields.map(f => `    public ${f.type} ${f.name};`).join("\n");

  return `using System.Runtime.InteropServices;
using GameEngine.Scripting;

[StructLayout(LayoutKind.Sequential)]
public struct ${name} : IComponent
{
${fieldLines}
}
`;
}
