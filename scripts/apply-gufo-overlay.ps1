param(
  [Parameter(Mandatory=$true)][string]$GufoRoot,
  [Parameter(Mandatory=$true)][string]$OverlayRoot
)
$ErrorActionPreference='Stop'
$Utf8NoBom=New-Object System.Text.UTF8Encoding($false)

if(-not(Test-Path $GufoRoot)){throw "Gufo root not found: $GufoRoot"}
if(-not(Test-Path $OverlayRoot)){throw "Gufo overlay not found: $OverlayRoot"}

# Copy the project's known-good prepared Qwen3.8 sources after normal Gufo
# preparation. This deliberately avoids changing prepare-gufo.ps1's hash, so
# adopting this overlay cannot trigger an unnecessary hard reset by itself.
$files=Get-ChildItem $OverlayRoot -File -Recurse
foreach($file in $files){
  $relative=$file.FullName.Substring($OverlayRoot.Length).TrimStart([char[]]'\/')
  $destination=Join-Path $GufoRoot $relative
  $parent=Split-Path $destination -Parent
  New-Item -ItemType Directory -Force $parent | Out-Null
  Copy-Item $file.FullName $destination -Force
}

# Keep the Windows port's tuning mechanism, but do not enable its new
# performance policies as FlashNextVelocity defaults in this baseline pass.
$tuningPath=Join-Path $GufoRoot 'src\core\platform\tuning.hpp'
$tuning=[IO.File]::ReadAllText($tuningPath)
if($tuning.Contains('inline constexpr bool kWindowsDefault = true;')){
  $tuning=$tuning.Replace('inline constexpr bool kWindowsDefault = true;',
                          'inline constexpr bool kWindowsDefault = false;')
  [IO.File]::WriteAllText($tuningPath,$tuning,$Utf8NoBom)
}elseif(-not $tuning.Contains('inline constexpr bool kWindowsDefault = false;')){
  throw "Gufo platform tuning default changed upstream: $tuningPath"
}

