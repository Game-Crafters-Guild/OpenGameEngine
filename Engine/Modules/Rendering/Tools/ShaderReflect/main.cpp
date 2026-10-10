#include <algorithm>
#include <cstdio>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cctype>
#include <cstring>
#include <span>
#include <string_view>

#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Materials/ShaderMetaJson.h"
#include "Rendering/Materials/ShaderMetaValidation.h"
#include "Rendering/ShaderCache/ShaderMetaBinary.h"
#include "Rendering/ShaderCache/ShaderPackageContainer.h"
#include <nlohmann/json.hpp>
#include <filesystem>

using namespace GameEngine::Rendering;

static bool ReadFileWords(const std::string& path, std::vector<uint32_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() % 4 != 0) return false;
    out.resize(bytes.size() / 4);
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return true;
}

static bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

static bool WriteFileBytes(const std::string& path, std::span<const uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

static bool WriteShaderPkg(const std::string& outPath,
                           const ShaderMeta& meta,
                           const std::vector<std::pair<std::string, std::vector<uint32_t>>>& stages) {
    std::vector<uint8_t> metaBytes;
    std::string error;
    if (!EncodeShaderMetaBinary(meta, metaBytes, &error)) { std::cerr << error << "\n"; return false; }
    std::vector<ShaderPackageChunkSource> chunks;
    chunks.push_back({std::string(kShaderPackageMetaChunk), metaBytes});
    for (const auto& [key, words] : stages) {
        chunks.push_back({key + std::string(kShaderPackageSpirvSuffix),
                          std::span(reinterpret_cast<const uint8_t*>(words.data()), words.size() * sizeof(uint32_t))});
    }
    std::vector<uint8_t> container;
    if (!WriteShaderPackageContainer(chunks, container, &error)) { std::cerr << error << "\n"; return false; }
    return WriteFileBytes(outPath, container);
}

// A package read whole, with its validated chunk table pointing into `Bytes`.
struct LoadedPackage {
    std::vector<uint8_t> Bytes;
    std::vector<ShaderPackageChunkView> Chunks;
};

static bool LoadPackage(const std::string& path, LoadedPackage& out) {
    if (!ReadFileBytes(path, out.Bytes)) { std::cerr << "Failed to open package: " << path << "\n"; return false; }
    std::string error;
    if (ReadShaderPackageContainer(out.Bytes, out.Chunks, &error) != ShaderPackageReadResult::Read) { std::cerr << path << ": " << error << "\n"; return false; }
    return true;
}

static const ShaderPackageChunkView* FindChunk(const LoadedPackage& pkg, std::string_view name) {
    for (const ShaderPackageChunkView& chunk : pkg.Chunks)
        if (chunk.Name == name) return &chunk;
    return nullptr;
}

// `--chunk NAME=PATH`: the chunk a read or append mode acts on, and the file
// that holds (append) or receives (read) its bytes.
struct ChunkFileArgument {
    std::string Name;
    std::string Path;
};

static bool ParseChunkFileArgument(const std::string& text, ChunkFileArgument& out) {
    const size_t separator = text.find('=');
    if (separator == std::string::npos || separator == 0 || separator + 1 == text.size()) return false;
    out.Name = text.substr(0, separator);
    out.Path = text.substr(separator + 1);
    return true;
}

// Print the package's meta as JSON. The package stores it as binary; JSON is
// this tool's output format, not the package's storage format.
static int InspectPackage(const std::string& path) {
    LoadedPackage pkg;
    if (!LoadPackage(path, pkg)) return 6;
    const ShaderPackageChunkView* meta = FindChunk(pkg, kShaderPackageMetaChunk);
    if (!meta) { std::cerr << "No meta-bin in package\n"; return 6; }
    ShaderMeta inspected{};
    std::string metaErr;
    if (!DecodeShaderMetaBinary(meta->Bytes.data(), meta->Bytes.size(), inspected, &metaErr)) {
        std::cerr << "Failed to decode meta-bin: " << metaErr << "\n"; return 6;
    }
    std::cout << nlohmann::json(inspected).dump() << std::endl;
    return 0;
}

// Print the chunk table as JSON, in file order: [{"name", "type", "size"}, ...].
static int ListPackageChunks(const std::string& path) {
    LoadedPackage pkg;
    if (!LoadPackage(path, pkg)) return 6;
    nlohmann::json listing = nlohmann::json::array();
    for (const ShaderPackageChunkView& chunk : pkg.Chunks)
        listing.push_back({{"name", chunk.Name}, {"type", chunk.Type}, {"size", chunk.Bytes.size()}});
    std::cout << listing.dump() << std::endl;
    return 0;
}

static int ReadPackageChunks(const std::string& path, const std::vector<ChunkFileArgument>& requests) {
    LoadedPackage pkg;
    if (!LoadPackage(path, pkg)) return 6;
    for (const ChunkFileArgument& request : requests) {
        const ShaderPackageChunkView* chunk = FindChunk(pkg, request.Name);
        if (!chunk) { std::cerr << path << ": no chunk named '" << request.Name << "'\n"; return 6; }
        if (!WriteFileBytes(request.Path, chunk->Bytes)) { std::cerr << "Failed to write " << request.Path << "\n"; return 5; }
    }
    return 0;
}

// Rewrite the package with the given chunks after its existing ones.
static int AppendPackageChunks(const std::string& path, const std::vector<ChunkFileArgument>& additions) {
    LoadedPackage pkg;
    if (!LoadPackage(path, pkg)) return 6;
    std::vector<std::vector<uint8_t>> addedBytes(additions.size());
    std::vector<ShaderPackageChunkSource> chunks;
    for (const ShaderPackageChunkView& chunk : pkg.Chunks) chunks.push_back({chunk.Name, chunk.Bytes});
    for (size_t i = 0; i < additions.size(); ++i) {
        if (!ReadFileBytes(additions[i].Path, addedBytes[i])) { std::cerr << "Failed to read " << additions[i].Path << "\n"; return 7; }
        chunks.push_back({additions[i].Name, addedBytes[i]});
    }
    std::vector<uint8_t> container;
    std::string error;
    if (!WriteShaderPackageContainer(chunks, container, &error)) { std::cerr << path << ": " << error << "\n"; return 5; }
    if (!WriteFileBytes(path, container)) { std::cerr << "Failed to write " << path << "\n"; return 5; }
    return 0;
}

// Rewrites each package without the SPIR-V of every stage that also carries WGSL: a browser
// reads only the WGSL (ShaderPackageIO serves a stage's WGSL in place of its SPIR-V), so a
// package shipped to one keeps SPIR-V only for a stage the cook gave no WGSL.
static int StripSpirvWithWgsl(const std::vector<std::string>& paths) {
    for (const std::string& path : paths) {
        LoadedPackage pkg;
        if (!LoadPackage(path, pkg)) return 6;
        std::vector<std::string> wgslStages;
        for (const ShaderPackageChunkView& chunk : pkg.Chunks) {
            if (chunk.Name.ends_with(kShaderPackageWgslSuffix))
                wgslStages.push_back(chunk.Name.substr(0, chunk.Name.size() - kShaderPackageWgslSuffix.size()));
        }
        std::vector<ShaderPackageChunkSource> chunks;
        for (const ShaderPackageChunkView& chunk : pkg.Chunks) {
            const bool spirvWithWgsl = chunk.Name.ends_with(kShaderPackageSpirvSuffix) &&
                std::find(wgslStages.begin(), wgslStages.end(),
                          chunk.Name.substr(0, chunk.Name.size() - kShaderPackageSpirvSuffix.size())) != wgslStages.end();
            if (!spirvWithWgsl) chunks.push_back({chunk.Name, chunk.Bytes});
        }
        if (chunks.size() == pkg.Chunks.size()) continue;
        std::vector<uint8_t> container;
        std::string error;
        if (!WriteShaderPackageContainer(chunks, container, &error)) { std::cerr << path << ": " << error << "\n"; return 5; }
        if (!WriteFileBytes(path, container)) { std::cerr << "Failed to write " << path << "\n"; return 5; }
    }
    return 0;
}

int main(int argc, char** argv) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    std::cerr << "Reflection is disabled at build time (RENDERING_ENABLE_SPIRV_REFLECTION=OFF)\n";
    return 2;
#endif

    std::string vs, fs, cs, ms, gs, outPath, recipePath, inspectPath, listPath, readPath, appendPath;
    std::vector<ChunkFileArgument> chunkArguments;
    std::vector<std::string> stripPaths;
    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if (a == "--vs" && i+1 < argc) vs = argv[++i];
        else if (a == "--fs" && i+1 < argc) fs = argv[++i];
        else if (a == "--cs" && i+1 < argc) cs = argv[++i];
        else if (a == "--ms" && i+1 < argc) ms = argv[++i];
        else if (a == "--gs" && i+1 < argc) gs = argv[++i];
        else if (a == "--out" && i+1 < argc) outPath = argv[++i];
        else if (a == "--recipe" && i+1 < argc) recipePath = argv[++i];
        else if (a == "--inspect" && i+1 < argc) inspectPath = argv[++i];
        else if (a == "--list-chunks" && i+1 < argc) listPath = argv[++i];
        else if (a == "--read-chunk" && i+1 < argc) readPath = argv[++i];
        else if (a == "--append-chunk" && i+1 < argc) appendPath = argv[++i];
        else if (a == "--strip-spirv-with-wgsl") { for (++i; i < argc; ++i) stripPaths.emplace_back(argv[i]); }
        else if (a == "--chunk" && i+1 < argc) {
            ChunkFileArgument chunk;
            if (!ParseChunkFileArgument(argv[++i], chunk)) { std::cerr << "--chunk expects NAME=PATH, got '" << argv[i] << "'\n"; return 1; }
            chunkArguments.push_back(std::move(chunk));
        }
        else if (a == "--help") {
            std::cout << "Usage: ShaderReflect.exe [--vs path] [--fs path] [--cs path] [--ms path] [--gs path] [--out out.json|out.shaderpkg] [--recipe recipe.json]\n"
                         "       ShaderReflect.exe --inspect file.shaderpkg        print the reflection metadata as JSON\n"
                         "       ShaderReflect.exe --list-chunks file.shaderpkg    print the chunk table as JSON\n"
                         "       ShaderReflect.exe --read-chunk file.shaderpkg --chunk NAME=PATH [--chunk NAME=PATH ...]\n"
                         "       ShaderReflect.exe --append-chunk file.shaderpkg --chunk NAME=PATH [--chunk NAME=PATH ...]\n"
                         "       ShaderReflect.exe --strip-spirv-with-wgsl file.shaderpkg [file.shaderpkg ...]\n";
            return 0;
        }
    }

    if (!inspectPath.empty()) return InspectPackage(inspectPath);
    if (!listPath.empty()) return ListPackageChunks(listPath);
    if (!stripPaths.empty()) return StripSpirvWithWgsl(stripPaths);
    if ((!readPath.empty() || !appendPath.empty()) && chunkArguments.empty()) {
        std::cerr << "--read-chunk and --append-chunk need at least one --chunk NAME=PATH\n";
        return 1;
    }
    if (!readPath.empty()) return ReadPackageChunks(readPath, chunkArguments);
    if (!appendPath.empty()) return AppendPackageChunks(appendPath, chunkArguments);

    // Recipe mode: populate stages from JSON recipe
    if (!recipePath.empty()) {
        std::ifstream rin(recipePath);
        if (!rin) { std::cerr << "Failed to open recipe: " << recipePath << "\n"; return 7; }
        nlohmann::json rj; rin >> rj;
        auto getStr = [&](const char* k)->std::string{ return rj.contains(k) && rj[k].is_string() ? rj[k].get<std::string>() : std::string(); };
        if (vs.empty()) vs = getStr("vs");
        if (fs.empty()) fs = getStr("fs");
        if (cs.empty()) cs = getStr("cs");
        if (ms.empty()) ms = getStr("ms");
        if (gs.empty()) gs = getStr("gs");
        if (outPath.empty() && rj.contains("out") && rj["out"].is_string()) outPath = rj["out"].get<std::string>();
        // Optional future: defines/spec constants per stage
    }

    if (vs.empty() && fs.empty() && cs.empty() && ms.empty() && gs.empty()) {
        std::cerr << "No inputs provided. Use --help for usage.\n";
        return 1;
    }

    std::vector<StageReflectionResult> results;
    ReflectionOptions opts{};
    std::vector<std::pair<std::string, std::vector<uint32_t>>> stageBytes;
    auto reflectOne = [&](const std::string& path, ShaderStageKind sk, const char* key){
        if (path.empty()) return;
        std::vector<uint32_t> words; if (!ReadFileWords(path, words)) { std::cerr << "Failed to read SPIR-V: " << path << "\n"; std::exit(3); }
        StageReflectionResult r{}; std::string err;
        if (!ReflectSpirv(sk, words.data(), words.size(), opts, r, &err)) { std::cerr << "ReflectSpirv failed: " << err << "\n"; std::exit(4); }
        results.push_back(std::move(r));
        stageBytes.emplace_back(key, std::move(words));
    };

    reflectOne(vs, ShaderStageKind::Vertex, "vs");
    reflectOne(fs, ShaderStageKind::Fragment, "fs");
    reflectOne(cs, ShaderStageKind::Compute, "cs");
    reflectOne(ms, ShaderStageKind::Mesh, "ms");
    reflectOne(gs, ShaderStageKind::Geometry, "gs");

    ShaderMeta meta = MergeStages(results);

    // Validate and fail-fast on errors
    {
        auto report = ValidateShaderMeta(meta, 128);
        if (report.HasErrors()) {
            for (const auto& issue : report.Issues) {
                if (issue.Severity == IssueSeverity::Error) {
                    std::cerr << "Validation error [" << issue.Code << "]: " << issue.Message << "\n";
                }
            }
            return 8; // validation error code
        }
    }

    // Output selection
    nlohmann::json metaJson = meta;
    if (outPath.empty()) {
        std::cout << metaJson.dump() << std::endl;
    } else {
        auto lower = outPath; for (auto& ch : lower) ch = char(std::tolower((unsigned char)ch));
        if (lower.size() >= 10 && lower.substr(lower.size()-10) == ".shaderpkg") {
            if (!WriteShaderPkg(outPath, meta, stageBytes)) { std::cerr << "Failed to write .shaderpkg\n"; return 5; }
        } else {
            std::ofstream out(outPath, std::ios::binary); if (!out) { std::cerr << "Failed to write output file\n"; return 5; }
            out << metaJson.dump();
        }
    }
    return 0;
}

