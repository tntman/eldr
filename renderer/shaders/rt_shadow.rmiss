#version 460
#extension GL_EXT_ray_tracing : require

// Shadow rays: nothing in the way.
layout(location = 1) rayPayloadInEXT float unoccluded;

void main()
{
    unoccluded = 1.0;
}
