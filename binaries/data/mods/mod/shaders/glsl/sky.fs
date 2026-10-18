#version 110

#include "sky.h"

#include "common/fragment.h"

void main()
{
#if USE_SPIRV
    // Quest VR: the battlefield floats in the sky, so mirror the sky below
    // the horizon instead of fading it to black.
    vec3 mirrored = vec3(v_tex.x, -abs(v_tex.y), v_tex.z);
    OUTPUT_FRAGMENT_SINGLE_COLOR(vec4(SAMPLE_CUBE(GET_DRAW_TEXTURE_CUBE(baseTex), mirrored).rgb, 1.0));
    return;
#endif
    vec3 color = SAMPLE_CUBE(GET_DRAW_TEXTURE_CUBE(baseTex), v_tex).rgb;

    float m = (1.0 - v_tex.y) - 0.75;
    m *= 4.0;

    OUTPUT_FRAGMENT_SINGLE_COLOR(vec4((v_tex.y > 0.0) ? clamp(color * m, 0.0, 1.0) : color, 1.0));
}
