/* Minimal stub of boost/serialization/access.hpp for the DQ8 sampler
 * differential test build.  azahar's src/common/vector_math.h only needs
 * `friend class boost::serialization::access;` to be declarable; the real
 * boost submodule is not vendored in this checkout and the full boost is not
 * required to compile texture_decode.cpp / etc1.cpp as a reference decoder.
 * This stub lives in the DQ8 worktree test scaffolding, NOT in azahar-master. */
#pragma once
namespace boost {
namespace serialization {
class access {};
} // namespace serialization
} // namespace boost
