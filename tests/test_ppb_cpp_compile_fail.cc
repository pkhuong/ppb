// Tests for compile-time diagnostics / rejections.
//
// Each block uses the format:
//   /* expect-error: <regex matched against compiler stderr> */
//   #ifdef PPB_FAIL_<NAME>
//   <code that should fail to compile>
//   #endif

#include <ppb/ppb.hpp>

/* expect-error: this is a placeholder negative test */
#ifdef PPB_FAIL_PLACEHOLDER
static_assert(false, "this is a placeholder negative test");
#endif
