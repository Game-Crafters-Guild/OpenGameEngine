#include "Rendering/ShaderCache/ShaderMetaBinary.h"

#include "Types/Fnv1a.h"

#include <algorithm>
#include <cstring>

namespace GameEngine { namespace Rendering {

namespace {

// TypeDesc and Member nest through each other. A struct member can be a struct,
// so a corrupt package could otherwise describe arbitrarily deep nesting and
// recurse the decoder off the stack. Real reflection nests a handful deep.
constexpr uint32_t kMaxTypeDepth = 32;

// One past the last enumerator of each enum the encoding stores. A stored value
// at or past it is corruption, not a newer enumerator: a new enumerator is a
// package-version bump like any other change to the encoding.
constexpr uint32_t kTypeKindEnd = static_cast<uint32_t>(TypeKind::Struct) + 1;
constexpr uint32_t kBaseTypeEnd = static_cast<uint32_t>(BaseType::Bool) + 1;
constexpr uint32_t kPropertyTypeEnd = static_cast<uint32_t>(ShaderPropertyType::Enum) + 1;
constexpr uint32_t kPropertyOriginEnd = static_cast<uint32_t>(ShaderPropertyOrigin::VertexModifier) + 1;

// Payload header: magic, then an FNV-1a of everything after it. Both are needed
// -- the magic rejects a chunk that is not this encoding at all, the checksum
// rejects one that is, but has been damaged.
constexpr char kMetaMagic[4] = {'G', 'E', 'S', 'M'};
constexpr size_t kMetaHeaderSize = sizeof(kMetaMagic) + sizeof(uint64_t);

class MetaWriter
{
public:
    void U8(uint8_t v) { m_Bytes.push_back(v); }
    void Bool(bool v) { U8(v ? 1u : 0u); }

    void U32(uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            m_Bytes.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }

    void U64(uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            m_Bytes.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }

    void F32(float v)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        U32(bits);
    }

    void Str(const std::string& s)
    {
        U32(static_cast<uint32_t>(s.size()));
        m_Bytes.insert(m_Bytes.end(), s.begin(), s.end());
    }

    std::vector<uint8_t> Take() { return std::move(m_Bytes); }

    // Only the first failure is kept.
    void Fail(const char* reason)
    {
        if (m_FailureReason == nullptr)
            m_FailureReason = reason;
    }
    [[nodiscard]] const char* FailureReason() const { return m_FailureReason; }

private:
    std::vector<uint8_t> m_Bytes;
    const char* m_FailureReason = nullptr;
};

class MetaReader
{
public:
    MetaReader(const uint8_t* data, size_t size) : m_Data(data), m_Size(size) {}

    uint8_t U8()
    {
        if (m_Failed || m_Off + 1 > m_Size)
            return Fail(kTruncated), uint8_t{0};
        return m_Data[m_Off++];
    }

    bool Bool() { return U8() != 0; }

    uint32_t U32()
    {
        if (m_Failed || m_Off + 4 > m_Size)
            return Fail(kTruncated), uint32_t{0};
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(m_Data[m_Off + i]) << (i * 8);
        m_Off += 4;
        return v;
    }

    uint64_t U64()
    {
        if (m_Failed || m_Off + 8 > m_Size)
            return Fail(kTruncated), uint64_t{0};
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(m_Data[m_Off + i]) << (i * 8);
        m_Off += 8;
        return v;
    }

    float F32()
    {
        const uint32_t bits = U32();
        float v = 0.0f;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }

    std::string Str()
    {
        const uint32_t len = U32();
        if (m_Failed || len > Remaining())
            return Fail("a string is longer than the bytes left"), std::string{};
        std::string s(reinterpret_cast<const char*>(m_Data + m_Off), len);
        m_Off += len;
        return s;
    }

    // An enum stored as u32, refused when it names no enumerator.
    template <typename E>
    E Enum(uint32_t end)
    {
        const uint32_t value = U32();
        if (!m_Failed && value >= end)
            Fail("an enum value is outside its type");
        return static_cast<E>(value);
    }

