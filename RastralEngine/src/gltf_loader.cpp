#define TINYGLTF_NOEXCEPTION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_IMPLEMENTATION
#include "tiny_gltf.h"

#include <cfloat>
#include <unordered_map>
#include "math_helper.h"
#include "renderer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

// ---------- Public draw record ----------
struct GLTFDraw {
    GLsizei indexCount;
    GLsizei indexOffset;
    GLuint  texture;
    float   baseColor[4];

    bool    skinned;
    int     boneCount;
    std::vector<float> bones16; // packed 16*boneCount floats

    Mat4    localModel; // NEW: mesh-node transform for this primitive
};

// Externally read by renderer
std::vector<GLTFDraw> gGLTFDraws;
float gModelFitRadius = 1.0f;
bool  gPlaceOnGround = true;

// ---------- Local helpers ----------
static Mat4 NodeLocalMatrix(const tinygltf::Node& n) {
    // If node.matrix exists, treat it as COLUMN-MAJOR and copy as-is.
    if (n.matrix.size() == 16) {
        Mat4 M{};
        for (int i = 0; i < 16; ++i) M.m[i] = (float)n.matrix[i]; // no transpose
        return M;
    }

    // Otherwise build from TRS in glTF order (T * R * S)
    float tx = 0, ty = 0, tz = 0;
    float qx = 0, qy = 0, qz = 0, qw = 1;
    float sx = 1, sy = 1, sz = 1;

    if (n.translation.size() == 3) { tx = (float)n.translation[0]; ty = (float)n.translation[1]; tz = (float)n.translation[2]; }
    if (n.rotation.size()    == 4) { qx = (float)n.rotation[0];    qy = (float)n.rotation[1];    qz = (float)n.rotation[2];    qw = (float)n.rotation[3];    }
    if (n.scale.size()       == 3) { sx = (float)n.scale[0];        sy = (float)n.scale[1];        sz = (float)n.scale[2];        }

    Mat4 T = Mat4Translate(tx, ty, tz);      // column-major
    Mat4 R = matFromQuat(qx, qy, qz, qw);        // column-major rotation from quaternion
    Mat4 S = matScale(sx, sy, sz);           // column-major

    return matMul(matMul(T, R), S); // T * R * S
}

GLenum GLWrap(int w) {
    switch (w) {
    case TINYGLTF_TEXTURE_WRAP_REPEAT: return GL_REPEAT;
    case TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT: return GL_MIRRORED_REPEAT;
    default: return GL_CLAMP_TO_EDGE;
    }
}
GLenum GLMinFilter(int f) {
    switch (f) {
    case TINYGLTF_TEXTURE_FILTER_NEAREST: return GL_NEAREST;
    case TINYGLTF_TEXTURE_FILTER_LINEAR: return GL_LINEAR;
    case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST: return GL_NEAREST_MIPMAP_NEAREST;
    case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST:  return GL_LINEAR_MIPMAP_NEAREST;
    case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR:  return GL_NEAREST_MIPMAP_LINEAR;
    case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR:   return GL_LINEAR_MIPMAP_LINEAR;
    default: return GL_LINEAR_MIPMAP_LINEAR;
    }
}
GLenum GLMagFilter(int f) {
    switch (f) {
    case TINYGLTF_TEXTURE_FILTER_NEAREST: return GL_NEAREST;
    case TINYGLTF_TEXTURE_FILTER_LINEAR:  return GL_LINEAR;
    default: return GL_LINEAR;
    }
}
bool NeedsMips(GLenum minf) {
    return (minf == GL_NEAREST_MIPMAP_NEAREST || minf == GL_LINEAR_MIPMAP_NEAREST ||
        minf == GL_NEAREST_MIPMAP_LINEAR || minf == GL_LINEAR_MIPMAP_LINEAR);
}

GLuint CreateGLTextureFromImage(const tinygltf::Image& img, int minF = -1, int magF = -1, int wrapS = -1, int wrapT = -1);

// ---------- New: keep node index with world matrix ----------
struct MeshNodeInfo {
    int  nodeIndex;
    int  meshIndex;
    Mat4 WM;
};

static void GatherMeshNodes(const tinygltf::Model& model, int nodeIndex, const Mat4& parent, std::vector<MeshNodeInfo>& out) {
    const tinygltf::Node& n = model.nodes[nodeIndex];
    Mat4 M = matMul(parent, NodeLocalMatrix(n));
    if (n.mesh >= 0) out.push_back({ nodeIndex, n.mesh, M });
    for (int ci : n.children) GatherMeshNodes(model, ci, M, out);
}

