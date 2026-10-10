// The precompiled action catalog for game_logic graphs: the node types a graph
// may contain, their pins, and their default parameter values. This is the
// authority the editor's action browser and the C++ compiler both read from.

export type GraphPinDirection = "in" | "out";

export interface GameGraphPin {
  id: string;
  direction: GraphPinDirection;
  dataType: string;
  displayName: string;
}
export interface GameGraphAction {
  typeId: string;
  displayName: string;
  category: string;
  pins: GameGraphPin[];
  defaults: Record<string, string>;
}

function pin(id: string, direction: GraphPinDirection, dataType: string, displayName: string): GameGraphPin {
  return { id, direction, dataType, displayName };
}

function flowPins(extra: GameGraphPin[] = []): GameGraphPin[] {
  return [pin("in", "in", "flow", "In"), ...extra, pin("out", "out", "flow", "Out")];
}

export const kGameGraphActions: GameGraphAction[] = [
  { typeId: "Entry", displayName: "Entry", category: "Flow", pins: [pin("out", "out", "flow", "Out")], defaults: {} },
  { typeId: "Sequence", displayName: "Sequence", category: "Flow", pins: [pin("in", "in", "flow", "In"), pin("out0", "out", "flow", "Out 1"), pin("out1", "out", "flow", "Out 2"), pin("out2", "out", "flow", "Out 3")], defaults: {} },
  { typeId: "Branch", displayName: "Branch", category: "Flow", pins: [pin("in", "in", "flow", "In"), pin("condition", "in", "bool", "Condition"), pin("true", "out", "flow", "True"), pin("false", "out", "flow", "False")], defaults: { condition: "false" } },
  { typeId: "Delay", displayName: "Delay", category: "Flow", pins: flowPins([pin("seconds", "in", "float", "Seconds")]), defaults: { seconds: "1" } },
  { typeId: "SendEvent", displayName: "Send Event", category: "Flow", pins: flowPins([pin("eventName", "in", "string", "Event")]), defaults: { eventName: "Event" } },
  { typeId: "Finish", displayName: "Finish", category: "Flow", pins: [pin("in", "in", "flow", "In")], defaults: {} },
  { typeId: "SetVariable", displayName: "Set Variable", category: "Variables", pins: flowPins([pin("value", "in", "any", "Value")]), defaults: { variableName: "playerScore", value: "0" } },
  { typeId: "GetVariable", displayName: "Get Variable", category: "Variables", pins: [pin("out", "out", "any", "Value")], defaults: { variableName: "playerScore" } },
  { typeId: "CompareFloat", displayName: "Compare Float", category: "Variables", pins: [pin("in", "in", "flow", "In"), pin("a", "in", "float", "A"), pin("b", "in", "float", "B"), pin("true", "out", "flow", "True"), pin("false", "out", "flow", "False")], defaults: { a: "0", b: "0", comparison: "greater" } },
  { typeId: "CompareBool", displayName: "Compare Bool", category: "Variables", pins: [pin("in", "in", "flow", "In"), pin("value", "in", "bool", "Value"), pin("true", "out", "flow", "True"), pin("false", "out", "flow", "False")], defaults: { value: "false", expected: "true" } },
  { typeId: "BoolOperator", displayName: "Bool Operator", category: "Logic", pins: [pin("a", "in", "bool", "A"), pin("b", "in", "bool", "B"), pin("result", "out", "bool", "Result")], defaults: { a: "false", b: "false", operation: "and" } },
  { typeId: "BoolNot", displayName: "Bool Not", category: "Logic", pins: [pin("value", "in", "bool", "Value"), pin("result", "out", "bool", "Result")], defaults: { value: "false" } },
  { typeId: "CompareInt", displayName: "Compare Int", category: "Logic", pins: [pin("in", "in", "flow", "In"), pin("a", "in", "int", "A"), pin("b", "in", "int", "B"), pin("true", "out", "flow", "True"), pin("false", "out", "flow", "False")], defaults: { a: "0", b: "0", comparison: "equal" } },
  { typeId: "FloatOperator", displayName: "Float Operator", category: "Math", pins: [pin("a", "in", "float", "A"), pin("b", "in", "float", "B"), pin("result", "out", "float", "Result")], defaults: { a: "0", b: "0", operation: "add" } },
  { typeId: "FloatClamp", displayName: "Float Clamp", category: "Math", pins: [pin("value", "in", "float", "Value"), pin("min", "in", "float", "Min"), pin("max", "in", "float", "Max"), pin("result", "out", "float", "Result")], defaults: { value: "0", min: "0", max: "1" } },
  { typeId: "FloatLerp", displayName: "Float Lerp", category: "Math", pins: [pin("a", "in", "float", "A"), pin("b", "in", "float", "B"), pin("t", "in", "float", "T"), pin("result", "out", "float", "Result")], defaults: { a: "0", b: "1", t: "0.5" } },
  { typeId: "FloatAbs", displayName: "Float Abs", category: "Math", pins: [pin("value", "in", "float", "Value"), pin("result", "out", "float", "Result")], defaults: { value: "0" } },
  { typeId: "FloatRound", displayName: "Float Round", category: "Math", pins: [pin("value", "in", "float", "Value"), pin("result", "out", "float", "Result")], defaults: { value: "0" } },
  { typeId: "FloatSign", displayName: "Float Sign", category: "Math", pins: [pin("value", "in", "float", "Value"), pin("result", "out", "float", "Result")], defaults: { value: "0" } },
  { typeId: "RandomFloat", displayName: "Random Float", category: "Math", pins: [pin("min", "in", "float", "Min"), pin("max", "in", "float", "Max"), pin("result", "out", "float", "Result")], defaults: { min: "0", max: "1" } },
  { typeId: "IntOperator", displayName: "Int Operator", category: "Math", pins: [pin("a", "in", "int", "A"), pin("b", "in", "int", "B"), pin("result", "out", "int", "Result")], defaults: { a: "0", b: "0", operation: "add" } },
  { typeId: "RandomInt", displayName: "Random Int", category: "Math", pins: [pin("min", "in", "int", "Min"), pin("max", "in", "int", "Max"), pin("result", "out", "int", "Result")], defaults: { min: "0", max: "10" } },
  { typeId: "MakeVector3", displayName: "Make Vector3", category: "Vector3", pins: [pin("x", "in", "float", "X"), pin("y", "in", "float", "Y"), pin("z", "in", "float", "Z"), pin("vector", "out", "float3", "Vector")], defaults: { x: "0", y: "0", z: "0" } },
  { typeId: "GetVector3XYZ", displayName: "Get Vector3 XYZ", category: "Vector3", pins: [pin("vector", "in", "float3", "Vector"), pin("x", "out", "float", "X"), pin("y", "out", "float", "Y"), pin("z", "out", "float", "Z")], defaults: { vector: "0, 0, 0" } },
  { typeId: "Vector3Operator", displayName: "Vector3 Operator", category: "Vector3", pins: [pin("a", "in", "float3", "A"), pin("b", "in", "float3", "B"), pin("result", "out", "float3", "Result")], defaults: { a: "0, 0, 0", b: "0, 0, 0", operation: "add" } },
  { typeId: "Vector3Scale", displayName: "Vector3 Scale", category: "Vector3", pins: [pin("vector", "in", "float3", "Vector"), pin("scale", "in", "float", "Scale"), pin("result", "out", "float3", "Result")], defaults: { vector: "0, 0, 0", scale: "1" } },
  { typeId: "Vector3Normalize", displayName: "Vector3 Normalize", category: "Vector3", pins: [pin("vector", "in", "float3", "Vector"), pin("result", "out", "float3", "Result")], defaults: { vector: "0, 0, 0" } },
  { typeId: "Vector3Magnitude", displayName: "Vector3 Magnitude", category: "Vector3", pins: [pin("vector", "in", "float3", "Vector"), pin("result", "out", "float", "Result")], defaults: { vector: "0, 0, 0" } },
  { typeId: "Vector3Distance", displayName: "Vector3 Distance", category: "Vector3", pins: [pin("a", "in", "float3", "A"), pin("b", "in", "float3", "B"), pin("result", "out", "float", "Result")], defaults: { a: "0, 0, 0", b: "0, 0, 0" } },
  { typeId: "Vector3Dot", displayName: "Vector3 Dot", category: "Vector3", pins: [pin("a", "in", "float3", "A"), pin("b", "in", "float3", "B"), pin("result", "out", "float", "Result")], defaults: { a: "0, 0, 0", b: "0, 0, 0" } },
  { typeId: "Vector3Cross", displayName: "Vector3 Cross", category: "Vector3", pins: [pin("a", "in", "float3", "A"), pin("b", "in", "float3", "B"), pin("result", "out", "float3", "Result")], defaults: { a: "0, 0, 0", b: "0, 0, 0" } },
  { typeId: "Vector3Lerp", displayName: "Vector3 Lerp", category: "Vector3", pins: [pin("a", "in", "float3", "A"), pin("b", "in", "float3", "B"), pin("t", "in", "float", "T"), pin("result", "out", "float3", "Result")], defaults: { a: "0, 0, 0", b: "0, 0, 0", t: "0.5" } },
  { typeId: "GetTime", displayName: "Get Time", category: "Time", pins: [pin("time", "out", "float", "Time")], defaults: {} },
  { typeId: "GetDeltaTime", displayName: "Get Delta Time", category: "Time", pins: [pin("deltaTime", "out", "float", "Delta Time")], defaults: {} },
  { typeId: "SetTimeScale", displayName: "Set Time Scale", category: "Time", pins: flowPins([pin("scale", "in", "float", "Scale")]), defaults: { scale: "1" } },
  { typeId: "QuitApplication", displayName: "Quit Application", category: "Application", pins: flowPins(), defaults: {} },
  { typeId: "SetSkyTimeOfDay", displayName: "Set Sky Time Of Day", category: "Sky", pins: flowPins([pin("hours", "in", "float", "Hours"), pin("animate", "in", "bool", "Animate"), pin("cycleSeconds", "in", "float", "Cycle Seconds")]), defaults: { hours: "12", animate: "false", cycleSeconds: "120" } },
  { typeId: "PlaySound", displayName: "Play Sound", category: "Audio", pins: flowPins([pin("clipGuid", "in", "string", "Clip"), pin("volume", "in", "float", "Volume"), pin("pitch", "in", "float", "Pitch"), pin("loop", "in", "bool", "Loop"), pin("spatialized", "in", "bool", "3D"), pin("position", "in", "float3", "Position")]), defaults: { clipGuid: "", volume: "1", pitch: "1", loop: "false", spatialized: "false", position: "0, 0, 0", worldId: "0", bus: "2" } },
  { typeId: "StopAllSounds", displayName: "Stop All Sounds", category: "Audio", pins: flowPins(), defaults: {} },
  { typeId: "SetBusVolume", displayName: "Set Bus Volume", category: "Audio", pins: flowPins([pin("bus", "in", "int", "Bus"), pin("volume", "in", "float", "Volume")]), defaults: { bus: "0", volume: "1" } },
  { typeId: "CreateEntity", displayName: "Create Entity", category: "Game Object", pins: flowPins([pin("name", "in", "string", "Name")]), defaults: { name: "Entity" } },
  { typeId: "DestroyEntity", displayName: "Destroy Entity", category: "Game Object", pins: flowPins([pin("entity", "in", "entity", "Entity")]), defaults: { entity: "self" } },
  { typeId: "SetEntityEnabled", displayName: "Set Entity Enabled", category: "Game Object", pins: flowPins([pin("entity", "in", "entity", "Entity"), pin("enabled", "in", "bool", "Enabled")]), defaults: { entity: "self", enabled: "true" } },
  { typeId: "FindEntityByName", displayName: "Find Entity By Name", category: "Game Object", pins: [pin("name", "in", "string", "Name"), pin("entity", "out", "entity", "Entity")], defaults: { name: "Player" } },
  { typeId: "SetPosition", displayName: "Set Position", category: "Transform", pins: flowPins([pin("entity", "in", "entity", "Entity"), pin("position", "in", "float3", "Position")]), defaults: { entity: "self", position: "0, 0, 0" } },
  { typeId: "GetPosition", displayName: "Get Position", category: "Transform", pins: [pin("entity", "in", "entity", "Entity"), pin("position", "out", "float3", "Position")], defaults: { entity: "self" } },
  { typeId: "Translate", displayName: "Translate", category: "Transform", pins: flowPins([pin("entity", "in", "entity", "Entity"), pin("delta", "in", "float3", "Delta")]), defaults: { entity: "self", delta: "0, 0, 0" } },
  { typeId: "Rotate", displayName: "Rotate", category: "Transform", pins: flowPins([pin("entity", "in", "entity", "Entity"), pin("euler", "in", "float3", "Euler")]), defaults: { entity: "self", euler: "0, 0, 0" } },
  { typeId: "LookAt", displayName: "Look At", category: "Transform", pins: flowPins([pin("entity", "in", "entity", "Entity"), pin("target", "in", "float3", "Target")]), defaults: { entity: "self", target: "0, 0, 0" } },
  { typeId: "AddForce", displayName: "Add Force", category: "Physics", pins: flowPins([pin("entity", "in", "entity", "Entity"), pin("force", "in", "float3", "Force")]), defaults: { entity: "self", force: "0, 10, 0" } },
  { typeId: "SetVelocity", displayName: "Set Velocity", category: "Physics", pins: flowPins([pin("entity", "in", "entity", "Entity"), pin("velocity", "in", "float3", "Velocity")]), defaults: { entity: "self", velocity: "0, 0, 0" } },
  { typeId: "Raycast", displayName: "Raycast", category: "Physics", pins: [pin("in", "in", "flow", "In"), pin("origin", "in", "float3", "Origin"), pin("direction", "in", "float3", "Direction"), pin("distance", "in", "float", "Distance"), pin("hit", "out", "flow", "Hit"), pin("miss", "out", "flow", "Miss")], defaults: { origin: "0, 0, 0", direction: "0, 0, -1", distance: "100" } },
  { typeId: "CollisionEvent", displayName: "Collision Event", category: "Physics", pins: [pin("in", "in", "flow", "In"), pin("entity", "in", "entity", "Entity"), pin("tag", "in", "string", "Tag"), pin("hit", "out", "flow", "Hit"), pin("miss", "out", "flow", "Miss")], defaults: { entity: "self", phase: "enter", tag: "" } },
  { typeId: "TriggerEvent", displayName: "Trigger Event", category: "Physics", pins: [pin("in", "in", "flow", "In"), pin("entity", "in", "entity", "Entity"), pin("tag", "in", "string", "Tag"), pin("entered", "out", "flow", "Entered"), pin("miss", "out", "flow", "Miss")], defaults: { entity: "self", phase: "enter", tag: "" } },
  { typeId: "GetInputAction", displayName: "Get Input Action", category: "Input", pins: [pin("action", "in", "string", "Action"), pin("pressed", "out", "bool", "Pressed")], defaults: { action: "Jump" } },
  { typeId: "GetInputAxis", displayName: "Get Input Axis", category: "Input", pins: [pin("axis", "in", "string", "Axis"), pin("value", "out", "float", "Value")], defaults: { axis: "Horizontal" } },
  { typeId: "LoadScene", displayName: "Load Scene", category: "Scene", pins: flowPins([pin("scene", "in", "string", "Scene")]), defaults: { scene: "Main" } },
  { typeId: "ReloadScene", displayName: "Reload Scene", category: "Scene", pins: flowPins(), defaults: {} },
  { typeId: "ShowPanel", displayName: "Show Panel", category: "UI", pins: flowPins([pin("panel", "in", "string", "Panel")]), defaults: { panel: "HUD" } },
  { typeId: "HidePanel", displayName: "Hide Panel", category: "UI", pins: flowPins([pin("panel", "in", "string", "Panel")]), defaults: { panel: "HUD" } },
  { typeId: "SetText", displayName: "Set Text", category: "UI", pins: flowPins([pin("element", "in", "string", "Element"), pin("text", "in", "string", "Text")]), defaults: { element: "Label", text: "" } },
  { typeId: "SetLightIntensity", displayName: "Set Light Intensity", category: "Rendering", pins: flowPins([pin("entity", "in", "entity", "Light"), pin("intensity", "in", "float", "Intensity")]), defaults: { entity: "self", intensity: "1" } },
  { typeId: "SetCameraActive", displayName: "Set Camera Active", category: "Rendering", pins: flowPins([pin("entity", "in", "entity", "Camera"), pin("active", "in", "bool", "Active")]), defaults: { entity: "self", active: "true" } },
  { typeId: "ThirdPersonCameraFollow", displayName: "Third Person Camera Follow", category: "Rendering", pins: flowPins([pin("camera", "in", "entity", "Camera"), pin("target", "in", "entity", "Target"), pin("offset", "in", "float3", "Offset"), pin("lookOffset", "in", "float3", "Look Offset"), pin("positionSmoothing", "in", "float", "Position Smoothing"), pin("rotationSmoothing", "in", "float", "Rotation Smoothing")]), defaults: { camera: "Main Camera", target: "self", offset: "0, 2.6, -7", lookOffset: "0, 1.25, 0", positionSmoothing: "8", rotationSmoothing: "12" } },
  { typeId: "MoveTo", displayName: "Move To", category: "Navigation", pins: flowPins([pin("entity", "in", "entity", "Entity"), pin("target", "in", "float3", "Target")]), defaults: { entity: "self", target: "0, 0, 0" } },
  { typeId: "StopMove", displayName: "Stop Move", category: "Navigation", pins: flowPins([pin("entity", "in", "entity", "Entity")]), defaults: { entity: "self" } },
  { typeId: "Log", displayName: "Log", category: "Debug", pins: flowPins([pin("message", "in", "string", "Message")]), defaults: { message: "Hello" } },
  { typeId: "DrawDebugLine", displayName: "Draw Debug Line", category: "Debug", pins: flowPins([pin("from", "in", "float3", "From"), pin("to", "in", "float3", "To")]), defaults: { from: "0, 0, 0", to: "0, 1, 0", duration: "0" } },
];

export function findGameGraphAction(typeId: string): GameGraphAction {
  const action = kGameGraphActions.find(a => a.typeId === typeId);
  if (!action) throw new Error(`Unknown game graph action type '${typeId}'. Use list_game_graph_actions.`);
  return action;
}
