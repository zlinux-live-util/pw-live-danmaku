#pragma once
// Synthetic faces for the offline --demo run.
//
// The demo has no network, so every row used to arrive with no picture at all. That is the one
// thing a layout check cannot see through: a face drawn into the wrong box, at the wrong place, or
// clipped by the wrong circle looks exactly like no face at all, so the demo could not tell a
// correct avatar box from a broken one. These faces are solid red-and-green grids instead -- flat
// colours, hard edges, nothing for antialiasing to soften a measurement -- and each has a different
// cell count, so two neighbouring rows can be told apart at a glance and a stretch or a crop of the
// box shows up immediately.
//
// They are drawn rather than shipped as files on purpose. A checked-in PNG would need a decoder
// plumbed into the demo path and could drift from what the code expects, while these are four
// cairo fills that are correct by construction and identical on every machine.

#include <string>
#include <vector>

#include "cairo_util.hpp"

namespace dwm {

/** How many distinct faces demoFaces() makes. demoFaceKey() cycles through them, so a long demo
 *  list still shows a different pattern on consecutive rows. */
inline constexpr int kDemoFaceCount = 4;

/** The avatarUrl a demo message carries in order to be given face i. Panel looks the key up in the
 *  table setDemoFaces() installs, so the naming convention lives here rather than being spelled out
 *  on both sides. */
std::string demoFaceKey(int i);

/** The faces, kDemoFaceCount of them, in the same order as demoFaceKey(). */
std::vector<pwvideo::SurfacePtr> demoFaces(int px = 256);

}  // namespace dwm