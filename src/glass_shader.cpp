#include "glass_shader.hpp"

#include <algorithm>

#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Shader.hpp>
#include <hyprland/src/render/Texture.hpp>

using namespace Render;
using namespace Render::GL;

namespace {
SP<CShader> g_shader;

constexpr int MODE_GLASS = 0;
constexpr int MODE_FRAME = 1;

constexpr const char *VERTEX_SHADER = R"GLSL(#version 300 es
precision highp float;

uniform mat3 proj;
in vec2 pos;
in vec2 texcoord;
out vec2 v_texcoord;

void main() {
    gl_Position = vec4(proj * vec3(pos, 1.0), 1.0);
    v_texcoord = texcoord;
}
)GLSL";

constexpr const char *FRAGMENT_SHADER = R"GLSL(#version 300 es
precision highp float;

in vec2 v_texcoord;
out vec4 fragColor;

uniform sampler2D tex;       // scene, downscaled but not blurred
uniform sampler2D blurredBG; // same scene, blurred
uniform vec2 topLeft;               // box origin in monitor framebuffer px
uniform vec2 fullSize;              // box size in px
uniform vec2 fullSizeUntransformed; // monitor framebuffer size in px
uniform vec3 tint;
uniform float radius;
uniform float roundingPower;
uniform float alpha;
uniform float blurAlpha;   // tint opacity
uniform float distort;     // bevel steepness, 0..1
uniform float brightness;  // bevel rim light weight
uniform float shadowPower; // bevel falloff exponent
uniform float vibrancy;    // dispersion in px
uniform float vibrancy_darkness; // how sharp the refracting rim reads
uniform float noise;
uniform float sdrSaturation; // how much of the backdrop's colour survives
uniform float range;       // glass: bevel width in px | frame: border opacity
uniform float contrast;    // frame highlight opacity
uniform float thick;       // frame ring width in px
uniform int applyTint;     // 0 = glass underlay, 1 = frame overlay

// Hyprland's rounding.glsl smoothing constant, pi / 5.34665792551. Matching it
// means the plugin's coverage ramp lands on the same subpixels as the client's.
const float SMOOTHING = 0.587367;

const int MODE_FRAME = 1;

// Soda-lime window glass. Fused silica is 1.46, acrylic 1.49; water is 1.33.
// This is held fixed rather than exposed because the lateral shift is dominated
// by tan(theta), not by n: across the whole plausible range of glass indices the
// bend changes by under 2%, so an index slider would look physical and do
// almost nothing. The bevel's steepness is the honest knob, and it is geometry.
const float GLASS_IOR = 1.52;

// Steepest tangent just inside the boundary, at full strength.
const float MAX_TANGENT = 5.0;

// How fast the tilt decays inward, in bevel widths.
const float FALLOFF = 3.0;

// The one shape in this shader: the signed distance to Hyprland's rounded-rect
// Lame silhouette, positive inside, plus its inward normal. Coverage, the frame
// rings and the refraction all read it, so the plugin's outline cannot disagree
// with the client's own clip and the bevel's contours are genuine offsets of
// that outline -- they follow the real corner.
//
// Inside the corner square both components are positive and the Lame norm
// applies; elsewhere the nearest side does. The two agree in value and gradient
// where they meet, because a component reaching zero collapses the norm onto
// that side's distance and its normal onto that side's axis.
float roundedRectField(vec2 pixel, out vec2 inwardNormal) {
    vec2 halfSize = fullSize * 0.5;
    vec2 fromCenter = pixel - halfSize;
    vec2 quadrant = vec2(fromCenter.x < 0.0 ? -1.0 : 1.0,
                         fromCenter.y < 0.0 ? -1.0 : 1.0);
    vec2 q = abs(fromCenter) - (halfSize - radius) + 1.0 / fullSize;

    if (q.x > 0.0 && q.y > 0.0) {
        float power = max(roundingPower, 2.0);
        float dist = pow(pow(q.x, power) + pow(q.y, power), 1.0 / power);
        vec2 gradient = pow(q / max(dist, 0.0001), vec2(power - 1.0));
        inwardNormal = -normalize(gradient) * quadrant;
        return radius - dist;
    }

    vec2 toEdge = halfSize - abs(fromCenter);
    if (toEdge.x < toEdge.y) {
        inwardNormal = vec2(-quadrant.x, 0.0);
        return toEdge.x;
    }
    inwardNormal = vec2(0.0, -quadrant.y);
    return toEdge.y;
}

