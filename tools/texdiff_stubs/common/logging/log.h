/* Minimal shadow of azahar's common/logging/log.h for the DQ8 sampler
 * differential test build.  azahar-master's real logging stack needs the
 * vendored fmt submodule (externals/fmt), which is not present in this
 * checkout.  Logging has no role in texture decode semantics, so the test
 * build replaces the LOG_* macros with no-ops.  All decoder logic under test
 * (texture_decode.cpp, etc1.cpp, color.h, vector_math.h, bit_field.h,
 * regs_texturing.h, utils.h) remains the real azahar source. */
#pragma once
#define LOG_TRACE(...) do {} while (0)
#define LOG_DEBUG(...) do {} while (0)
#define LOG_INFO(...) do {} while (0)
#define LOG_WARNING(...) do {} while (0)
#define LOG_ERROR(...) do {} while (0)
#define LOG_CRITICAL(...) do {} while (0)
