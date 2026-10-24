// xdesk.native.svg: NanoSVG's C headers (compat.nanosvg), read here and
// nowhere else -- what xdesk.draw renders a scalable icon with. The package
// compiles both implementations; nothing here defines them again.
module;

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <nanosvg.h>
#include <nanosvgrast.h>

export module xdesk.native.svg;

export using ::NSVGimage;
export using ::NSVGrasterizer;
export using ::nsvgParseFromFile;
export using ::nsvgDelete;
export using ::nsvgCreateRasterizer;
export using ::nsvgRasterize;
export using ::nsvgDeleteRasterizer;