# sampling.cpp is intentionally not vendored wholesale. Add one narrow helper
# to the pinned Gufo source. The helper keeps ORIGINAL vocabulary IDs through
# penalty/filter ordering; this is required for deterministic tie semantics in
# exact compact verification.
$samplingCpp=Join-Path $GufoRoot 'src\core\sampling.cpp'
if(-not(Test-Path $samplingCpp)){throw "Gufo sampling.cpp not found: $samplingCpp"}
$text=[IO.File]::ReadAllText($samplingCpp).Replace("`r`n","`n").Replace("`r","`n")
if(-not $text.Contains('SamplerState::DistributionMapped(')){
  $marker=@'
void SamplerState::DeferSample(TokenId token) {
'@
  $method=@'
SamplingDistribution SamplerState::DistributionMapped(
    std::span<const float> logits, std::span<const TokenId> token_ids) const {
  config_.Validate();
  if (logits.size() != token_ids.size())
    throw std::invalid_argument("compact logits and token IDs differ in size");
  if (logits.empty())
    throw std::invalid_argument("cannot sample an empty compact distribution");
  if (config_.top_k <= 0)
    throw std::invalid_argument("mapped compact distribution requires top-k");

  std::unordered_set<TokenId> seen;
  seen.reserve(token_ids.size());
  std::vector<Probability> candidates;
  candidates.reserve(logits.size());
  for (std::size_t i = 0; i < logits.size(); ++i) {
    const TokenId token = token_ids[i];
    if (!seen.insert(token).second)
      throw std::invalid_argument("compact distribution contains duplicate token IDs");
    if (!std::isfinite(logits[i]))
      continue;
    const auto found = std::ranges::lower_bound(penalty_counts_, token, {},
                                                &TokenPenalty::token);
    const double adjusted =
        found == penalty_counts_.end() || found->token != token
            ? static_cast<double>(logits[i])
            : Penalize(logits[i], config_, *found);
    if (!std::isfinite(adjusted))
      throw std::runtime_error(
          "sampling penalties produced a non-finite logit");
    candidates.push_back({.token = token, .value = adjusted});
  }
  if (candidates.empty())
    throw std::runtime_error("logit distribution contains no finite values");

  // This is the top_k>0 branch of PrepareSelected, applied to a support set
  // that the caller has already certified complete. Original vocabulary IDs
  // stay in Probability::token, preserving the full sampler's equal-score
  // tie ordering exactly.
  std::ranges::sort(candidates, IsBetterProbability);
  const std::size_t requested = std::max<std::size_t>(
      static_cast<std::size_t>(config_.top_k),
      std::max<std::size_t>(config_.min_keep, 1));
  candidates.resize(std::min(candidates.size(), requested));
  if (config_.temperature == 0.0F)
    return SamplingDistribution({{candidates.front().token, 1.0}}, 1.0);

  std::size_t keep = candidates.size();
  const std::size_t minimum = MinimumKept(config_, candidates.size());
  if (config_.top_p < 1.0F && keep > 1) {
    const double maximum = candidates.front().value;
    double sum = 0.0;
    for (const auto& candidate : candidates)
      sum += std::exp((candidate.value - maximum) / config_.temperature);
    const double target = static_cast<double>(config_.top_p) * sum;
    double cumulative = 0.0;
    std::size_t top_p_keep = 0;
    while (top_p_keep < keep) {
      cumulative += std::exp((candidates[top_p_keep].value - maximum) /
                             config_.temperature);
      ++top_p_keep;
      if (top_p_keep >= minimum && cumulative >= target)
        break;
    }
    keep = top_p_keep;
  }
  if (config_.min_p > 0.0F && keep > 1) {
    const double threshold = candidates.front().value +
                             static_cast<double>(config_.temperature) *
                                 std::log(static_cast<double>(config_.min_p));
    std::size_t min_p_keep = 0;
    while (min_p_keep < keep && candidates[min_p_keep].value >= threshold)
      ++min_p_keep;
    keep = std::min(keep, std::max(min_p_keep, minimum));
  }
  candidates.resize(keep);

  const double maximum = candidates.front().value;
  double total = 0.0;
  for (auto& candidate : candidates) {
    candidate.value =
        std::exp((candidate.value - maximum) / config_.temperature);
    total += candidate.value;
  }
  return SamplingDistribution(std::move(candidates), total);
}

void SamplerState::DeferSample(TokenId token) {
'@
  $count=([regex]::Matches($text,[regex]::Escape($marker))).Count
  if($count -ne 1){throw "sampling.cpp integration mismatch. Expected DeferSample marker once, found $count"}
  $text=$text.Replace($marker,$method)
  [IO.File]::WriteAllText($samplingCpp,$text,$Utf8NoBom)
}

$samplingHpp=Join-Path $GufoRoot 'src\core\sampling.hpp'
$hpp=[IO.File]::ReadAllText($samplingHpp)
if(-not $hpp.Contains('DistributionMapped(')){
  throw 'Gufo overlay failed: DistributionMapped declaration is missing.'
}
$cpp=[IO.File]::ReadAllText($samplingCpp)
if(-not $cpp.Contains('SamplerState::DistributionMapped(')){
  throw 'Gufo overlay failed: DistributionMapped implementation is missing.'
}

# The pinned Gufo scalar oracle currently normalizes the Qwen3.8 MTP hidden
# handoff as one HC*H row. The trained head and the preserved Halo runtime
# normalize each hyper-connection stream independently. Keep the reference
# oracle aligned with the production GPU path so parity tests catch regressions.
$referenceCpp=Join-Path $GufoRoot 'src\models\qwen38_flash_next\reference.cpp'
if(-not(Test-Path $referenceCpp)){throw "Gufo reference.cpp not found: $referenceCpp"}
$reference=[IO.File]::ReadAllText($referenceCpp).Replace("`r`n","`n").Replace("`r","`n")
$groupedMarker='const auto* hnorm = static_cast<const float*>(l.nextn_hnorm.data);'
if(-not $reference.Contains($groupedMarker)){
  $old=@'
  cpu::RmsNorm(norm, static_cast<const float*>(l.nextn_hnorm.data), c_.rms_eps);
'@
  $new=@'
  const auto* hnorm = static_cast<const float*>(l.nextn_hnorm.data);
  for (std::uint32_t s = 0; s < c_.hc_count; ++s) {
    cpu::RmsNorm(std::span<float>(norm.data() + static_cast<std::size_t>(s) * H, H),
                 hnorm + static_cast<std::size_t>(s) * H, c_.rms_eps);
  }
'@
  $count=([regex]::Matches($reference,[regex]::Escape($old))).Count
  if($count -ne 1){throw "reference.cpp MTP hnorm integration mismatch. Expected whole-row norm once, found $count"}
  $reference=$reference.Replace($old,$new)
  [IO.File]::WriteAllText($referenceCpp,$reference,$Utf8NoBom)
}
$reference=[IO.File]::ReadAllText($referenceCpp)
if(-not $reference.Contains($groupedMarker) -or
   $reference.Contains('cpu::RmsNorm(norm, static_cast<const float*>(l.nextn_hnorm.data), c_.rms_eps);')){
  throw 'Gufo overlay failed: scalar MTP hidden normalization is not per-HC-stream.'
}
Write-Host "FlashNextVelocity Gufo overlay applied: $($files.Count) source file(s) + mapped sampler helper + grouped MTP reference norm." -ForegroundColor Green