static void ComputeGlobalTransforms(const tinygltf::Model& model,
    std::vector<Mat4>& globals) {
    globals.assign(model.nodes.size(), matIdentity());
    int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : (model.scenes.empty() ? -1 : 0);
    if (sceneIndex < 0) return;

    struct StackItem { int node; Mat4 parent; };
    std::vector<StackItem> st;
    for (int root : model.scenes[sceneIndex].nodes) {
        st.push_back({ root, matIdentity() }); // <- identity at roots, no axis fix
    }
    while (!st.empty()) {
        auto it = st.back(); st.pop_back();
        const tinygltf::Node& n = model.nodes[it.node];
        Mat4 G = matMul(it.parent, NodeLocalMatrix(n));
        globals[it.node] = G;
        for (int c : n.children) st.push_back({ c, G });
    }
}

static GLuint CreateGLTextureFromImage(const tinygltf::Image& img, int minF, int magF, int wrapS, int wrapT) {
    if (img.width <= 0 || img.height <= 0 || img.image.empty()) return 0;
    int comp = img.component;
    std::vector<unsigned char> rgba;
    const unsigned char* pixels = nullptr;
    if (comp == 4) {
        pixels = img.image.data();
    }
    else {
        rgba.resize((size_t)img.width * img.height * 4);
        for (int i = 0, j = 0; i < img.width * img.height; ++i) {
            rgba[j + 0] = img.image[i * 3 + 0];
            rgba[j + 1] = img.image[i * 3 + 1];
            rgba[j + 2] = img.image[i * 3 + 2];
            rgba[j + 3] = 255; j += 4;
        }
        pixels = rgba.data();
    }
    if (minF < 0) minF = GL_LINEAR_MIPMAP_LINEAR;
    if (magF < 0) magF = GL_LINEAR;
    if (wrapS < 0) wrapS = GL_REPEAT;
    if (wrapT < 0) wrapT = GL_REPEAT;

    extern GLuint CreateTexture2D(int, int, GLenum, GLenum, GLenum, const void*, GLint, GLint, GLint, GLint);
    GLuint tex = CreateTexture2D(img.width, img.height, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, pixels, minF, magF, wrapS, wrapT);
    if (NeedsMips(minF)) { glBindTexture(GL_TEXTURE_2D, tex); glGenerateMipmap(GL_TEXTURE_2D); }
    return tex;
}

// ---------- Public API ----------
extern std::vector<GLTFDraw> gGLTFDraws;
extern float gModelFitRadius;
extern bool  gPlaceOnGround;