// Lateral shift, in bevel widths, for a ray entering the bevel at depth d, also
// in bevel widths. Snell through a surface whose normal tilts toward grazing at
// the boundary; the tangents diverge there, which is what folds the scene.
//
// The tilt decays exponentially and is never clamped to zero, so the bevel has
// no outer boundary. Any profile with finite support ends on a curve parallel to
// the silhouette, and easing that end only pushes the discontinuity into a
// higher derivative, which still reads as a Mach band.
float bevelShift(float d) {
    // Drive the tangent rather than the sine: it is the quantity the shift is
    // linear in, so the strength control behaves evenly across its range.
    float tanTheta = clamp(distort, 0.0, 1.0) * MAX_TANGENT *
                     exp(-FALLOFF * max(shadowPower, 0.05) * d);
    float sinTheta = tanTheta / sqrt(1.0 + tanTheta * tanTheta);
    float sinPhi = sinTheta / GLASS_IOR;
    float cosPhi = sqrt(max(1.0 - sinPhi * sinPhi, 0.0001));
    return tanTheta - sinPhi / cosPhi;
}

float random(vec2 coordinate) {
    return fract(sin(dot(coordinate, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
    vec2 pixel = v_texcoord * fullSize;
    vec2 inwardNormal;
    float distanceToEdge = roundedRectField(pixel, inwardNormal);

    float coverage = 1.0 - smoothstep(
        0.0, 1.0, (-distanceToEdge + SMOOTHING) / (SMOOTHING * 2.0));
    if (coverage <= 0.002)
        discard;

    if (applyTint == MODE_FRAME) {
        // Two exclusive rings cut out of the same coverage curve. Taking
        // differences of nested masks keeps their sum stable through the curved
        // corners, so the translucent colors never cross-fade into a gray halo.
        float ring = max(thick, 1.0);
        float bandAA = clamp(fwidth(distanceToEdge) * 0.5, 0.35, 0.60);
        float afterDark = smoothstep(ring - bandAA, ring + bandAA,
                                     distanceToEdge);
        float afterLight = smoothstep(2.0 * ring - bandAA, 2.0 * ring + bandAA,
                                      distanceToEdge);
        float darkAlpha = clamp(range, 0.0, 1.0) *
                          max(coverage - afterDark, 0.0) * alpha;
        float lightAlpha = clamp(contrast, 0.0, 1.0) *
                           max(afterDark - afterLight, 0.0) * alpha;
        float combined = lightAlpha + darkAlpha * (1.0 - lightAlpha);
        if (combined <= 0.002)
            discard;
        fragColor = vec4(vec3(lightAlpha), combined);
        return;
    }

    // The bevel width is the only length scale in the optics: the decay length
    // of the surface tilt, and the unit the displacement is measured in. Sizing
    // it near the corner radius is what keeps the refraction a rim rather than a
    // body-wide warp, and it also buries the field's medial-axis crease, which
    // begins around that same depth where the tilt has already decayed away.
    float halfMin = max(min(fullSize.x, fullSize.y) * 0.5, 1.0);
    float band = clamp(range, 4.0, halfMin);
    float depth = max(distanceToEdge, 0.0) / band;

    vec2 bendPixels = inwardNormal * bevelShift(depth) * band;

    // Clamping into the box keeps the lens inside its own silhouette, so the
    // optical footprint stays the blur kernel and damage needs no extra margin.
    vec2 samplePixel = clamp(pixel + bendPixels, vec2(0.5), fullSize - 0.5);
    vec2 sceneUV = (topLeft + samplePixel) / fullSizeUntransformed;

    // Dispersion follows the bend, so it fades out wherever the lens is flat
    // instead of tinting the whole body.
    float bendLength = length(bendPixels);
    vec2 chroma = bendLength > 0.001
        ? (bendPixels / bendLength) * vibrancy / fullSizeUntransformed
        : vec2(0.0);

    vec2 uvR = clamp(sceneUV + chroma, vec2(0.0), vec2(1.0));
    vec2 uvG = clamp(sceneUV, vec2(0.0), vec2(1.0));
    vec2 uvB = clamp(sceneUV - chroma, vec2(0.0), vec2(1.0));

    vec3 blurredGlass = vec3(texture(blurredBG, uvR).r, texture(blurredBG, uvG).g,
                             texture(blurredBG, uvB).b);
    vec3 sharpGlass = vec3(texture(tex, uvR).r, texture(tex, uvG).g,
                           texture(tex, uvB).b);

    // Frosted body, clearer rim. The displacement is only a couple of dozen
    // pixels while the blur kernel is about the same, so a uniformly blurred
    // scene is locally flat and bending it produces no visible change at all.
    // Letting detail survive where the bend is strongest is what makes the
    // refraction readable, and it matches how a thick edge carries the scene
    // through while the face stays frosted.
    float clarity = clamp(vibrancy_darkness, 0.0, 1.0) * exp(-1.5 * depth);
    vec3 glass = mix(blurredGlass, sharpGlass, clarity);

    // Desaturate before tinting. A translucent material that mixes the backdrop
    // at full chroma inherits whatever the wallpaper is doing, so a saturated
    // desktop turns a light surface colourful however much tint is applied.
    // Pulling the sample toward its own luminance first is what lets the material
    // stay translucent -- and keep its refraction visible -- while still reading
    // as a clean surface.
    float backdropLuma = dot(glass, vec3(0.2126, 0.7152, 0.0722));
    glass = mix(vec3(backdropLuma), glass, clamp(sdrSaturation, 0.0, 1.0));

    glass = mix(glass, tint, clamp(blurAlpha, 0.0, 1.0));

    // Directional rim light on the bevel. Additive rather than multiplicative:
    // a bevel catches light, it does not scale what is behind it. Riding the
    // same exponential depth keeps it on the bevel, so it cannot leave a seam.
    //
    // Clamped to positive: the raw directional term swings from -1 to +1, and the
    // negative half was subtracting light, painting a dark shade around the side
    // of the rim facing away. A highlight only ever adds. Squaring tightens it
    // into a specular streak instead of a broad wash over half the border.
    vec2 fromMiddle = v_texcoord - 0.5;
    float facing = max(sin(atan(fromMiddle.y, fromMiddle.x) - 0.5), 0.0);
    glass += vec3(facing * facing * brightness * exp(-2.0 * depth));

    glass += vec3((random(gl_FragCoord.xy) - 0.5) * noise);

    float opacity = alpha * coverage;
    fragColor = vec4(glass * opacity, opacity);
}
)GLSL";

bool ensureShader() {
  if (g_shader)
    return true;

  g_shader = makeShared<CShader>();
  if (g_shader->createProgram(VERTEX_SHADER, FRAGMENT_SHADER, true, false))
    return true;

  g_shader.reset();
  return false;
}

// Both passes share the same quad, projection and geometry uniforms.
WP<CShader> begin(const Glass::Shader::SGeometry &geometry, int mode) {
  auto transformedBox = geometry.box;
  g_pHyprRenderer->m_renderData.renderModif.applyToBox(transformedBox);
  const auto matrix = g_pHyprRenderer->projectBoxToTarget(transformedBox);

  const auto shader = g_pHyprOpenGL->useShader(g_shader);
  shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_TRUE, matrix.getMatrix());
  shader->setUniformFloat2(SHADER_TOP_LEFT,
                           static_cast<float>(transformedBox.x),
                           static_cast<float>(transformedBox.y));
  shader->setUniformFloat2(SHADER_FULL_SIZE,
                           static_cast<float>(transformedBox.width),
                           static_cast<float>(transformedBox.height));
  // Clamped so a caller can ask for a pill by passing a very large radius; the
  // corner field is only well defined while the radius fits inside the box.
  const auto limit = static_cast<float>(
      std::min(transformedBox.width, transformedBox.height) * 0.5);
  shader->setUniformFloat(SHADER_RADIUS,
                          std::clamp(geometry.radius, 0.F, limit));
  shader->setUniformFloat(SHADER_ROUNDING_POWER,
                          std::clamp(geometry.roundingPower, 2.F, 10.F));
  shader->setUniformInt(SHADER_APPLY_TINT, mode);
  return shader;
}

// Stays inside the frame's damage so every pixel written is also a pixel the
// compositor will present. Blur::claimDamage() is what widens that damage to the
// whole lens when a nearby change makes the rest of it stale.
void submit(const CBox &box, const CRegion &damage) {
  CRegion clipped = damage.copy();
  clipped.intersect(box);
  if (clipped.empty())
    return;

  g_pHyprOpenGL->blend(true);
  const auto shader = g_pHyprOpenGL->useShader(g_shader);
  glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
  clipped.forEachRect([](const auto &rect) {
    g_pHyprOpenGL->scissor(&rect,
                           g_pHyprRenderer->m_renderData.transformDamage);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  });
  glBindVertexArray(0);
  g_pHyprOpenGL->scissor(nullptr);
}
} // namespace

