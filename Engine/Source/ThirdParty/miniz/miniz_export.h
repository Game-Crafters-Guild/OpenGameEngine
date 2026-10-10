#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H

/* Upstream generates this header from CMake to decorate symbols for shared
 * builds. We compile miniz statically into the Editor, so no decoration. */
#define MINIZ_EXPORT

#endif
