#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"
#include "scene.glsl"
#include "rt.glsl"

// Nothing hit: the ray generation shader shades the dome.
layout(location = 0) rayPayloadInEXT RtHit hit;

void main()
{
    hit.t = -1.0;
}
