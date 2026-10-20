#include "harness.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "render/PathBsdf.h"

namespace {

bool finite(ember::Vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

float luminance(ember::Vec3 value) {
  return 0.2126f * value.x + 0.7152f * value.y + 0.0722f * value.z;
}

} // namespace

TEST(path_bsdf_lambertian_cosine_pdf_and_sample_weight_agree) {
  ember::MaterialDesc material;
  material.baseColor = {0.7f, 0.4f, 0.2f, 1.0f};
  material.ior = 1.0f;
  const ember::Vec3 normal{0.0f, 0.0f, 1.0f};
  const ember::Vec3 outgoing{0.0f, 0.0f, 1.0f};
  const ember::Vec3 incoming = ember::normalize(ember::Vec3{0.3f, -0.4f, 0.8660254f});
  const ember::render::PathBsdfEval evaluation =
      ember::render::evaluatePathBsdf(material, normal, outgoing, incoming);
  CHECK_NEAR(evaluation.value.x, material.baseColor.r / ember::kPi, 1.0e-5f);
  CHECK_NEAR(evaluation.pdf, incoming.z / ember::kPi, 1.0e-5f);

  const auto sample = ember::render::samplePathBsdf(material, normal, outgoing, 0.0f, 0.25f, 0.36f);
  CHECK(sample.valid);
  CHECK(!sample.delta);
  CHECK_NEAR(ember::length(sample.direction), 1.0f, 1.0e-5f);
  const auto sampledEval = ember::render::evaluatePathBsdf(material, normal, outgoing, sample.direction);
  CHECK_NEAR(sample.pdf, sampledEval.pdf, 1.0e-6f);
  CHECK_NEAR(sample.weight.x, material.baseColor.r, 2.0e-5f);
  CHECK_NEAR(sample.weight.y, material.baseColor.g, 2.0e-5f);
  CHECK_NEAR(sample.weight.z, material.baseColor.b, 2.0e-5f);
}

TEST(path_bsdf_ggx_samples_match_evaluation_pdf) {
  ember::MaterialDesc material;
  material.roughness = 0.35f;
  material.metallic = 0.7f;
  const ember::Vec3 normal{0.0f, 1.0f, 0.0f};
  const ember::Vec3 outgoing = ember::normalize(ember::Vec3{0.2f, 0.9f, -0.1f});
  const auto sample = ember::render::samplePathBsdf(material, normal, outgoing, 0.5f, 0.31f, 0.42f);
  CHECK(sample.valid);
  CHECK(!sample.delta);
  const auto evaluation = ember::render::evaluatePathBsdf(material, normal, outgoing, sample.direction);
  CHECK(evaluation.pdf > 0.0f);
  CHECK_NEAR(sample.pdf, evaluation.pdf, std::max(1.0e-6f, evaluation.pdf * 1.0e-5f));
  CHECK(finite(sample.weight));
}

TEST(path_bsdf_energy_decreases_with_metallic_and_transmission_partition) {
  ember::MaterialDesc diffuse;
  diffuse.baseColor = {0.8f, 0.8f, 0.8f, 1.0f};
  // Evaluate away from the specular peak so this tests the diffuse-energy
  // reduction without conflating it with a rough metal's directional highlight.
  diffuse.roughness = 0.1f;
  const ember::Vec3 normal{0.0f, 0.0f, 1.0f};
  const ember::Vec3 direction = ember::normalize(ember::Vec3{0.45f, 0.0f, 0.8930286f});
  const float diffuseEnergy = luminance(ember::render::evaluatePathBsdf(diffuse, normal, normal, direction).value);

  ember::MaterialDesc metallic = diffuse;
  metallic.metallic = 1.0f;
  const float metallicDiffuseEnergy =
      luminance(ember::render::evaluatePathBsdf(metallic, normal, normal, direction).value);
  CHECK(metallicDiffuseEnergy < diffuseEnergy);

  ember::MaterialDesc glass = diffuse;
  glass.transmission = 1.0f;
  const float glassReflectionValue =
      luminance(ember::render::evaluatePathBsdf(glass, normal, normal, direction).value);
  CHECK(glassReflectionValue < diffuseEnergy);
}

TEST(path_bsdf_hemispherical_reflectance_and_transmission_remain_energy_bounded) {
  ember::MaterialDesc glass;
  glass.baseColor = {1.0f, 1.0f, 1.0f, 1.0f};
  glass.transmission = 1.0f;
  glass.roughness = 0.35f;
  glass.ior = 1.5f;
  const ember::Vec3 normal{0.0f, 0.0f, 1.0f};
  const ember::Vec3 outgoing{0.0f, 0.0f, 1.0f};
  constexpr int kZSteps = 160;
  constexpr int kPhiSteps = 320;
  const float dOmega = 4.0f * ember::kPi / static_cast<float>(kZSteps * kPhiSteps);
  float integratedEnergy = 0.0f;
  for (int zIndex = 0; zIndex < kZSteps; ++zIndex) {
    const float z = -1.0f + (2.0f * (static_cast<float>(zIndex) + 0.5f) / kZSteps);
    const float radius = std::sqrt(std::max(0.0f, 1.0f - z * z));
    for (int phiIndex = 0; phiIndex < kPhiSteps; ++phiIndex) {
      const float phi = ember::kTwoPi * (static_cast<float>(phiIndex) + 0.5f) / kPhiSteps;
      const ember::Vec3 incoming{radius * std::cos(phi), radius * std::sin(phi), z};
      const auto value = ember::render::evaluatePathBsdf(glass, normal, outgoing, incoming).value;
      integratedEnergy += luminance(value) * std::fabs(z) * dOmega;
    }
  }
  CHECK(integratedEnergy > 0.0f);
  CHECK(integratedEnergy <= 1.05f);
}

TEST(path_bsdf_roughness_broadens_ggx_reflection) {
  ember::MaterialDesc smooth;
  smooth.metallic = 1.0f;
  smooth.roughness = 0.08f;
  ember::MaterialDesc rough = smooth;
  rough.roughness = 0.7f;
  const ember::Vec3 normal{0.0f, 0.0f, 1.0f};
  const ember::Vec3 outgoing = ember::normalize(ember::Vec3{0.4f, 0.0f, 0.9165151f});
  const ember::Vec3 nearReflection{-outgoing.x, 0.0f, outgoing.z};
  const ember::Vec3 offReflection = ember::normalize(ember::Vec3{-0.2f, 0.0f, 0.9797959f});
  const float smoothPeak = luminance(ember::render::evaluatePathBsdf(smooth, normal, outgoing, nearReflection).value);
  const float roughPeak = luminance(ember::render::evaluatePathBsdf(rough, normal, outgoing, nearReflection).value);
  const float smoothTail = luminance(ember::render::evaluatePathBsdf(smooth, normal, outgoing, offReflection).value);
  const float roughTail = luminance(ember::render::evaluatePathBsdf(rough, normal, outgoing, offReflection).value);
  CHECK(smoothPeak > roughPeak);
  CHECK(roughTail > smoothTail);
}

TEST(path_bsdf_dielectric_fresnel_reflection_and_transmission_choices) {
  ember::MaterialDesc glass;
  glass.baseColor = {0.9f, 0.95f, 1.0f, 1.0f};
  glass.transmission = 1.0f;
  glass.roughness = 0.0f;
  glass.ior = 1.5f;
  const ember::Vec3 normal{0.0f, 0.0f, 1.0f};
  const ember::Vec3 outgoing{0.0f, 0.0f, 1.0f};
  const auto reflected = ember::render::samplePathBsdf(glass, normal, outgoing, 0.0f, 0.4f, 0.6f);
  const auto transmitted = ember::render::samplePathBsdf(glass, normal, outgoing, 0.5f, 0.4f, 0.6f);
  CHECK(reflected.valid && reflected.delta);
  CHECK(transmitted.valid && transmitted.delta);
  CHECK(reflected.direction.z > 0.999f);
  CHECK(transmitted.direction.z < -0.999f);
  CHECK_NEAR(reflected.weight.x * reflected.pdf, 0.04f, 1.0e-4f);
  CHECK(reflected.pdf > 0.0f && transmitted.pdf > 0.0f);

  const auto grazingReflection =
      ember::render::samplePathBsdf(glass, normal, ember::normalize(ember::Vec3{0.99f, 0.0f, 0.1f}),
                                    0.5f, 0.2f, 0.3f);
  CHECK(grazingReflection.valid);
  CHECK(grazingReflection.direction.z > 0.0f);
}

TEST(path_bsdf_total_internal_reflection_never_emits_invalid_sample) {
  ember::MaterialDesc glass;
  glass.transmission = 1.0f;
  glass.roughness = 0.0f;
  glass.ior = 1.5f;
  const ember::Vec3 normal{0.0f, 0.0f, 1.0f};
  const ember::Vec3 insideOutgoing = ember::normalize(ember::Vec3{0.9f, 0.0f, -0.4358899f});
  const auto sample = ember::render::samplePathBsdf(glass, normal, insideOutgoing, 0.99f, 0.3f, 0.7f);
  CHECK(sample.valid);
  CHECK(sample.delta);
  CHECK(sample.direction.z < 0.0f);
  CHECK_NEAR(sample.weight.x, 1.0f, 1.0e-5f);
  CHECK(finite(sample.direction) && finite(sample.weight));
}

TEST(path_bsdf_rough_microfacet_tir_reflects_with_the_mixture_pdf) {
  ember::MaterialDesc glass;
  glass.transmission = 1.0f;
  glass.roughness = 0.5f;
  glass.ior = 1.5f;
  const ember::Vec3 normal{0.0f, 0.0f, 1.0f};
  const ember::Vec3 insideOutgoing = ember::normalize(ember::Vec3{0.6f, 0.0f, -0.8f});

  // Select a tilted GGX normal that is below the critical angle for the macro
  // ray but causes TIR locally; both lobe choices can then map to reflection.
  const auto sample = ember::render::samplePathBsdf(
      glass, normal, insideOutgoing, 0.5f, 0.5f, 0.79f);
  CHECK(sample.valid);
  CHECK(!sample.delta);
  CHECK(sample.direction.z < 0.0f);
  CHECK(finite(sample.direction) && finite(sample.weight));
  const auto evaluation = ember::render::evaluatePathBsdf(
      glass, normal, insideOutgoing, sample.direction);
  CHECK(evaluation.pdf > 0.0f);
  CHECK_NEAR(sample.pdf, evaluation.pdf,
             std::max(1.0e-7f, evaluation.pdf * 1.0e-5f));
}

TEST(path_bsdf_grazing_invalid_vectors_and_malformed_material_stay_finite) {
  ember::MaterialDesc material;
  material.roughness = std::numeric_limits<float>::quiet_NaN();
  material.metallic = std::numeric_limits<float>::infinity();
  material.transmission = std::numeric_limits<float>::quiet_NaN();
  material.ior = std::numeric_limits<float>::infinity();
  const ember::Vec3 normal{0.0f, 0.0f, 1.0f};
  const ember::Vec3 grazing = ember::normalize(ember::Vec3{1.0f, 0.0f, 1.0e-8f});
  const auto evaluation = ember::render::evaluatePathBsdf(material, normal, grazing, normal);
  CHECK(finite(evaluation.value));
  CHECK(std::isfinite(evaluation.pdf));
  const auto invalid = ember::render::samplePathBsdf(material, {}, normal, 0.2f, 0.5f, 0.5f);
  CHECK(!invalid.valid);
  CHECK_EQ(invalid.pdf, 0.0f);
  const auto badSample = ember::render::samplePathBsdf(material, normal, normal,
      std::numeric_limits<float>::quiet_NaN(), 0.5f, 0.5f);
  CHECK(!badSample.valid);
}

HARNESS_MAIN()
