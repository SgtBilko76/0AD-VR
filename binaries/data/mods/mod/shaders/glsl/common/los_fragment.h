#ifndef INCLUDED_COMMON_LOS_FRAGMENT
#define INCLUDED_COMMON_LOS_FRAGMENT

#include "common/texture.h"

#if !IGNORE_LOS
float getLOS(sampler2D losTex, vec2 uv)
{
	float los = SAMPLE_2D(losTex, uv).r;
	float threshold = 0.03;
	return clamp(los - threshold, 0.0, 1.0) / (1.0 - threshold);

	return 1.0;
}

#if USE_SPIRV
// Quest VR (the only SPIR-V target of this build): unexplored shroud is
// see-through instead of black, so the sky or the real room shows behind
// the battlefield. Opacity fades from explored (~0.48) down to unexplored
// (0) with a world-locked dither; the colour keeps the explored brightness.
float getVisibleLOS(sampler2D losTex, vec2 uv)
{
	float los = getLOS(losTex, uv);
	float opacity = clamp(los / 0.45, 0.0, 1.0);
	float noise = fract(sin(dot(floor(uv * 8192.0), vec2(12.9898, 78.233))) * 43758.5453);
	if (opacity < 1.0 && opacity <= noise)
		discard;
	return max(los, 0.48);
}
#else
float getVisibleLOS(sampler2D losTex, vec2 uv)
{
	return getLOS(losTex, uv);
}
#endif
#endif

#endif // INCLUDED_COMMON_LOS_FRAGMENT
