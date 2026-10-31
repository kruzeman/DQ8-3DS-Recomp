/* Minimal shadow of azahar's common/assert.h for the DQ8 sampler
 * differential test build (the real one pulls the logging stack, which needs
 * the absent vendored fmt submodule).  Release-mode semantics are preserved:
 * DEBUG_ASSERT is compiled out, ASSERT aborts on failure, UNIMPLEMENTED
 * aborts (the test matrix never exercises unknown-format paths, so hitting it
 * means the test itself is wrong). */
#pragma once
#include <cstdlib>
#define ASSERT(_a_) do { if (!(_a_)) std::abort(); } while (0)
#define ASSERT_MSG(_a_, ...) do { if (!(_a_)) std::abort(); } while (0)
#define UNREACHABLE() std::abort()
#define UNREACHABLE_MSG(...) std::abort()
#define UNIMPLEMENTED() std::abort()
#define DEBUG_ASSERT(_a_) do {} while (0)
#define DEBUG_ASSERT_MSG(_a_, ...) do {} while (0)
