// Lane-name aliases for surfaces that still read the material parameter block
// by lane (Mat.uBaseColor, Mat.uParams0 .. Mat.uParams24) instead of through
// declared properties (Props.<name>). Their .material keys are wired to these
// lanes by MaterialRegistry's hand-typed table. Both go once every such surface
// declares its properties; a declared surface never uses these names.
#define uBaseColor uParams[0]
#define uParams0   uParams[1]
#define uParams1   uParams[2]
#define uParams2   uParams[3]
#define uParams3   uParams[4]
#define uParams4   uParams[5]
#define uParams5   uParams[6]
#define uParams6   uParams[7]
#define uParams7   uParams[8]
#define uParams8   uParams[9]
#define uParams9   uParams[10]
#define uParams10  uParams[11]
#define uParams11  uParams[12]
#define uParams12  uParams[13]
#define uParams13  uParams[14]
#define uParams14  uParams[15]
#define uParams15  uParams[16]
#define uParams16  uParams[17]
#define uParams17  uParams[18]
#define uParams18  uParams[19]
#define uParams19  uParams[20]
#define uParams20  uParams[21]
#define uParams21  uParams[22]
#define uParams22  uParams[23]
#define uParams23  uParams[24]
#define uParams24  uParams[25]

// The generic project-extensibility lanes. A surface that declares its
// properties never touches these; they carry the graph editor's live-preview
// values (Editor MaterialGraphPreviewModel) and any surface still authored
// against the user0..15 / userVec0..3 material keys, and go with the rest of
// this file once the shader graph declares its properties too.
#define uUser0     uParams[26]
#define uUser1     uParams[27]
#define uUser2     uParams[28]
#define uUser3     uParams[29]