bool CreateMeshFromGLTF_PosUV_Textured(const char* path, GLuint& outVAO, GLuint& outVBO, GLuint& outEBO, Mat4& outPreXform) {
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err, warn;

    stbi_set_flip_vertically_on_load(1);

    bool ok = false;
    std::string p(path);
    auto endsWith = [&](const char* s) {
        size_t n = strlen(s), m = p.size();
        return m >= n && _stricmp(p.c_str() + m - n, s) == 0;
        };
    if (endsWith(".glb")) ok = loader.LoadBinaryFromFile(&model, &err, &warn, p);
    else ok = loader.LoadASCIIFromFile(&model, &err, &warn, p);
    if (!ok || !warn.empty() || !err.empty()) {
        if (!warn.empty()) OutputDebugStringA(("glTF warn: " + warn + "\n").c_str());
        if (!err.empty())  OutputDebugStringA(("glTF err: " + err + "\n").c_str());
        if (!ok) return false;
    }

    // Textures cache
    std::vector<GLuint> texForTextureIdx(model.textures.size(), 0);
    auto getGLTexForMaterial = [&](int matIndex)->GLuint {
        if (matIndex < 0 || matIndex >= (int)model.materials.size()) return 0;
        const auto& m = model.materials[matIndex];
        int texIdx = m.pbrMetallicRoughness.baseColorTexture.index;
        if (texIdx < 0) return 0;
        if (texIdx >= (int)model.textures.size()) return 0;
        if (texForTextureIdx[texIdx]) return texForTextureIdx[texIdx];

        const auto& t = model.textures[texIdx];
        int imgIdx = t.source; if (imgIdx < 0 || imgIdx >= (int)model.images.size()) return 0;

        int sIdx = t.sampler;
        GLint minF = GL_LINEAR_MIPMAP_LINEAR, magF = GL_LINEAR, wrapS = GL_REPEAT, wrapT = GL_REPEAT;
        if (sIdx >= 0 && sIdx < (int)model.samplers.size()) {
            const auto& s = model.samplers[sIdx];
            minF = GLMinFilter(s.minFilter); magF = GLMagFilter(s.magFilter);
            wrapS = GLWrap(s.wrapS); wrapT = GLWrap(s.wrapT);
        }
        texForTextureIdx[texIdx] = CreateGLTextureFromImage(model.images[imgIdx], minF, magF, wrapS, wrapT);
        return texForTextureIdx[texIdx];
        };

    int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : (model.scenes.empty() ? -1 : 0);
    if (sceneIndex < 0) return false;

    std::vector<MeshNodeInfo> meshNodes;
    for (int rootNode : model.scenes[sceneIndex].nodes) {
        GatherMeshNodes(model, rootNode, matIdentity(), meshNodes);
    }

    std::vector<Mat4> globalXf;
    ComputeGlobalTransforms(model, globalXf);

    // Utilities for reading attributes to floats (normalized for non-float)
    auto getAsFloat = [&](int accessorIndex, std::vector<float>& out, int& comps)->bool {
        if (accessorIndex < 0) return false;
        const auto& acc = model.accessors[accessorIndex];
        const auto& bv = model.bufferViews[acc.bufferView];
        const auto& buf = model.buffers[bv.buffer];
        const uint8_t* base = buf.data.data() + bv.byteOffset + acc.byteOffset;
        const size_t stride = acc.ByteStride(bv);
        comps = tinygltf::GetNumComponentsInType(acc.type);
        out.resize((size_t)acc.count * comps);

        auto convert = [&](auto readVal, float maxVal, bool isSigned) {
            for (size_t i = 0; i < acc.count; ++i) {
                const uint8_t* p = base + i * stride;
                for (int c = 0; c < comps; ++c) {
                    auto v = readVal(p, c);
                    float f = isSigned ? std::max(-1.f, std::min(1.f, float(v) / maxVal))
                        : std::max(0.f, std::min(1.f, float(v) / maxVal));
                    out[i * comps + c] = f;
                }
            }
            };

        switch (acc.componentType) {
        case TINYGLTF_COMPONENT_TYPE_FLOAT: {
            for (size_t i = 0; i < acc.count; ++i) {
                const float* src = (const float*)(base + i * stride);
                for (int c = 0; c < comps; ++c) out[i * comps + c] = src[c];
            } return true;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
            auto rd = [](const uint8_t* p, int c) {return ((const uint16_t*)p)[c]; };
            convert(rd, 65535.f, false); return true;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
            auto rd = [](const uint8_t* p, int c) {return ((const uint8_t*)p)[c]; };
            convert(rd, 255.f, false); return true;
        }
        case TINYGLTF_COMPONENT_TYPE_SHORT: {
            auto rd = [](const uint8_t* p, int c) {return ((const int16_t*)p)[c]; };
            convert(rd, 32767.f, true);  return true;
        }
        case TINYGLTF_COMPONENT_TYPE_BYTE: {
            auto rd = [](const uint8_t* p, int c) {return ((const int8_t*)p)[c]; };
            convert(rd, 127.f, true);  return true;
        }
        default: return false;
        }
    };

    auto readIndices = [&](int accessorIndex, std::vector<uint32_t>& out)->bool {
        if (accessorIndex < 0) return false;
        const auto& acc = model.accessors[accessorIndex];
        const auto& bv = model.bufferViews[acc.bufferView];
        const auto& buf = model.buffers[bv.buffer];
        const uint8_t* data = buf.data.data() + bv.byteOffset + acc.byteOffset;
        const size_t stride = acc.ByteStride(bv);
        out.resize(acc.count);
        switch (acc.componentType) {
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            for (size_t i = 0; i < acc.count; i++) out[i] = ((const uint8_t*)(data + i * stride))[0]; break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            for (size_t i = 0; i < acc.count; i++) out[i] = ((const uint16_t*)(data + i * stride))[0]; break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
            for (size_t i = 0; i < acc.count; i++) out[i] = ((const uint32_t*)(data + i * stride))[0]; break;
        default: return false;
        }
        return true;
    };


    // Build interleaved buffers
    std::vector<float>    interleaved;
    std::vector<uint32_t> indices;
    gGLTFDraws.clear();

    float minX = +FLT_MAX, minY = +FLT_MAX, minZ = +FLT_MAX;
    float maxX = -FLT_MAX, maxY = -FLT_MAX, maxZ = -FLT_MAX;

    size_t baseVertex = 0;

    auto findUVacc = [&](const tinygltf::Primitive& prim, int set)->int {
        if (set == 0) {
            if (auto it = prim.attributes.find("TEXCOORD_0"); it != prim.attributes.end()) return it->second;
        }
        else {
            char key[16]; std::snprintf(key, sizeof(key), "TEXCOORD_%d", set);
            if (auto it = prim.attributes.find(key); it != prim.attributes.end()) return it->second;
        }
        return -1;
        };

    for (const auto& mn : meshNodes) {
        const tinygltf::Node& node = model.nodes[mn.nodeIndex];
        const tinygltf::Mesh& mesh = model.meshes[mn.meshIndex];

        Mat4 WM = mn.WM;

        int skinIndex = node.skin; // -1 if none

        for (const auto& prim : mesh.primitives) {
            // POSITION/UV/JOINTS/WEIGHTS
            int posAcc = -1;
            if (auto it = prim.attributes.find("POSITION"); it != prim.attributes.end()) posAcc = it->second;

            int uvSet = 0;
            if (prim.material >= 0 && prim.material < (int)model.materials.size()) {
                const auto& m = model.materials[prim.material];
                uvSet = m.pbrMetallicRoughness.baseColorTexture.texCoord;
            }
            int uvAcc = findUVacc(prim, uvSet);
            if (uvAcc < 0) uvAcc = findUVacc(prim, 0);

            int jointsAcc = -1, weightsAcc = -1;
            if (auto it = prim.attributes.find("JOINTS_0"); it != prim.attributes.end()) jointsAcc = it->second;
            if (auto it = prim.attributes.find("WEIGHTS_0"); it != prim.attributes.end()) weightsAcc = it->second;

            // Read attributes
            std::vector<float> pos; int pcomps = 0; getAsFloat(posAcc, pos, pcomps);
            std::vector<float> uv;  int ucomps = 0; getAsFloat(uvAcc, uv, ucomps);
            std::vector<float> jn;  int jcomps = 0; getAsFloat(jointsAcc, jn, jcomps);
            std::vector<float> wt;  int wcomps = 0; getAsFloat(weightsAcc, wt, wcomps);

            if (ucomps != 2) { uv.assign((pos.size() / 3) * 2, 0.0f); }
            const bool hasSkin = (skinIndex >= 0 && jcomps == 4 && wcomps == 4);

            // Indices
            std::vector<uint32_t> localIdx;
            if (prim.indices >= 0) { if (!readIndices(prim.indices, localIdx)) continue; }
            else {
                localIdx.resize(pos.size() / 3);
                for (uint32_t i = 0; i < (uint32_t)localIdx.size(); ++i) localIdx[i] = i;
            }

            const size_t vertCount = pos.size() / 3;
            std::vector<float> thisInter; thisInter.reserve(vertCount * 13);

            for (size_t i = 0; i < vertCount; ++i) {
                float x = pos[i * 3 + 0], y = pos[i * 3 + 1], z = pos[i * 3 + 2];

                // AABB in WORLD for proper fit:
                float ax = x, ay = y, az = z;
                if (!hasSkin) {
                    // Non-skinned: AABB in the same (WM-baked) space as the payload
                    xformPoint(WM, x, y, z, ax, ay, az);
                }
                minX = std::min(minX, ax); minY = std::min(minY, ay); minZ = std::min(minZ, az);
                maxX = std::max(maxX, ax); maxY = std::max(maxY, ay); maxZ = std::max(maxZ, az);

                // Vertex payload:
                // - If SKINNED: keep ORIGINAL mesh-space position (no WM bake)
                // - Else: bake WM into position (legacy behavior)
                float vx = x, vy = y, vz = z;
                if (!hasSkin) {
                    xformPoint(WM, x, y, z, vx, vy, vz);
                }

                float u = uv[i * 2 + 0], v = uv[i * 2 + 1];

                thisInter.push_back(vx);
                thisInter.push_back(vy);
                thisInter.push_back(vz);
                thisInter.push_back(u);
                thisInter.push_back(v);

                if (hasSkin) {
                    thisInter.push_back(jn[i * 4 + 0]);
                    thisInter.push_back(jn[i * 4 + 1]);
                    thisInter.push_back(jn[i * 4 + 2]);
                    thisInter.push_back(jn[i * 4 + 3]);

                    thisInter.push_back(wt[i * 4 + 0]);
                    thisInter.push_back(wt[i * 4 + 1]);
                    thisInter.push_back(wt[i * 4 + 2]);
                    thisInter.push_back(wt[i * 4 + 3]);
                }
                else {
                    thisInter.push_back(0.f); thisInter.push_back(0.f); thisInter.push_back(0.f); thisInter.push_back(0.f);
                    thisInter.push_back(1.f); thisInter.push_back(0.f); thisInter.push_back(0.f); thisInter.push_back(0.f);
                }
            }

            const size_t indexOffset = indices.size();
            for (uint32_t ii : localIdx) indices.push_back((uint32_t)baseVertex + ii);
            baseVertex += (uint32_t)vertCount;

            // Texture / base color
            GLuint tex = getGLTexForMaterial(prim.material);
            float factor[4] = { 1,1,1,1 };
            if (prim.material >= 0 && prim.material < (int)model.materials.size()) {
                const auto& m = model.materials[prim.material];
                if (m.pbrMetallicRoughness.baseColorFactor.size() == 4) {
                    for (int i = 0; i < 4; i++) factor[i] = (float)m.pbrMetallicRoughness.baseColorFactor[i];
                }
            }

            GLTFDraw d{};
            d.indexCount = (GLsizei)localIdx.size();
            d.indexOffset = (GLsizei)indexOffset;
            d.texture = tex;
            d.baseColor[0] = factor[0]; d.baseColor[1] = factor[1];
            d.baseColor[2] = factor[2]; d.baseColor[3] = factor[3];
            d.skinned = hasSkin;

            // localModel & bones
            if (hasSkin) {
                // Per-draw mesh-node model
                d.localModel = WM;

                const tinygltf::Skin& skin = model.skins[skinIndex];

                // inverse bind matrices
                std::vector<float> invBind; int ibComps = 0;
                if (skin.inverseBindMatrices >= 0) getAsFloat(skin.inverseBindMatrices, invBind, ibComps);

                const int jointCount = (int)skin.joints.size();
                d.bones16.resize(jointCount * 16);
                d.boneCount = jointCount;

                for (int j = 0; j < jointCount; ++j) {
                    int jointNode = skin.joints[j];
                    Mat4 G = globalXf[jointNode];
                    Mat4 IB = matIdentity();
                    if ((int)invBind.size() >= (j + 1) * 16) {
                        for (int k = 0; k < 16; k++) IB.m[k] = invBind[j * 16 + k];
                    }
                    Mat4 B = matMul(G, IB); // NO WM^{-1} here
                    for (int k = 0; k < 16; k++) d.bones16[j * 16 + k] = B.m[k];
                }
            }
            else {
                // Non-skinned: positions were baked already; no extra local model
                d.localModel = matIdentity();
                d.boneCount = 0;
            }

            gGLTFDraws.push_back(std::move(d));
            interleaved.insert(interleaved.end(), thisInter.begin(), thisInter.end());
        }
    }

    if (indices.empty()) return false;

    // AABB already computed in WORLD, so fit math uses it directly
    const float cx = 0.5f * (minX + maxX);
    const float cy = 0.5f * (minY + maxY);
    const float cz = 0.5f * (minZ + maxZ);

    float height = std::max(1e-5f, maxY - minY);
    float s = 2.0f / height;
    s = std::max(0.01f, std::min(s, 100.0f));  // keep scale sane

    Mat4 C = Mat4Translate(-cx, -cy, -cz);
    Mat4 S = matScale(s, s, s);
    outPreXform = matMul(S, C);
    float ex = (maxX - minX) * s, ey = (maxY - minY) * s, ez = (maxZ - minZ) * s;
    gModelFitRadius = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez);
    if (gPlaceOnGround) outPreXform = matMul(Mat4Translate(0.f, 1.f, 0.f), outPreXform);

    // --------- Create VAO/VBO/EBO ---------
    glGenVertexArrays(1, &outVAO);
    glBindVertexArray(outVAO);

    glGenBuffers(1, &outVBO);
    glBindBuffer(GL_ARRAY_BUFFER, outVBO);
    glBufferData(GL_ARRAY_BUFFER, interleaved.size() * sizeof(float), interleaved.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &outEBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, outEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint32_t), indices.data(), GL_STATIC_DRAW);

    const GLsizei stride = (GLsizei)(13 * sizeof(float)); // pos3 uv2 joints4 weights4

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)(0));
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)((3 + 2) * sizeof(float)));
    glEnableVertexAttribArray(2);

    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, (void*)((3 + 2 + 4) * sizeof(float)));
    glEnableVertexAttribArray(3);

    glBindVertexArray(0);
    return true;
}
