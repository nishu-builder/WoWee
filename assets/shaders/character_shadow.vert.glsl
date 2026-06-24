#version 450

layout(push_constant) uniform Push {
    mat4 lightSpaceMatrix;
    mat4 model;
} push;

layout(set = 2, binding = 0) readonly buffer BoneSSBO {
    mat4 bones[];
};

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aBoneWeights;
layout(location = 2) in uvec4 aBoneIndices;
layout(location = 3) in vec2 aTexCoord;

layout(location = 0) out vec2 TexCoord;

mat4 skinMatrix(uvec4 boneIndices, vec4 boneWeights) {
    float weightSum = boneWeights.x + boneWeights.y + boneWeights.z + boneWeights.w;
    if (weightSum <= 0.0001) {
        return mat4(1.0);
    }

    return bones[boneIndices.x] * boneWeights.x
         + bones[boneIndices.y] * boneWeights.y
         + bones[boneIndices.z] * boneWeights.z
         + bones[boneIndices.w] * boneWeights.w;
}

void main() {
    mat4 skinMat = skinMatrix(aBoneIndices, aBoneWeights);
    vec4 skinnedPos = skinMat * vec4(aPos, 1.0);
    TexCoord = aTexCoord;
    gl_Position = push.lightSpaceMatrix * push.model * skinnedPos;
}
