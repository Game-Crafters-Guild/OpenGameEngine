import { kGameGraphActions } from "./actions.js";
import type { GameGraphLink, GameGraphModel, GameGraphNode } from "./model.js";

// Lower a game_logic graph to C++ that calls native action implementations —
// the fast-play path. Emission walks flow pins from the Entry node; value pins
// are resolved to expressions on demand.

export function sanitizeCppIdentifier(value: string): string {
  const sanitized = value.replace(/[^A-Za-z0-9_]/g, "_").replace(/^[0-9]/, "_$&");
  return sanitized || "ExecuteGameLogicGraph";
}

function cppString(value: string): string {
  return JSON.stringify(value);
}

export function compileGameGraphCpp(model: GameGraphModel, functionName: string): string {
  const safeName = sanitizeCppIdentifier(functionName);
  const byId = new Map(model.nodes.map(n => [n.id, n]));
  const outLink = (nodeId: string, pinId: string): GameGraphLink | undefined =>
    model.links.find(l => l.sourceNodeId === nodeId && l.sourcePinId === pinId);
  const inLink = (nodeId: string, pinId: string): GameGraphLink | undefined =>
    model.links.find(l => l.targetNodeId === nodeId && l.targetPinId === pinId);
  const param = (node: GameGraphNode, key: string): string => {
    const action = kGameGraphActions.find(a => a.typeId === node.typeId);
    return node.parameters?.[key] ?? action?.defaults[key] ?? "";
  };
  const trim = (value: string): string => value.trim();
  const isTrue = (value: string): boolean => ["true", "1", "yes", "on"].includes(trim(value).toLowerCase());
  const numberText = (value: string, fallback: string): string => {
    const text = trim(value);
    if (text.length === 0) return fallback;
    const parsed = Number(text);
    return Number.isFinite(parsed) ? text : fallback;
  };
  const floatLiteral = (value: string, fallback = "0"): string => {
    let text = numberText(value, fallback);
    if (!/[.eE]/.test(text)) text += ".0";
    return `${text}f`;
  };
  const intLiteral = (value: string, fallback = "0"): string => {
    const text = trim(value);
    if (/^-?\d+$/.test(text)) return text;
    return fallback;
  };
  const vec3Literal = (value: string): string => {
    const parts = value.replace(/[(),;]/g, " ").trim().split(/\s+/).filter(Boolean);
    return `GameLogicVec3{${floatLiteral(parts[0] ?? "0")}, ${floatLiteral(parts[1] ?? "0")}, ${floatLiteral(parts[2] ?? "0")}}`;
  };
  const literalForType = (type: string, value: string): string => {
    if (type === "bool") return isTrue(value) ? "true" : "false";
    if (type === "float") return floatLiteral(value);
    if (type === "int") return intLiteral(value);
    if (type === "float3") return vec3Literal(value);
    if (type === "entity") return trim(value).length === 0 || value === "self" ? "ctx.Self()" : `ctx.FindEntityByName(${cppString(value)})`;
    return cppString(value);
  };
  const valueExpr = (node: GameGraphNode, pinId: string, type: string, fallbackKey = pinId): string => {
    const link = inLink(node.id, pinId);
    const source = link ? byId.get(link.sourceNodeId) : undefined;
    if (source) return sourceValueExpr(source, link!.sourcePinId, type);
    return literalForType(type, param(node, fallbackKey));
  };
  const entityExpr = (node: GameGraphNode, pinId: string): string => valueExpr(node, pinId, "entity", pinId);
  const sourceValueExpr = (source: GameGraphNode, sourcePinId: string, targetType: string): string => {
    if (source.typeId === "GetVariable") {
      const name = param(source, "variableName") || "value";
      if (targetType === "bool") return `ctx.GetBoolVariable(${cppString(name)})`;
      if (targetType === "float") return `ctx.GetFloatVariable(${cppString(name)})`;
      return `ctx.GetStringVariable(${cppString(name)})`;
    }
    if (source.typeId === "FindEntityByName") return `ctx.FindEntityByName(${cppString(param(source, "name") || "Entity")})`;
    if (source.typeId === "GetPosition") return `ctx.GetPosition(${entityExpr(source, "entity")})`;
    if (source.typeId === "GetInputAction") return `ctx.GetInputAction(${cppString(param(source, "action") || "Jump")})`;
    if (source.typeId === "GetInputAxis") return `ctx.GetInputAxis(${cppString(param(source, "axis") || "Horizontal")})`;
    if (source.typeId === "BoolOperator") return `ctx.BoolOperator(${valueExpr(source, "a", "bool", "a")}, ${valueExpr(source, "b", "bool", "b")}, ${cppString(param(source, "operation") || "and")})`;
    if (source.typeId === "BoolNot") return `ctx.BoolNot(${valueExpr(source, "value", "bool", "value")})`;
    if (source.typeId === "FloatOperator") return `ctx.FloatOperator(${valueExpr(source, "a", "float", "a")}, ${valueExpr(source, "b", "float", "b")}, ${cppString(param(source, "operation") || "add")})`;
    if (source.typeId === "FloatClamp") return `ctx.FloatClamp(${valueExpr(source, "value", "float", "value")}, ${valueExpr(source, "min", "float", "min")}, ${valueExpr(source, "max", "float", "max")})`;
    if (source.typeId === "FloatLerp") return `ctx.FloatLerp(${valueExpr(source, "a", "float", "a")}, ${valueExpr(source, "b", "float", "b")}, ${valueExpr(source, "t", "float", "t")})`;
    if (source.typeId === "FloatAbs") return `ctx.FloatAbs(${valueExpr(source, "value", "float", "value")})`;
    if (source.typeId === "FloatRound") return `ctx.FloatRound(${valueExpr(source, "value", "float", "value")})`;
    if (source.typeId === "FloatSign") return `ctx.FloatSign(${valueExpr(source, "value", "float", "value")})`;
    if (source.typeId === "RandomFloat") return `ctx.RandomFloat(${valueExpr(source, "min", "float", "min")}, ${valueExpr(source, "max", "float", "max")})`;
    if (source.typeId === "IntOperator") return `ctx.IntOperator(${valueExpr(source, "a", "int", "a")}, ${valueExpr(source, "b", "int", "b")}, ${cppString(param(source, "operation") || "add")})`;
    if (source.typeId === "RandomInt") return `ctx.RandomInt(${valueExpr(source, "min", "int", "min")}, ${valueExpr(source, "max", "int", "max")})`;
    if (source.typeId === "MakeVector3") return `ctx.MakeVector3(${valueExpr(source, "x", "float", "x")}, ${valueExpr(source, "y", "float", "y")}, ${valueExpr(source, "z", "float", "z")})`;
    if (source.typeId === "GetVector3XYZ") {
      const vector = valueExpr(source, "vector", "float3", "vector");
      const member = sourcePinId === "x" ? "X" : sourcePinId === "y" ? "Y" : "Z";
      return `(${vector}).${member}`;
    }
    if (source.typeId === "Vector3Operator") return `ctx.Vector3Operator(${valueExpr(source, "a", "float3", "a")}, ${valueExpr(source, "b", "float3", "b")}, ${cppString(param(source, "operation") || "add")})`;
    if (source.typeId === "Vector3Scale") return `ctx.Vector3Scale(${valueExpr(source, "vector", "float3", "vector")}, ${valueExpr(source, "scale", "float", "scale")})`;
    if (source.typeId === "Vector3Normalize") return `ctx.Vector3Normalize(${valueExpr(source, "vector", "float3", "vector")})`;
    if (source.typeId === "Vector3Magnitude") return `ctx.Vector3Magnitude(${valueExpr(source, "vector", "float3", "vector")})`;
    if (source.typeId === "Vector3Distance") return `ctx.Vector3Distance(${valueExpr(source, "a", "float3", "a")}, ${valueExpr(source, "b", "float3", "b")})`;
    if (source.typeId === "Vector3Dot") return `ctx.Vector3Dot(${valueExpr(source, "a", "float3", "a")}, ${valueExpr(source, "b", "float3", "b")})`;
    if (source.typeId === "Vector3Cross") return `ctx.Vector3Cross(${valueExpr(source, "a", "float3", "a")}, ${valueExpr(source, "b", "float3", "b")})`;
    if (source.typeId === "Vector3Lerp") return `ctx.Vector3Lerp(${valueExpr(source, "a", "float3", "a")}, ${valueExpr(source, "b", "float3", "b")}, ${valueExpr(source, "t", "float", "t")})`;
    if (source.typeId === "GetTime") return "ctx.GetTime()";
    if (source.typeId === "GetDeltaTime") return "ctx.GetDeltaTime()";
    return literalForType(targetType, param(source, sourcePinId));
  };
  const valueOnly = new Set(["GetVariable", "FindEntityByName", "GetPosition", "GetInputAction", "GetInputAxis", "BoolOperator", "BoolNot", "FloatOperator", "FloatClamp", "FloatLerp", "FloatAbs", "FloatRound", "FloatSign", "RandomFloat", "IntOperator", "RandomInt", "MakeVector3", "GetVector3XYZ", "Vector3Operator", "Vector3Scale", "Vector3Normalize", "Vector3Magnitude", "Vector3Distance", "Vector3Dot", "Vector3Cross", "Vector3Lerp", "GetTime", "GetDeltaTime"]);
  const emitNode = (node: GameGraphNode, lines: string[], active: Set<string>): void => {
    if (active.has(node.id)) { lines.push(`    ctx.TraceAction(${cppString(`cycle:${node.typeId}`)});`); return; }
    active.add(node.id);
    const emitOut = (pinId: string) => {
      const link = outLink(node.id, pinId);
      const target = link ? byId.get(link.targetNodeId) : undefined;
      if (target) emitNode(target, lines, active);
    };
    if (node.typeId === "Sequence") {
      const outputs = node.pins
        .filter(p => p.direction === "out" && /^out\d+$/.test(p.id))
        .map(p => ({ index: Number(p.id.slice(3)), id: p.id }))
        .sort((a, b) => a.index - b.index || a.id.localeCompare(b.id));
      for (const output of outputs) emitOut(output.id);
    }
    else if (node.typeId === "Branch") {
      lines.push(`    if (${valueExpr(node, "condition", "bool", "condition")})`);
      lines.push("    {");
      emitOut("true");
      lines.push("    }");
      lines.push("    else");
      lines.push("    {");
      emitOut("false");
      lines.push("    }");
    } else if (node.typeId === "CompareFloat") {
      lines.push(`    if (ctx.CompareFloat(${valueExpr(node, "a", "float", "a")}, ${valueExpr(node, "b", "float", "b")}, ${cppString(param(node, "comparison") || "greater")}))`);
      lines.push("    {");
      emitOut("true");
      lines.push("    }");
      lines.push("    else");
      lines.push("    {");
      emitOut("false");
      lines.push("    }");
    } else if (node.typeId === "CompareBool") {
      lines.push(`    if (${valueExpr(node, "value", "bool", "value")} == ${literalForType("bool", param(node, "expected") || "true")})`);
      lines.push("    {");
      emitOut("true");
      lines.push("    }");
      lines.push("    else");
      lines.push("    {");
      emitOut("false");
      lines.push("    }");
    } else if (node.typeId === "CompareInt") {
      lines.push(`    if (ctx.CompareInt(${valueExpr(node, "a", "int", "a")}, ${valueExpr(node, "b", "int", "b")}, ${cppString(param(node, "comparison") || "equal")}))`);
      lines.push("    {");
      emitOut("true");
      lines.push("    }");
      lines.push("    else");
      lines.push("    {");
      emitOut("false");
      lines.push("    }");
    } else if (node.typeId === "Raycast") {
      lines.push(`    if (ctx.Raycast(${valueExpr(node, "origin", "float3", "origin")}, ${valueExpr(node, "direction", "float3", "direction")}, ${valueExpr(node, "distance", "float", "distance")}))`);
      lines.push("    {");
      emitOut("hit");
      lines.push("    }");
      lines.push("    else");
      lines.push("    {");
      emitOut("miss");
      lines.push("    }");
    } else if (node.typeId === "CollisionEvent") {
      lines.push(`    if (ctx.CollisionEvent(${entityExpr(node, "entity")}, ${cppString(param(node, "phase") || "enter")}, ${valueExpr(node, "tag", "string", "tag")}))`);
      lines.push("    {");
      emitOut("hit");
      lines.push("    }");
      lines.push("    else");
      lines.push("    {");
      emitOut("miss");
      lines.push("    }");
    } else if (node.typeId === "TriggerEvent") {
      lines.push(`    if (ctx.TriggerEvent(${entityExpr(node, "entity")}, ${cppString(param(node, "phase") || "enter")}, ${valueExpr(node, "tag", "string", "tag")}))`);
      lines.push("    {");
      emitOut("entered");
      lines.push("    }");
      lines.push("    else");
      lines.push("    {");
      emitOut("miss");
      lines.push("    }");
    } else if (node.typeId === "Finish") {
      lines.push("    return;");
    } else if (node.typeId === "Delay") {
      lines.push(`    ctx.Delay(${valueExpr(node, "seconds", "float", "seconds")});`);
      emitOut("out");
    } else if (node.typeId === "SendEvent") {
      lines.push(`    ctx.SendEvent(${valueExpr(node, "eventName", "string", "eventName")});`);
      emitOut("out");
    } else if (node.typeId === "PlaySound") {
      lines.push(`    ctx.PlaySound(${valueExpr(node, "clipGuid", "string", "clipGuid")}, ${valueExpr(node, "volume", "float", "volume")}, ${valueExpr(node, "pitch", "float", "pitch")}, ${valueExpr(node, "loop", "bool", "loop")}, ${valueExpr(node, "spatialized", "bool", "spatialized")}, ${valueExpr(node, "position", "float3", "position")}, ${literalForType("int", param(node, "worldId") || "0")}, ${literalForType("int", param(node, "bus") || "2")});`);
      emitOut("out");
    } else if (node.typeId === "Log") {
      lines.push(`    ctx.Log(${valueExpr(node, "message", "string", "message")});`);
      emitOut("out");
    } else if (node.typeId === "SetVariable") {
      lines.push(`    ctx.SetVariable(${cppString(param(node, "variableName") || "value")}, ${valueExpr(node, "value", "string", "value")});`);
      emitOut("out");
    } else if (node.typeId === "StopAllSounds") { lines.push("    ctx.StopAllSounds();"); emitOut("out");
    } else if (node.typeId === "SetBusVolume") { lines.push(`    ctx.SetBusVolume(${valueExpr(node, "bus", "int", "bus")}, ${valueExpr(node, "volume", "float", "volume")});`); emitOut("out");
    } else if (node.typeId === "CreateEntity") { lines.push(`    (void)ctx.CreateEntity(${valueExpr(node, "name", "string", "name")});`); emitOut("out");
    } else if (node.typeId === "DestroyEntity") { lines.push(`    ctx.DestroyEntity(${entityExpr(node, "entity")});`); emitOut("out");
    } else if (node.typeId === "SetEntityEnabled") { lines.push(`    ctx.SetEntityEnabled(${entityExpr(node, "entity")}, ${valueExpr(node, "enabled", "bool", "enabled")});`); emitOut("out");
    } else if (node.typeId === "SetPosition") { lines.push(`    ctx.SetPosition(${entityExpr(node, "entity")}, ${valueExpr(node, "position", "float3", "position")});`); emitOut("out");
    } else if (node.typeId === "Translate") { lines.push(`    ctx.Translate(${entityExpr(node, "entity")}, ${valueExpr(node, "delta", "float3", "delta")});`); emitOut("out");
    } else if (node.typeId === "Rotate") { lines.push(`    ctx.Rotate(${entityExpr(node, "entity")}, ${valueExpr(node, "euler", "float3", "euler")});`); emitOut("out");
    } else if (node.typeId === "LookAt") { lines.push(`    ctx.LookAt(${entityExpr(node, "entity")}, ${valueExpr(node, "target", "float3", "target")});`); emitOut("out");
    } else if (node.typeId === "AddForce") { lines.push(`    ctx.AddForce(${entityExpr(node, "entity")}, ${valueExpr(node, "force", "float3", "force")});`); emitOut("out");
    } else if (node.typeId === "SetVelocity") { lines.push(`    ctx.SetVelocity(${entityExpr(node, "entity")}, ${valueExpr(node, "velocity", "float3", "velocity")});`); emitOut("out");
    } else if (node.typeId === "PlayAnimation") { lines.push(`    ctx.PlayAnimation(${entityExpr(node, "entity")}, ${valueExpr(node, "clipGuid", "string", "clipGuid")}, ${literalForType("bool", param(node, "loop") || "false")});`); emitOut("out");
    } else if (node.typeId === "SetAnimatorBool") { lines.push(`    ctx.SetAnimatorBool(${entityExpr(node, "entity")}, ${valueExpr(node, "name", "string", "name")}, ${valueExpr(node, "value", "bool", "value")});`); emitOut("out");
    } else if (node.typeId === "SetAnimatorFloat") { lines.push(`    ctx.SetAnimatorFloat(${entityExpr(node, "entity")}, ${valueExpr(node, "name", "string", "name")}, ${valueExpr(node, "value", "float", "value")});`); emitOut("out");
    } else if (node.typeId === "SetAnimatorTrigger") { lines.push(`    ctx.SetAnimatorTrigger(${entityExpr(node, "entity")}, ${valueExpr(node, "name", "string", "name")});`); emitOut("out");
    } else if (node.typeId === "LoadScene") { lines.push(`    ctx.LoadScene(${valueExpr(node, "scene", "string", "scene")});`); emitOut("out");
    } else if (node.typeId === "ReloadScene") { lines.push("    ctx.ReloadScene();"); emitOut("out");
    } else if (node.typeId === "SetTimeScale") { lines.push(`    ctx.SetTimeScale(${valueExpr(node, "scale", "float", "scale")});`); emitOut("out");
    } else if (node.typeId === "QuitApplication") { lines.push("    ctx.QuitApplication();"); emitOut("out");
    } else if (node.typeId === "SetSkyTimeOfDay") { lines.push(`    ctx.SetSkyTimeOfDay(${valueExpr(node, "hours", "float", "hours")}, ${valueExpr(node, "animate", "bool", "animate")}, ${valueExpr(node, "cycleSeconds", "float", "cycleSeconds")});`); emitOut("out");
    } else if (node.typeId === "ShowPanel") { lines.push(`    ctx.ShowPanel(${valueExpr(node, "panel", "string", "panel")});`); emitOut("out");
    } else if (node.typeId === "HidePanel") { lines.push(`    ctx.HidePanel(${valueExpr(node, "panel", "string", "panel")});`); emitOut("out");
    } else if (node.typeId === "SetText") { lines.push(`    ctx.SetText(${valueExpr(node, "element", "string", "element")}, ${valueExpr(node, "text", "string", "text")});`); emitOut("out");
    } else if (node.typeId === "SetLightIntensity") { lines.push(`    ctx.SetLightIntensity(${entityExpr(node, "entity")}, ${valueExpr(node, "intensity", "float", "intensity")});`); emitOut("out");
    } else if (node.typeId === "SetCameraActive") { lines.push(`    ctx.SetCameraActive(${entityExpr(node, "entity")}, ${valueExpr(node, "active", "bool", "active")});`); emitOut("out");
    } else if (node.typeId === "ThirdPersonCameraFollow") { lines.push(`    ctx.ThirdPersonCameraFollow(${entityExpr(node, "camera")}, ${entityExpr(node, "target")}, ${valueExpr(node, "offset", "float3", "offset")}, ${valueExpr(node, "lookOffset", "float3", "lookOffset")}, ${valueExpr(node, "positionSmoothing", "float", "positionSmoothing")}, ${valueExpr(node, "rotationSmoothing", "float", "rotationSmoothing")});`); emitOut("out");
    } else if (node.typeId === "MoveTo") { lines.push(`    ctx.MoveTo(${entityExpr(node, "entity")}, ${valueExpr(node, "target", "float3", "target")});`); emitOut("out");
    } else if (node.typeId === "StopMove") { lines.push(`    ctx.StopMove(${entityExpr(node, "entity")});`); emitOut("out");
    } else if (node.typeId === "DrawDebugLine") { lines.push(`    ctx.DrawDebugLine(${valueExpr(node, "from", "float3", "from")}, ${valueExpr(node, "to", "float3", "to")}, ${literalForType("float", param(node, "duration") || "0")});`); emitOut("out");
    } else if (!kGameGraphActions.some(a => a.typeId === node.typeId)) {
      lines.push(`    GameEngine::GameGraphActions::${sanitizeCppIdentifier(node.typeId)}(ctx);`);
      emitOut("out");
    } else if (!valueOnly.has(node.typeId)) {
      lines.push(`    ctx.TraceAction(${cppString(node.typeId)});`);
      emitOut("out");
    }
    active.delete(node.id);
  };
  const entry = model.nodes.find(n => n.typeId === "Entry");
  if (!entry) throw new Error("Game graph requires an Entry node.");
  const retiredAnimatorActions = new Set([
    "PlayAnimation",
    "SetAnimatorBool",
    "SetAnimatorFloat",
    "SetAnimatorTrigger",
  ]);
  const customTypes = Array.from(new Set(model.nodes
    .filter(n => n.typeId !== "Entry"
      && !retiredAnimatorActions.has(n.typeId)
      && !kGameGraphActions.some(a => a.typeId === n.typeId))
    .map(n => sanitizeCppIdentifier(n.typeId))));
  const lines = [`#include "Graph/GameLogicRuntime.h"`, ""];
  if (customTypes.length > 0) {
    lines.push("namespace GameEngine::GameGraphActions");
    lines.push("{");
    for (const type of customTypes)
      lines.push(`void ${type}(GameLogicRuntimeContext& ctx);`);
    lines.push("} // namespace GameEngine::GameGraphActions");
    lines.push("");
  }
  lines.push(`void ${safeName}(GameEngine::GameLogicRuntimeContext& ctx)`);
  lines.push("{");
  lines.push("    using namespace GameEngine;");
  lines.push("    (void)ctx;");
  const first = outLink(entry.id, "out");
  const target = first ? byId.get(first.targetNodeId) : undefined;
  if (target) emitNode(target, lines, new Set<string>());
  lines.push("}");
  lines.push("");
  return lines.join("\n");
}