    // An element costs at least one byte, so a count larger than what is left is
    // corrupt. Checking before reserving is what stops a bogus length turning
    // into a huge allocation.
    uint32_t Count()
    {
        const uint32_t n = U32();
        if (m_Failed || n > Remaining())
            return Fail("a count is larger than the bytes left"), uint32_t{0};
        return n;
    }

    // Only the first failure is kept: everything after it reads zeros.
    void Fail(const char* reason)
    {
        if (!m_Failed)
            m_FailureReason = reason;
        m_Failed = true;
    }

    [[nodiscard]] bool Failed() const { return m_Failed; }
    [[nodiscard]] const char* FailureReason() const { return m_FailureReason; }
    [[nodiscard]] size_t Remaining() const { return m_Off <= m_Size ? m_Size - m_Off : 0; }

private:
    static constexpr const char* kTruncated = "it ends before its last field";

    const uint8_t* m_Data = nullptr;
    size_t m_Size = 0;
    size_t m_Off = 0;
    bool m_Failed = false;
    const char* m_FailureReason = "";
};

// An optional is a presence byte then, if present, the value.
template <typename T, typename WriteFn>
void WriteOpt(MetaWriter& w, const std::optional<T>& opt, WriteFn write)
{
    w.Bool(opt.has_value());
    if (opt.has_value())
        write(*opt);
}

template <typename T, typename ReadFn>
void ReadOpt(MetaReader& r, std::optional<T>& opt, ReadFn read)
{
    opt.reset();
    if (!r.Bool() || r.Failed())
        return;
    T value{};
    read(value);
    if (!r.Failed())
        opt = std::move(value);
}

void WriteTypeDesc(MetaWriter& w, const TypeDesc& t, uint32_t depth);
bool ReadTypeDesc(MetaReader& r, TypeDesc& t, uint32_t depth);

void WriteMember(MetaWriter& w, const Member& m, uint32_t depth)
{
    w.Str(m.Name);
    WriteTypeDesc(w, m.Type, depth);
    w.U32(m.Offset);
    w.U32(m.Size);
    WriteOpt(w, m.ArrayStride, [&](uint32_t v) { w.U32(v); });
    WriteOpt(w, m.MatrixStride, [&](uint32_t v) { w.U32(v); });
    w.Bool(m.RowMajor);
}

bool ReadMember(MetaReader& r, Member& m, uint32_t depth)
{
    m.Name = r.Str();
    if (!ReadTypeDesc(r, m.Type, depth))
        return false;
    m.Offset = r.U32();
    m.Size = r.U32();
    ReadOpt(r, m.ArrayStride, [&](uint32_t& v) { v = r.U32(); });
    ReadOpt(r, m.MatrixStride, [&](uint32_t& v) { v = r.U32(); });
    m.RowMajor = r.Bool();
    return !r.Failed();
}

void WriteTypeDesc(MetaWriter& w, const TypeDesc& t, uint32_t depth)
{
    if (depth > kMaxTypeDepth)
    {
        w.Fail("a type nests deeper than the decoder allows");
        return;
    }
    w.U32(static_cast<uint32_t>(t.Kind));
    w.U32(static_cast<uint32_t>(t.Base));
    w.U32(t.Rows);
    w.U32(t.Cols);
    w.U32(t.VecSize);
    w.U32(static_cast<uint32_t>(t.ArrayDims.size()));
    for (uint32_t dim : t.ArrayDims)
        w.U32(dim);
    w.U32(static_cast<uint32_t>(t.StructMembers.size()));
    for (const Member& m : t.StructMembers)
        WriteMember(w, m, depth + 1);
}

bool ReadTypeDesc(MetaReader& r, TypeDesc& t, uint32_t depth)
{
    if (depth > kMaxTypeDepth)
    {
        r.Fail("a type nests deeper than the decoder allows");
        return false;
    }

    t.Kind = r.Enum<TypeKind>(kTypeKindEnd);
    t.Base = r.Enum<BaseType>(kBaseTypeEnd);
    t.Rows = r.U32();
    t.Cols = r.U32();
    t.VecSize = r.U32();

    const uint32_t dimCount = r.Count();
    t.ArrayDims.clear();
    t.ArrayDims.reserve(dimCount);
    for (uint32_t i = 0; i < dimCount && !r.Failed(); ++i)
        t.ArrayDims.push_back(r.U32());

    const uint32_t memberCount = r.Count();
    t.StructMembers.clear();
    t.StructMembers.reserve(memberCount);
    for (uint32_t i = 0; i < memberCount && !r.Failed(); ++i)
    {
        Member m{};
        if (!ReadMember(r, m, depth + 1))
            return false;
        t.StructMembers.push_back(std::move(m));
    }
    return !r.Failed();
}

void WriteBlockLayout(MetaWriter& w, const BlockLayout& b)
{
    w.U32(b.Size);
    w.U32(static_cast<uint32_t>(b.Members.size()));
    for (const Member& m : b.Members)
        WriteMember(w, m, 0);
}

bool ReadBlockLayout(MetaReader& r, BlockLayout& b)
{
    b.Size = r.U32();
    const uint32_t count = r.Count();
    b.Members.clear();
    b.Members.reserve(count);
    for (uint32_t i = 0; i < count && !r.Failed(); ++i)
    {
        Member m{};
        if (!ReadMember(r, m, 0))
            return false;
        b.Members.push_back(std::move(m));
    }
    return !r.Failed();
}

void WriteImageInfo(MetaWriter& w, const DescriptorBindingMeta::ImageInfo& i)
{
    w.U32(i.Dim);
    w.Bool(i.Arrayed);
    w.Bool(i.Multisample);
    w.Bool(i.Cube);
    w.Bool(i.Depth);
    w.Bool(i.Filtered);
    w.U32(i.Format);
    w.Bool(i.UnsignedInteger);
}

void ReadImageInfo(MetaReader& r, DescriptorBindingMeta::ImageInfo& i)
{
    i.Dim = r.U32();
    i.Arrayed = r.Bool();
    i.Multisample = r.Bool();
    i.Cube = r.Bool();
    i.Depth = r.Bool();
    i.Filtered = r.Bool();
    i.Format = r.U32();
    i.UnsignedInteger = r.Bool();
}

void WriteStageIO(MetaWriter& w, const StageIO& io)
{
    w.U32(io.Location);
    w.U32(io.Component);
    w.Str(io.Name);
    WriteTypeDesc(w, io.Type, 0);
    WriteOpt(w, io.Builtin, [&](const std::string& s) { w.Str(s); });
}

bool ReadStageIO(MetaReader& r, StageIO& io)
{
    io.Location = r.U32();
    io.Component = r.U32();
    io.Name = r.Str();
    if (!ReadTypeDesc(r, io.Type, 0))
        return false;
    ReadOpt(r, io.Builtin, [&](std::string& s) { s = r.Str(); });
    return !r.Failed();
}

void WriteLocalSize(MetaWriter& w, const StageMeta::LocalSize& s)
{
    w.U32(s.X);
    w.U32(s.Y);
    w.U32(s.Z);
}

void ReadLocalSize(MetaReader& r, StageMeta::LocalSize& s)
{
    s.X = r.U32();
    s.Y = r.U32();
    s.Z = r.U32();
}

void WriteShaderProperty(MetaWriter& w, const ShaderProperty& p)
{
    w.Str(p.Name);
    w.Str(p.DisplayName);
    w.U32(static_cast<uint32_t>(p.Type));
    w.U32(static_cast<uint32_t>(p.Origin));
    for (float c : p.Default)
        w.F32(c);
    w.Bool(p.HasRange);
    w.F32(p.RangeMin);
    w.F32(p.RangeMax);
    w.Str(p.Group);
    w.Str(p.VisibleIf);
    w.Str(p.Tooltip);
    w.U32(static_cast<uint32_t>(p.EnumValues.size()));
    for (const std::string& e : p.EnumValues)
        w.Str(e);
    w.Bool(p.Hdr);
    w.Bool(p.Hidden);
    w.Bool(p.HasAlpha);
    w.Bool(p.HasLane);
    w.U32(p.Lane);
    w.U32(p.Component);
    w.U32(p.ByteOffset);
    w.U32(p.ByteSize);
    w.Str(p.SourceFile);
    w.U32(p.SourceLine);
}

void ReadShaderProperty(MetaReader& r, ShaderProperty& p)
{
    p.Name = r.Str();
    p.DisplayName = r.Str();
    p.Type = r.Enum<ShaderPropertyType>(kPropertyTypeEnd);
    p.Origin = r.Enum<ShaderPropertyOrigin>(kPropertyOriginEnd);
    for (float& c : p.Default)
        c = r.F32();
    p.HasRange = r.Bool();
    p.RangeMin = r.F32();
    p.RangeMax = r.F32();
    p.Group = r.Str();
    p.VisibleIf = r.Str();
    p.Tooltip = r.Str();
    const uint32_t enumCount = r.Count();
    p.EnumValues.clear();
    p.EnumValues.reserve(enumCount);
    for (uint32_t i = 0; i < enumCount && !r.Failed(); ++i)
        p.EnumValues.push_back(r.Str());
    p.Hdr = r.Bool();
    p.Hidden = r.Bool();
    p.HasAlpha = r.Bool();
    p.HasLane = r.Bool();
    p.Lane = r.U32();
    p.Component = r.U32();
    p.ByteOffset = r.U32();
    p.ByteSize = r.U32();
    p.SourceFile = r.Str();
    p.SourceLine = r.U32();
}

// A map's entries ordered by key. The meta keeps stages in unordered maps,
// whose iteration order depends on the standard library and on insertion
// history; encoding in that order would give the same meta different bytes.
template <typename Map>
std::vector<const typename Map::value_type*> SortedByKey(const Map& map)
{
    std::vector<const typename Map::value_type*> entries;
    entries.reserve(map.size());
    for (const auto& entry : map)
        entries.push_back(&entry);
    std::sort(entries.begin(), entries.end(),
              [](const auto* a, const auto* b) { return a->first < b->first; });
    return entries;
}

} // namespace

bool EncodeShaderMetaBinary(const ShaderMeta& meta, std::vector<uint8_t>& out, std::string* outError)
{
    MetaWriter w;
    w.U32(meta.Version);

    w.U32(static_cast<uint32_t>(meta.EntryPoints.size()));
    for (const auto* entry : SortedByKey(meta.EntryPoints))
    {
        w.Str(entry->first);
        w.Str(entry->second);
    }

    w.U32(static_cast<uint32_t>(meta.PushConstants.size()));
    for (const PushConstantRangeMeta& pc : meta.PushConstants)
    {
        w.U32(pc.Id);
        w.Str(pc.Name);
        w.U32(pc.Size);
        w.U32(pc.StagesMask);
        WriteBlockLayout(w, pc.Block);
    }

    w.U32(static_cast<uint32_t>(meta.Sets.size()));
    for (const DescriptorSetMeta& set : meta.Sets)
    {
        w.U32(set.Set);
        w.U32(static_cast<uint32_t>(set.Bindings.size()));
        for (const DescriptorBindingMeta& b : set.Bindings)
        {
            // NameId and BuiltLayoutHash are derived from what follows and are
            // recomputed on read, so they are not stored.
            w.U32(b.Binding);
            w.Str(b.Name);
            w.U32(b.Type);
            w.U32(b.Count);
            w.U32(b.StagesMask);
            WriteOpt(w, b.Block, [&](const BlockLayout& bl) { WriteBlockLayout(w, bl); });
            w.Bool(b.ReadOnly);
            WriteOpt(w, b.Image,
                     [&](const DescriptorBindingMeta::ImageInfo& i) { WriteImageInfo(w, i); });
        }
    }

    w.U32(static_cast<uint32_t>(meta.Stages.size()));
    for (const auto* entry : SortedByKey(meta.Stages))
    {
        const StageMeta& sm = entry->second;
        w.Str(entry->first);
        w.U32(static_cast<uint32_t>(sm.Inputs.size()));
        for (const StageIO& io : sm.Inputs)
            WriteStageIO(w, io);
        w.U32(static_cast<uint32_t>(sm.Outputs.size()));
        for (const StageIO& io : sm.Outputs)
            WriteStageIO(w, io);
        WriteOpt(w, sm.ComputeLocalSize,
                 [&](const StageMeta::LocalSize& s) { WriteLocalSize(w, s); });
        WriteOpt(w, sm.MeshLocalSize,
                 [&](const StageMeta::LocalSize& s) { WriteLocalSize(w, s); });
        w.Str(sm.EntryPoint);
    }

    w.U32(static_cast<uint32_t>(meta.SpecConstants.size()));
    for (const SpecConstantMeta& sc : meta.SpecConstants)
    {
        w.U32(sc.Id);
        w.Str(sc.Name);
        WriteTypeDesc(w, sc.Type, 0);
        WriteOpt(w, sc.DefaultValue, [&](const std::string& s) { w.Str(s); });
    }

    w.U32(static_cast<uint32_t>(meta.Requirements.size()));
    for (const std::string& s : meta.Requirements)
        w.Str(s);

    w.U32(static_cast<uint32_t>(meta.RequiredDefines.size()));
    for (const std::string& s : meta.RequiredDefines)
        w.Str(s);

    w.U32(static_cast<uint32_t>(meta.DeclaredProperties.size()));
    for (const ShaderProperty& p : meta.DeclaredProperties)
        WriteShaderProperty(w, p);

    if (w.FailureReason() != nullptr)
    {
        if (outError)
            *outError = std::string("meta cannot be encoded: ") + w.FailureReason();
        return false;
    }

    const std::vector<uint8_t> payload = w.Take();
    out.clear();
    out.reserve(kMetaHeaderSize + payload.size());
    out.insert(out.end(), std::begin(kMetaMagic), std::end(kMetaMagic));
    const uint64_t checksum = Hashing::Fnv1a64(payload.data(), payload.size());
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>((checksum >> (i * 8)) & 0xFF));
    out.insert(out.end(), payload.begin(), payload.end());
    return true;
}