bool Glass::Shader::drawGlass(const Blur::SScene &scene,
                                   const SGeometry &geometry,
                                   const SGlass &glass, const CRegion &damage) {
  if (!scene.blurred || !scene.blurred->ok() || !scene.sharp ||
      !scene.sharp->ok() || damage.empty() || !ensureShader())
    return false;

  const auto bindScene = [](const SP<ITexture> &texture, GLenum unit) {
    glActiveTexture(unit);
    texture->bind();
    texture->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    texture->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    texture->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    texture->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  };
  bindScene(scene.sharp, GL_TEXTURE0);
  bindScene(scene.blurred, GL_TEXTURE1);

  const auto shader = begin(geometry, MODE_GLASS);
  shader->setUniformInt(SHADER_TEX, 0);
  shader->setUniformInt(SHADER_BLURRED_BG, 1);
  shader->setUniformFloat2(SHADER_FULL_SIZE_UNTRANSFORMED,
                           static_cast<float>(scene.size.x),
                           static_cast<float>(scene.size.y));
  shader->setUniformFloat3(SHADER_TINT, glass.tint.r, glass.tint.g,
                           glass.tint.b);
  shader->setUniformFloat(SHADER_ALPHA, std::clamp(glass.opacity, 0.F, 1.F));
  shader->setUniformFloat(SHADER_BLUR_ALPHA,
                          std::clamp(glass.tintOpacity, 0.F, 1.F));
  shader->setUniformFloat(SHADER_DISTORT,
                          std::clamp(glass.refraction, 0.F, 1.F));
  shader->setUniformFloat(SHADER_SHADOW_POWER,
                          std::clamp(glass.refractionPower, 0.1F, 8.F));
  shader->setUniformFloat(SHADER_BRIGHTNESS, std::clamp(glass.glow, 0.F, 1.F));
  shader->setUniformFloat(SHADER_VIBRANCY, std::max(glass.dispersion, 0.F));
  shader->setUniformFloat(SHADER_VIBRANCY_DARKNESS,
                          std::clamp(glass.clarity, 0.F, 1.F));
  shader->setUniformFloat(SHADER_NOISE, std::clamp(glass.noise, 0.F, 0.2F));
  shader->setUniformFloat(SHADER_SDR_SATURATION,
                          std::clamp(glass.saturation, 0.F, 1.F));
  shader->setUniformFloat(SHADER_RANGE, std::max(glass.band, 0.F));

  submit(geometry.box, damage);
  glActiveTexture(GL_TEXTURE1);
  scene.blurred->unbind();
  glActiveTexture(GL_TEXTURE0);
  scene.sharp->unbind();
  return true;
}

bool Glass::Shader::drawFrame(const SGeometry &geometry,
                                   const SFrame &frame, const CRegion &damage) {
  if (damage.empty() || !ensureShader())
    return false;
  if (frame.highlightOpacity <= 0.F && frame.borderOpacity <= 0.F)
    return true;

  const auto shader = begin(geometry, MODE_FRAME);
  shader->setUniformFloat(SHADER_ALPHA, std::clamp(frame.opacity, 0.F, 1.F));
  shader->setUniformFloat(SHADER_CONTRAST,
                          std::clamp(frame.highlightOpacity, 0.F, 1.F));
  shader->setUniformFloat(SHADER_RANGE,
                          std::clamp(frame.borderOpacity, 0.F, 1.F));
  shader->setUniformFloat(SHADER_THICK, std::max(frame.width, 1.F));

  submit(geometry.box, damage);
  return true;
}

void Glass::Shader::destroy() { g_shader.reset(); }
