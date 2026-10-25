#version 460
#extension GL_EXT_ray_tracing : require

// Shadow rays: no opaque surface in the way (rgb: what translucent
// casters let through, rt_shadow.rahit).
layout(location = 1) rayPayloadInEXT vec4 shadowRay;

void main()
{
    shadowRay.a = 1.0;
}
