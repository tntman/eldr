#version 450

// Full-screen triangle on the far plane: depth-tested LEQUAL against the
// cleared depth, so it only fills pixels no geometry covered.
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 1.0, 1.0);
}
