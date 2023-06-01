#version 330 core

precision mediump float;

layout (location = 0) in vec4 aPos;
layout (location = 1) in vec4 aTintColor;

out vec2 vTexCoord;
out vec4 vTintColor;
out vec3 vColor;    // DEBUG for non-textured display

out vec3 vFragPos;

uniform int ticks;      // ms since start
uniform mat4 model;     // model matrix
uniform mat4 transform; // Final mesh transform matrix from model to world space

void main()
{
    // Move the mesh vertices with the transform
    // Replace w with 1.0 as the vertices are normalized, and we're using w
    // just to flag that the vertex is the top left corner of the mesh
    gl_Position = transform * vec4(aPos.xyz, 1.0); 

    // This is for the fragment can determine which mosaic tile it's part of
    vFragPos = aPos.xyz;

    // The tint color is passed in to tint each vertex
    vTintColor = aTintColor;

    // This below is just to create a random vertex color for debugging untextured triangles
    float r = float(int(gl_VertexID * 204.95f) % 0xFF) / 255.f; 
    float g = float(int(gl_VertexID * 182.53f + aPos.x) % 0xFF) / 255.f; 
    float b = float(int(gl_VertexID * 159.65f + aPos.y) % 0xFF) / 255.f; 
    vColor = vec3(      // DEBUG: Change the colors of each triangle to be better visible
        r, g, b
    );
}