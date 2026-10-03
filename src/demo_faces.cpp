#include "demo_faces.hpp"

namespace dwm {

std::string demoFaceKey(int i) {
  return "demo-face-" + std::to_string(((i % kDemoFaceCount) + kDemoFaceCount) % kDemoFaceCount);
}

std::vector<pwvideo::SurfacePtr> demoFaces(int px) {
  // Pure red and pure green, so a blend at an edge can only come from resampling and not from the
  // colours themselves: anything between them on screen is the renderer mixing, which is what makes
  // a half-pixel offset legible.
  constexpr double kRed[3] = {0.90, 0.10, 0.10};
  constexpr double kGreen[3] = {0.10, 0.80, 0.20};

  std::vector<pwvideo::SurfacePtr> out;
  out.reserve(kDemoFaceCount);
  for (int face = 0; face < kDemoFaceCount; ++face) {
    const int cells = face + 1;  // one face per cell count, 1 through 4
    // The deleter is supplied explicitly, as Panel::resize does: cairo_surface_t is an opaque
    // type, so a shared_ptr that ever default-deleted one would need its size.
    pwvideo::SurfacePtr surf(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px),
                             pwvideo::CairoSurfaceDeleter{});
    pwvideo::ContextPtr cr(cairo_create(surf.get()));
    cairo_set_antialias(cr.get(), CAIRO_ANTIALIAS_NONE);
    const double cell = static_cast<double>(px) / static_cast<double>(cells);
    for (int y = 0; y < cells; ++y) {
      for (int x = 0; x < cells; ++x) {
        const double* rgb = ((x + y) % 2 == 0) ? kRed : kGreen;
        cairo_set_source_rgb(cr.get(), rgb[0], rgb[1], rgb[2]);
        // Overdrawn by a fraction of a pixel on the right and bottom: abutting rectangles with
        // antialiasing off still leave hairline seams from the rasteriser's rounding, and a seam
        // across the face would be read as a rendering fault.
        cairo_rectangle(cr.get(), x * cell, y * cell, cell + 0.5, cell + 0.5);
        cairo_fill(cr.get());
      }
    }
    cairo_surface_flush(surf.get());
    out.push_back(std::move(surf));
  }
  return out;
}

}  // namespace dwm