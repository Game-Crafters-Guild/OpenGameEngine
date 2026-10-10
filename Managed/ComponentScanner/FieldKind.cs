namespace GameEngine.ComponentScanner;

// The scanner's mirror of ECS::FieldTypeId (Engine/Modules/ECS/Include/ECS/Reflection.h): the
// kind a reflected field registers as, resolved from the field's C++ type text. Same names and
// values as the C++ enum; the web emitters write these names into the generated JSON so it
// compares field by field with the live registry.
internal enum FieldKind : ushort
{
    Unknown = 0,
    Bool = 1,
    Int8 = 2,
    Int16 = 3,
    Int32 = 4,
    Int64 = 5,
    UInt8 = 6,
    UInt16 = 7,
    UInt32 = 8,
    UInt64 = 9,
    Float = 10,
    Double = 11,
    Vec2 = 12,
    Vec3 = 13,
    Vec4 = 14,
    Quat = 15,
    Mat4 = 16,
    Color = 17,
    AssetGuid = 18,
    EntityHandle = 19,
    String = 20,
    Bytes = 21,
}
