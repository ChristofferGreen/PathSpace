#include "ext/doctest.h"

TEST_CASE("Renderer integration replay retains framebuffer parity" * doctest::skip(true)) {
    /* Placeholder: populate once GPU/HTML integration replay harness exists. */
}

#ifdef PATHSPACE_UI_METAL
TEST_CASE("PathSurfaceMetal integrates with ObjC++ presenter harness" * doctest::skip(true)) {
    /* Placeholder: enable once PathSurfaceMetal implementation lands. */
}
#else
TEST_CASE("PathSurfaceMetal integration harness pending" * doctest::skip(true)) {
    /* Placeholder: PATHSPACE_UI_METAL disabled. */
}
#endif

TEST_CASE("HtmlAdapter emits DOM/Canvas command parity" * doctest::skip(true)) {
    /* Placeholder: verify HtmlAdapter command parity once implemented. */
}
