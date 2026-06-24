#version 450

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrix;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 viewPos;
    vec4 fogColor;
    vec4 fogParams;
    vec4 shadowParams;
};

layout(push_constant) uniform Push {
    mat4 model;
} push;

layout(set = 2, binding = 0) readonly buffer BoneSSBO {
    mat4 bones[];
};

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aBoneWeights;
layout(location = 2) in uvec4 aBoneIndices;
layout(location = 3) in vec3 aNormal;
layout(location = 4) in vec2 aTexCoord;
layout(location = 5) in vec4 aTangent;

layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 Normal;
layout(location = 2) out vec2 TexCoord;
layout(location = 3) out vec3 Tangent;
layout(location = 4) out vec3 Bitangent;

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
    vec3 skinnedNorm = mat3(skinMat) * aNormal;
    vec3 skinnedTan = mat3(skinMat) * aTangent.xyz;

    vec4 worldPos = push.model * skinnedPos;
    mat3 modelMat3 = mat3(push.model);
    FragPos = worldPos.xyz;
    Normal = modelMat3 * skinnedNorm;
    TexCoord = aTexCoord;

    // Gram-Schmidt re-orthogonalize tangent w.r.t. normal
    vec3 N = normalize(Normal);
    vec3 T = normalize(modelMat3 * skinnedTan);
    T = normalize(T - dot(T, N) * N);
    vec3 B = cross(N, T) * aTangent.w;

    Tangent = T;
    Bitangent = B;

    gl_Position = projection * view * worldPos;
}
