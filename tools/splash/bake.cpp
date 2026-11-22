// Bake the Origins splash using its original colour and registered artwork layers.
// Build and usage instructions are in README.md.
#include <png.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

struct Image {
  int w, h;
  std::vector<unsigned char> data;
};

Image read_png(const std::filesystem::path& path, png_uint_32 format) {
  png_image info{};
  info.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_file(&info, path.c_str()))
    throw std::runtime_error(path.string() + ": " + info.message);
  info.format = format;
  Image out{int(info.width), int(info.height), std::vector<unsigned char>(PNG_IMAGE_SIZE(info))};
  if (!png_image_finish_read(&info, nullptr, out.data.data(), 0, nullptr))
    throw std::runtime_error(path.string() + ": " + info.message);
  return out;
}

void write_rgb(const std::filesystem::path& path, const Image& image) {
  png_image info{};
  info.version = PNG_IMAGE_VERSION;
  info.width = image.w;
  info.height = image.h;
  info.format = PNG_FORMAT_RGB;
  if (!png_image_write_to_file(&info, path.c_str(), 0, image.data.data(), 0, nullptr))
    throw std::runtime_error(path.string() + ": " + info.message);
}

struct Plane {
  Image matte;
  std::vector<float> foreground;  // Premultiplied contribution to the original colour.
  float depth;
};

struct Scene {
  Image base, depth;
  std::vector<Plane> planes;
};

void separate_plane(Scene& scene, const Image& clean, Image matte, float depth, int radius) {
  auto& source = scene.base;
  Plane plane{std::move(matte), std::vector<float>(source.data.size()), depth};
  // The matched hero-free plate extends into a narrow ring around Bayek. This removes
  // any colour fringe on the water under the bow, while leaving his shadow on the sand.
  const int w = source.w, h = source.h;
  std::vector<int> distance(size_t(w) * h, radius);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const int i = y * w + x;
      if (plane.matte.data[i]) distance[i] = 0;
      if (x) distance[i] = std::min(distance[i], distance[i - 1] + 1);
      if (y) distance[i] = std::min(distance[i], distance[i - w] + 1);
    }
  }
  for (int y = h - 1; y >= 0; --y) {
    for (int x = w - 1; x >= 0; --x) {
      const int i = y * w + x;
      if (x + 1 < w) distance[i] = std::min(distance[i], distance[i + 1] + 1);
      if (y + 1 < h) distance[i] = std::min(distance[i], distance[i + w] + 1);
    }
  }
  for (size_t i = 0; i < plane.matte.data.size(); ++i) {
    const float a = plane.matte.data[i] / 255.f;
    if (distance[i] == radius) continue;
    float plate = 1.f - float(distance[i]) / radius;
    if (i / size_t(w) > size_t(h) * 95 / 100 && !plane.matte.data[i]) plate = 0;
    for (int k = 0; k < 3; ++k) {
      const size_t index = i * 3 + k;
      const float original = source.data[index];
      scene.base.data[index] =
          static_cast<unsigned char>(std::round((1 - plate) * original + plate * clean.data[index]));
      // The signed contribution preserves even a one-pixel shadow or colour fringe.
      plane.foreground[index] = original - scene.base.data[index] * (1 - a);
    }
  }
  scene.planes.push_back(std::move(plane));
}

struct Sample {
  float source = -1, depth = -1;
};

// Render horizontal segments between adjacent source pixels. A break in depth
// separates silhouettes. Where two segments cover one output pixel the near one wins.
void project_row(std::vector<Sample>& row, const unsigned char* depth, float strength) {
  const int w = int(row.size());
  std::fill(row.begin(), row.end(), Sample{});
  for (int x = 0; x < w; ++x) {
    const float z = depth[x];
    const float p = x + strength * (z - 128);
    // At a discontinuity each endpoint still has half a pixel of coverage. Rasterize
    // all endpoints first: a later equal-depth endpoint must not quantize a segment.
    const int nearest = int(std::round(p));
    if (nearest >= 0 && nearest < w && z >= row[nearest].depth) row[nearest] = {float(x), z};
  }
  for (int x = 0; x + 1 < w; ++x) {
    const float z = depth[x];
    const float p = x + strength * (z - 128);
    if (std::abs(depth[x + 1] - depth[x]) > 24) continue;
    const float dz = depth[x + 1] - z, end = p + 1 + strength * dz;
    for (int i = std::max(0, int(std::ceil(p))); i < w && i <= end; ++i) {
      const float t = (i - p) / std::max(1e-5f, end - p);
      const float at_depth = z + t * dz;
      // Prefer the interpolated sample on a continuous surface.
      if (at_depth + 1 >= row[i].depth) row[i] = {x + t, at_depth};
    }
  }

  // At a newly revealed edge, extend the texture on the background side
  // into the narrow gap. Sample behind the edge to avoid streaking foreground colour.
  for (int i = 0; i < w;) {
    if (row[i].source >= 0) {
      ++i;
      continue;
    }
    const int begin = i;
    while (i < w && row[i].source < 0) ++i;
    if (begin == 0 && i == w) {
      for (int j = 0; j < w; ++j) row[j] = {float(j), float(depth[j])};
      break;
    }
    const bool left = i == w || (begin && row[begin - 1].depth < row[i].depth);
    const int edge = left ? begin - 1 : i;
    for (int j = begin; j < i; ++j) {
      int reflected = std::clamp(edge + (edge - j), 0, w - 1);
      if (row[reflected].source < 0 || std::abs(row[reflected].depth - row[edge].depth) > 18)
        reflected = edge;
      row[j] = row[reflected];
    }
  }
}