bool DecodeShaderMetaBinary(const uint8_t* data, size_t size, ShaderMeta& out,
                            std::string* outError)
{
    const auto fail = [&](const char* why) {
        if (outError)
            *outError = why;
        return false;
    };

    if (data == nullptr || size < kMetaHeaderSize)
        return fail("meta-bin chunk is empty or too small");
    if (std::memcmp(data, kMetaMagic, sizeof(kMetaMagic)) != 0)
        return fail("meta-bin chunk has the wrong magic");

    uint64_t stored = 0;
    for (int i = 0; i < 8; ++i)
        stored |= static_cast<uint64_t>(data[sizeof(kMetaMagic) + i]) << (i * 8);

    const uint8_t* payload = data + kMetaHeaderSize;
    const size_t payloadSize = size - kMetaHeaderSize;
    if (Hashing::Fnv1a64(payload, payloadSize) != stored)
        return fail("meta-bin chunk failed its checksum");

    MetaReader r(payload, payloadSize);
    const auto failCorrupt = [&]() {
        if (outError)
            *outError = std::string("meta-bin chunk is corrupt: ") + r.FailureReason();
        return false;
    };
    out = ShaderMeta{};
    out.Version = r.U32();

    const uint32_t entryPointCount = r.Count();
    for (uint32_t i = 0; i < entryPointCount && !r.Failed(); ++i)
    {
        std::string stage = r.Str();
        out.EntryPoints[std::move(stage)] = r.Str();
    }

    const uint32_t pushConstantCount = r.Count();
    out.PushConstants.reserve(pushConstantCount);
    for (uint32_t i = 0; i < pushConstantCount && !r.Failed(); ++i)
    {
        PushConstantRangeMeta pc{};
        pc.Id = r.U32();
        pc.Name = r.Str();
        pc.Size = r.U32();
        pc.StagesMask = r.U32();
        if (!ReadBlockLayout(r, pc.Block))
            return failCorrupt();
        out.PushConstants.push_back(std::move(pc));
    }

    const uint32_t setCount = r.Count();
    out.Sets.reserve(setCount);
    for (uint32_t i = 0; i < setCount && !r.Failed(); ++i)
    {
        DescriptorSetMeta set{};
        set.Set = r.U32();
        const uint32_t bindingCount = r.Count();
        set.Bindings.reserve(bindingCount);
        for (uint32_t b = 0; b < bindingCount && !r.Failed(); ++b)
        {
            DescriptorBindingMeta binding{};
            binding.Binding = r.U32();
            binding.Name = r.Str();
            binding.Type = r.U32();
            binding.Count = r.U32();
            binding.StagesMask = r.U32();
            bool blockOk = true;
            ReadOpt(r, binding.Block,
                    [&](BlockLayout& bl) { blockOk = ReadBlockLayout(r, bl); });
            if (!blockOk)
                return failCorrupt();
            binding.ReadOnly = r.Bool();
            ReadOpt(r, binding.Image,
                    [&](DescriptorBindingMeta::ImageInfo& info) { ReadImageInfo(r, info); });
            set.Bindings.push_back(std::move(binding));
        }
        // Name ids and the layout hash are derived from the bindings, so they
        // cannot disagree with them.
        FinalizeSetLayout(set);
        out.Sets.push_back(std::move(set));
    }

    const uint32_t stageCount = r.Count();
    for (uint32_t i = 0; i < stageCount && !r.Failed(); ++i)
    {
        std::string stage = r.Str();
        StageMeta sm{};
        const uint32_t inputCount = r.Count();
        sm.Inputs.reserve(inputCount);
        for (uint32_t k = 0; k < inputCount && !r.Failed(); ++k)
        {
            StageIO io{};
            if (!ReadStageIO(r, io))
                return failCorrupt();
            sm.Inputs.push_back(std::move(io));
        }
        const uint32_t outputCount = r.Count();
        sm.Outputs.reserve(outputCount);
        for (uint32_t k = 0; k < outputCount && !r.Failed(); ++k)
        {
            StageIO io{};
            if (!ReadStageIO(r, io))
                return failCorrupt();
            sm.Outputs.push_back(std::move(io));
        }
        ReadOpt(r, sm.ComputeLocalSize,
                [&](StageMeta::LocalSize& s) { ReadLocalSize(r, s); });
        ReadOpt(r, sm.MeshLocalSize, [&](StageMeta::LocalSize& s) { ReadLocalSize(r, s); });
        sm.EntryPoint = r.Str();
        out.Stages[std::move(stage)] = std::move(sm);
    }

    const uint32_t specCount = r.Count();
    out.SpecConstants.reserve(specCount);
    for (uint32_t i = 0; i < specCount && !r.Failed(); ++i)
    {
        SpecConstantMeta sc{};
        sc.Id = r.U32();
        sc.Name = r.Str();
        if (!ReadTypeDesc(r, sc.Type, 0))
            return failCorrupt();
        ReadOpt(r, sc.DefaultValue, [&](std::string& s) { s = r.Str(); });
        out.SpecConstants.push_back(std::move(sc));
    }

    const uint32_t requirementCount = r.Count();
    out.Requirements.reserve(requirementCount);
    for (uint32_t i = 0; i < requirementCount && !r.Failed(); ++i)
        out.Requirements.push_back(r.Str());

    const uint32_t defineCount = r.Count();
    out.RequiredDefines.reserve(defineCount);
    for (uint32_t i = 0; i < defineCount && !r.Failed(); ++i)
        out.RequiredDefines.push_back(r.Str());

    const uint32_t propertyCount = r.Count();
    out.DeclaredProperties.reserve(propertyCount);
    for (uint32_t i = 0; i < propertyCount && !r.Failed(); ++i)
    {
        ShaderProperty p{};
        ReadShaderProperty(r, p);
        out.DeclaredProperties.push_back(std::move(p));
    }

    if (!r.Failed() && r.Remaining() != 0)
        r.Fail("bytes follow the last field");
    if (r.Failed())
        return failCorrupt();
    return true;
}

}} // namespace GameEngine::Rendering
