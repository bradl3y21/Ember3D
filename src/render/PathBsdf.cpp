#include "render/PathBsdf.h"

#include <algorithm>
#include <cmath>

namespace ember::render {

namespace {

constexpr float kPi = ember::kPi;
constexpr float kMinAlpha = 0.002025f; // roughness 0.045 squared
constexpr float kDeltaRoughness = 0.01f;
constexpr float kDirectionEpsilon = 1.0e-12f;

bool finite(float value) { return std::isfinite(value); }

bool finite(ember::Vec3 value) {
  return finite(value.x) && finite(value.y) && finite(value.z);
}

bool normalizeSafe(ember::Vec3 value, ember::Vec3& result) {
  if (!finite(value)) return false;
  const double x = value.x, y = value.y, z = value.z;
  const double lengthSquared = x * x + y * y + z * z;
  if (!(lengthSquared > kDirectionEpsilon) || !std::isfinite(lengthSquared)) return false;
  const double inverseLength = 1.0 / std::sqrt(lengthSquared);
  result = {static_cast<float>(x * inverseLength), static_cast<float>(y * inverseLength),
            static_cast<float>(z * inverseLength)};
  return finite(result);
}

float clampUnit(float value) {
  return finite(value) ? ember::clampf(value, 0.0f, 1.0f) : 0.0f;
}

float luminance(ember::Vec3 value) {
  return 0.2126f * value.x + 0.7152f * value.y + 0.0722f * value.z;
}

ember::Vec3 cleanColor(const ember::MaterialDesc& material) {
  const ember::Vec3 color = material.baseColor.rgb();
  return {finite(color.x) ? ember::clampf(color.x, 0.0f, 1.0f) : 0.8f,
          finite(color.y) ? ember::clampf(color.y, 0.0f, 1.0f) : 0.8f,
          finite(color.z) ? ember::clampf(color.z, 0.0f, 1.0f) : 0.8f};
}

float metallic(const ember::MaterialDesc& material) {
  return clampUnit(material.metallic);
}

float transmission(const ember::MaterialDesc& material) {
  return clampUnit(material.transmission);
}

float ior(const ember::MaterialDesc& material) {
  return finite(material.ior) && material.ior >= 1.0f && material.ior <= 3.0f
             ? material.ior
             : 1.45f;
}

float roughness(const ember::MaterialDesc& material) {
  return finite(material.roughness) ? ember::clampf(material.roughness, 0.0f, 1.0f) : 0.5f;
}

float alphaFor(float rough) {
  return std::max(rough * rough, kMinAlpha);
}

struct Frame {
  ember::Vec3 tangent;
  ember::Vec3 bitangent;
  ember::Vec3 normal;
};

Frame makeFrame(ember::Vec3 normal) {
  const ember::Vec3 helper = std::fabs(normal.z) < 0.999f
                                 ? ember::Vec3{0.0f, 0.0f, 1.0f}
                                 : ember::Vec3{1.0f, 0.0f, 0.0f};
  const ember::Vec3 tangent = ember::normalize(ember::cross(helper, normal));
  return {tangent, ember::cross(normal, tangent), normal};
}

ember::Vec3 toLocal(const Frame& frame, ember::Vec3 value) {
  return {ember::dot(value, frame.tangent), ember::dot(value, frame.bitangent),
          ember::dot(value, frame.normal)};
}

ember::Vec3 toWorld(const Frame& frame, ember::Vec3 value) {
  return frame.tangent * value.x + frame.bitangent * value.y + frame.normal * value.z;
}

float fresnelDielectric(float cosIncident, float etaIncident, float etaTransmitted) {
  cosIncident = ember::clampf(std::fabs(cosIncident), 0.0f, 1.0f);
  const float sinIncident = std::sqrt(std::max(0.0f, 1.0f - cosIncident * cosIncident));
  const float sinTransmitted = (etaIncident / etaTransmitted) * sinIncident;
  if (sinTransmitted >= 1.0f) return 1.0f;
  const float cosTransmitted = std::sqrt(std::max(0.0f, 1.0f - sinTransmitted * sinTransmitted));
  const float parallelDenominator = etaTransmitted * cosIncident + etaIncident * cosTransmitted;
  const float perpendicularDenominator = etaIncident * cosIncident + etaTransmitted * cosTransmitted;
  if (!(parallelDenominator > 0.0f) || !(perpendicularDenominator > 0.0f)) return 1.0f;
  const float parallel = (etaTransmitted * cosIncident - etaIncident * cosTransmitted) /
                         parallelDenominator;
  const float perpendicular = (etaIncident * cosIncident - etaTransmitted * cosTransmitted) /
                              perpendicularDenominator;
  return ember::clampf(0.5f * (parallel * parallel + perpendicular * perpendicular), 0.0f, 1.0f);
}

ember::Vec3 fresnelSchlick(ember::Vec3 f0, float cosine) {
  const float oneMinus = 1.0f - ember::clampf(cosine, 0.0f, 1.0f);
  const float fifth = oneMinus * oneMinus * oneMinus * oneMinus * oneMinus;
  return f0 + (ember::Vec3{1.0f, 1.0f, 1.0f} - f0) * fifth;
}

float ggxD(float cosTheta, float alpha) {
  if (!(cosTheta > 0.0f)) return 0.0f;
  const float alphaSquared = alpha * alpha;
  const float denominator = cosTheta * cosTheta * (alphaSquared - 1.0f) + 1.0f;
  return alphaSquared / (kPi * denominator * denominator);
}

float smithG1(float cosTheta, float alpha) {
  const float absoluteCosine = std::fabs(cosTheta);
  if (!(absoluteCosine > 0.0f)) return 0.0f;
  const float cosineSquared = absoluteCosine * absoluteCosine;
  const float tangentSquared = std::max(0.0f, 1.0f - cosineSquared) / cosineSquared;
  return 2.0f / (1.0f + std::sqrt(1.0f + alpha * alpha * tangentSquared));
}

ember::Vec3 sampleGgxNormal(float alpha, float u1, float u2) {
  u1 = ember::clampf(u1, 0.0f, 1.0f - 1.0e-7f);
  u2 = ember::clampf(u2, 0.0f, 1.0f - 1.0e-7f);
  const float phi = 2.0f * kPi * u1;
  const float alphaSquared = alpha * alpha;
  const float cosTheta = std::sqrt((1.0f - u2) / (1.0f + (alphaSquared - 1.0f) * u2));
  const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
  return {sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta};
}

ember::Vec3 sampleCosineHemisphere(float u1, float u2) {
  u1 = ember::clampf(u1, 0.0f, 1.0f - 1.0e-7f);
  u2 = ember::clampf(u2, 0.0f, 1.0f - 1.0e-7f);
  const float phi = 2.0f * kPi * u1;
  const float radius = std::sqrt(u2);
  return {radius * std::cos(phi), radius * std::sin(phi), std::sqrt(std::max(0.0f, 1.0f - u2))};
}

struct LobeProbabilities {
  float diffuse = 0.0f;
  float reflection = 0.0f;
  float transmission = 0.0f;
};

LobeProbabilities lobeProbabilities(const ember::MaterialDesc& material,
                                    ember::Vec3 normal, ember::Vec3 outgoing,
                                    float etaIncident, float etaTransmitted) {
  const ember::Vec3 base = cleanColor(material);
  const float metal = metallic(material);
  const float trans = transmission(material);
  const float cosOutgoing = ember::clampf(ember::dot(normal, outgoing), 0.0f, 1.0f);
  const float dielectricF = fresnelDielectric(cosOutgoing, etaIncident, etaTransmitted);
  const float baseEnergy = std::max(0.0f, luminance(base));
  const float diffuseWeight = (1.0f - metal) * (1.0f - trans) * baseEnergy * (1.0f - dielectricF);
  const float reflectionWeight = metal * baseEnergy + (1.0f - metal) * dielectricF;
  const float transmissionWeight = (1.0f - metal) * trans * baseEnergy * (1.0f - dielectricF);
  const float total = diffuseWeight + reflectionWeight + transmissionWeight;
  if (!(total > 0.0f) || !finite(total)) return {1.0f, 0.0f, 0.0f};
  return {diffuseWeight / total, reflectionWeight / total, transmissionWeight / total};
}

float cosinePdf(float cosine) {
  return std::max(0.0f, cosine) / kPi;
}

bool validInput(ember::Vec3 normal, ember::Vec3 outgoing,
                ember::Vec3& unitNormal, ember::Vec3& unitOutgoing) {
  return normalizeSafe(normal, unitNormal) && normalizeSafe(outgoing, unitOutgoing);
}

} // namespace

PathBsdfEval evaluatePathBsdf(const ember::MaterialDesc& material, ember::Vec3 normal,
                              ember::Vec3 outgoing, ember::Vec3 incoming) {
  PathBsdfEval result{};
  ember::Vec3 unitNormal, unitOutgoing, unitIncoming;
  if (!validInput(normal, outgoing, unitNormal, unitOutgoing) ||
      !normalizeSafe(incoming, unitIncoming)) return result;

  const float authoredSide = ember::dot(unitNormal, unitOutgoing);
  if (std::fabs(authoredSide) <= 1.0e-7f) return result;
  if (authoredSide < 0.0f) unitNormal = -unitNormal;
  const Frame frame = makeFrame(unitNormal);
  const ember::Vec3 wo = toLocal(frame, unitOutgoing);
  const ember::Vec3 wi = toLocal(frame, unitIncoming);
  const float cosO = wo.z;
  const float absCosI = std::fabs(wi.z);
  if (!(cosO > 1.0e-7f) || !(absCosI > 1.0e-7f)) return result;

  const ember::Vec3 base = cleanColor(material);
  const float metal = metallic(material);
  const float trans = transmission(material);
  const float etaMaterial = ior(material);
  const float rough = roughness(material);
  const float alpha = alphaFor(rough);
  const float etaIncident = authoredSide > 0.0f ? 1.0f : etaMaterial;
  const float etaTransmitted = authoredSide > 0.0f ? etaMaterial : 1.0f;
  const float fOut = fresnelDielectric(cosO, etaIncident, etaTransmitted);
  const float fIn = fresnelDielectric(absCosI, etaIncident, etaTransmitted);
  const LobeProbabilities probabilities =
      lobeProbabilities(material, unitNormal, unitOutgoing, etaIncident, etaTransmitted);
  const bool sameHemisphere = wi.z > 0.0f;

  if (sameHemisphere) {
    const float diffuseScale = (1.0f - metal) * (1.0f - trans) * (1.0f - fOut) * (1.0f - fIn);
    result.value += base * (diffuseScale / kPi);
    result.pdf += probabilities.diffuse * cosinePdf(wi.z);

    ember::Vec3 halfVector;
    if (normalizeSafe(wo + wi, halfVector) && halfVector.z < 0.0f) halfVector = -halfVector;
    const float woH = ember::dot(wo, halfVector);
    const float wiH = ember::dot(wi, halfVector);
    if (halfVector.z > 0.0f && woH > 1.0e-7f && wiH > 0.0f) {
      const float distribution = ggxD(halfVector.z, alpha);
      const float geometry = smithG1(cosO, alpha) * smithG1(wi.z, alpha);
      const float dielectricF = fresnelDielectric(woH, etaIncident, etaTransmitted);
      const ember::Vec3 metalF = fresnelSchlick(base, woH);
      const ember::Vec3 fresnel = metalF * metal + ember::Vec3{dielectricF} * (1.0f - metal);
      const float denominator = 4.0f * cosO * wi.z;
      if (denominator > 1.0e-12f) {
        result.value += fresnel * (distribution * geometry / denominator);
      }

      const float halfPdf = distribution * halfVector.z;
      const float jacobian = 1.0f / (4.0f * std::fabs(woH));
      if (finite(halfPdf) && finite(jacobian)) {
        // A microfacet selected through the transmission lobe becomes a
        // reflection event under total internal reflection.
        const float reflectionSelection = dielectricF >= 1.0f && trans > 0.0f && metal < 1.0f
                                              ? probabilities.reflection + probabilities.transmission
                                              : probabilities.reflection;
        result.pdf += reflectionSelection * halfPdf * jacobian;
      }
    }
  } else if (trans > 0.0f && metal < 1.0f) {
    // Walter-style GGX microfacet refraction. eta is etaT/etaI in the
    // half-vector/Jacobian convention. The radiance-transport form omits the
    // importance-mode eta^2 multiplier, so the sampled throughput retains the
    // usual etaI^2 / etaT^2 transmission factor.
    const float eta = etaTransmitted / etaIncident;
    ember::Vec3 halfVector;
    if (normalizeSafe(wo + wi * eta, halfVector) && halfVector.z < 0.0f) halfVector = -halfVector;
    const float woH = ember::dot(wo, halfVector);
    const float wiH = ember::dot(wi, halfVector);
    const float denominator = woH + eta * wiH;
    if (halfVector.z > 0.0f && woH > 1.0e-7f && wiH < 0.0f &&
        std::fabs(denominator) > 1.0e-8f) {
      const float distribution = ggxD(halfVector.z, alpha);
      const float geometry = smithG1(cosO, alpha) * smithG1(wi.z, alpha);
      const float dielectricF = fresnelDielectric(woH, etaIncident, etaTransmitted);
      const float factor = (1.0f - metal) * trans * (1.0f - dielectricF) *
                           distribution * geometry * std::fabs(woH * wiH) /
                           (cosO * absCosI * denominator * denominator);
      result.value += base * factor;

      const float halfPdf = distribution * halfVector.z;
      const float jacobian = eta * eta * std::fabs(wiH) / (denominator * denominator);
      if (finite(halfPdf) && finite(jacobian)) {
        result.pdf += probabilities.transmission * halfPdf * jacobian;
      }
    }
  }

  if (!finite(result.value) || result.value.x < 0.0f || result.value.y < 0.0f || result.value.z < 0.0f ||
      !finite(result.pdf) || result.pdf < 0.0f) return {};
  return result;
}

PathBsdfSample samplePathBsdf(const ember::MaterialDesc& material, ember::Vec3 normal,
                              ember::Vec3 outgoing, float lobeSample, float u1, float u2) {
  PathBsdfSample result{};
  ember::Vec3 unitNormal, unitOutgoing;
  if (!validInput(normal, outgoing, unitNormal, unitOutgoing) || !finite(lobeSample) ||
      !finite(u1) || !finite(u2)) return result;

  const float authoredSide = ember::dot(unitNormal, unitOutgoing);
  if (std::fabs(authoredSide) <= 1.0e-7f) return result;
  const float etaMaterial = ior(material);
  const float metal = metallic(material);
  const float trans = transmission(material);
  const float rough = roughness(material);
  const float etaIncident = authoredSide > 0.0f ? 1.0f : etaMaterial;
  const float etaTransmitted = authoredSide > 0.0f ? etaMaterial : 1.0f;
  const ember::Vec3 sourceNormal = unitNormal;
  if (authoredSide < 0.0f) unitNormal = -unitNormal;
  const Frame frame = makeFrame(unitNormal);
  const ember::Vec3 wo = toLocal(frame, unitOutgoing);
  if (!(wo.z > 1.0e-7f)) return result;

  const LobeProbabilities probabilities =
      lobeProbabilities(material, unitNormal, unitOutgoing, etaIncident, etaTransmitted);
  const float choice = ember::clampf(lobeSample, 0.0f, 1.0f - 1.0e-7f);
  const float roughAlpha = alphaFor(rough);
  if (choice < probabilities.diffuse) {
    result.direction = ember::normalize(toWorld(frame, sampleCosineHemisphere(u1, u2)));
  } else if (choice < probabilities.diffuse + probabilities.reflection) {
    if (rough <= kDeltaRoughness) {
      const float dielectricF = fresnelDielectric(wo.z, etaIncident, etaTransmitted);
      const ember::Vec3 metalF = fresnelSchlick(cleanColor(material), wo.z);
      const ember::Vec3 reflectionF = metalF * metal + ember::Vec3{dielectricF} * (1.0f - metal);
      result.direction = ember::normalize(toWorld(frame, ember::Vec3{-wo.x, -wo.y, wo.z}));
      const float lobeProbability = std::max(probabilities.reflection, 1.0e-20f);
      result.weight = reflectionF / lobeProbability;
      result.pdf = probabilities.reflection;
      result.delta = true;
      result.valid = finite(result.direction) && finite(result.weight) && result.pdf > 0.0f;
      return result;
    }
    const ember::Vec3 microNormal = sampleGgxNormal(roughAlpha, u1, u2);
    const ember::Vec3 wi = ember::reflect(-wo, microNormal);
    if (wi.z > 0.0f) result.direction = ember::normalize(toWorld(frame, wi));
  } else if (choice < probabilities.diffuse + probabilities.reflection + probabilities.transmission &&
             probabilities.transmission > 0.0f && metal < 1.0f) {
    if (rough <= kDeltaRoughness) {
      const float dielectricF = fresnelDielectric(wo.z, etaIncident, etaTransmitted);
      const float eta = etaIncident / etaTransmitted;
      const float sinSquaredTransmitted = eta * eta * std::max(0.0f, 1.0f - wo.z * wo.z);
      if (sinSquaredTransmitted >= 1.0f) {
        result.direction = ember::normalize(toWorld(frame, ember::Vec3{-wo.x, -wo.y, wo.z}));
        result.weight = ember::Vec3{1.0f};
        result.pdf = probabilities.transmission;
        result.delta = true;
        result.valid = result.pdf > 0.0f;
        return result;
      }
      const float cosTransmitted = std::sqrt(std::max(0.0f, 1.0f - sinSquaredTransmitted));
      const ember::Vec3 refracted{-eta * wo.x, -eta * wo.y, -cosTransmitted};
      result.direction = ember::normalize(toWorld(frame, refracted));
      const float lobeProbability = std::max(probabilities.transmission, 1.0e-20f);
      result.weight = cleanColor(material) *
                      ((1.0f - metal) * trans * (1.0f - dielectricF) * eta * eta /
                       lobeProbability);
      result.pdf = probabilities.transmission;
      result.delta = true;
      result.valid = finite(result.direction) && finite(result.weight) && result.pdf > 0.0f;
      return result;
    }
    const ember::Vec3 microNormal = sampleGgxNormal(roughAlpha, u1, u2);
    const float cosIncident = ember::dot(wo, microNormal);
    const float eta = etaIncident / etaTransmitted;
    const float sinSquaredTransmitted = eta * eta * std::max(0.0f, 1.0f - cosIncident * cosIncident);
    if (cosIncident > 0.0f) {
      if (sinSquaredTransmitted >= 1.0f) {
        const ember::Vec3 reflected = ember::reflect(-wo, microNormal);
        if (reflected.z > 0.0f) result.direction = ember::normalize(toWorld(frame, reflected));
      } else {
        const float cosTransmitted = std::sqrt(std::max(0.0f, 1.0f - sinSquaredTransmitted));
        const ember::Vec3 wi = -wo * eta + microNormal * (eta * cosIncident - cosTransmitted);
        if (wi.z < 0.0f) result.direction = ember::normalize(toWorld(frame, wi));
      }
    }
  }

  if (!finite(result.direction) || ember::lengthSq(result.direction) < 0.99f) return {};
  const PathBsdfEval evaluation = evaluatePathBsdf(material, sourceNormal, unitOutgoing, result.direction);
  const float cosine = std::fabs(ember::dot(sourceNormal, result.direction));
  if (!(evaluation.pdf > 0.0f) || !finite(evaluation.pdf) || !finite(evaluation.value) ||
      !(cosine > 0.0f)) return {};
  result.pdf = evaluation.pdf;
  result.weight = evaluation.value * (cosine / evaluation.pdf);
  result.valid = finite(result.weight) && result.weight.x >= 0.0f &&
                 result.weight.y >= 0.0f && result.weight.z >= 0.0f;
  result.delta = false;
  if (!result.valid) return {};
  return result;
}

} // namespace ember::render