Image view(const Scene& scene, float position) {
  const int w = scene.base.w, h = scene.base.h;
  Image out{w, h, std::vector<unsigned char>(size_t(w) * h * 3)};
  // For a 960px eye the hero-to-pyramid difference between views is ~17px.
  const float strength = position * w * 0.011f / 255.f;
  std::vector<Sample> row(w);
  for (int y = 0; y < h; ++y) {
    const size_t start = size_t(y) * w;
    project_row(row, scene.depth.data.data() + start, strength);
    for (int x = 0; x < w; ++x) {
      float sx = std::clamp(row[x].source, 0.f, float(w - 1));
      int ix = int(sx), next = std::min(w - 1, ix + 1);
      float t = sx - ix;
      std::array<float, 3> rgb;
      for (int k = 0; k < 3; ++k)
        rgb[k] =
            (1 - t) * scene.base.data[(start + ix) * 3 + k] + t * scene.base.data[(start + next) * 3 + k];
      for (const auto& plane : scene.planes) {
        const float px = std::clamp(x - strength * (plane.depth - 128.f), 0.f, float(w - 1));
        const int x0 = int(px), x1 = std::min(w - 1, x0 + 1);
        const float s = px - x0;
        const float a = ((1 - s) * plane.matte.data[start + x0] + s * plane.matte.data[start + x1]) / 255.f;
        for (int k = 0; k < 3; ++k) {
          const float f =
              (1 - s) * plane.foreground[(start + x0) * 3 + k] + s * plane.foreground[(start + x1) * 3 + k];
          rgb[k] = f + (1 - a) * rgb[k];
        }
      }
      for (int k = 0; k < 3; ++k)
        out.data[(start + x) * 3 + k] =
            static_cast<unsigned char>(std::round(std::clamp(rgb[k], 0.f, 255.f)));
    }
  }
  return out;
}

int main(int argc, char** argv) {
  if (argc != 4 && argc != 5) {
    std::fprintf(stderr, "usage: %s original.png layers-directory stereo.png [view-dir]\n", argv[0]);
    return 1;
  }
  try {
    const std::filesystem::path art = argv[2];
    auto source = read_png(argv[1], PNG_FORMAT_RGB);
    auto clean = read_png(art / "clean-plates.png", PNG_FORMAT_RGB);
    auto hero = read_png(art / "bayek-matte.png", PNG_FORMAT_GRAY);
    auto title = read_png(art / "title-matte.png", PNG_FORMAT_GRAY);
    auto depth = read_png(art / "background-depth.png", PNG_FORMAT_GRAY);
    for (const auto* input : {&clean, &hero, &title, &depth})
      if (input->w != source.w || input->h != source.h) throw std::runtime_error("input dimensions differ");
    Scene scene{source, std::move(depth), {}};
    separate_plane(scene, clean, std::move(hero), 237, 8);
    separate_plane(scene, clean, std::move(title), 128, 1);
    auto left = view(scene, 1), right = view(scene, -1);
    Image pair{source.w * 2, source.h, std::vector<unsigned char>(source.data.size() * 2)};
    const int stride = source.w * 3;
    for (int y = 0; y < source.h; ++y) {
      std::copy_n(left.data.data() + y * stride, stride, pair.data.data() + 2 * y * stride);
      std::copy_n(right.data.data() + y * stride, stride, pair.data.data() + (2 * y + 1) * stride);
    }
    write_rgb(argv[3], pair);
    if (argc == 5) {
      std::filesystem::create_directories(argv[4]);
      constexpr int frames = 40;
      for (int i = 0; i < frames; ++i) {
        const float position = std::sin(i * (2 * 3.14159265f / frames));
        char frame[24];
        std::snprintf(frame, sizeof(frame), "%02d.png", i);
        write_rgb(std::filesystem::path(argv[4]) / frame, view(scene, position));
      }
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
