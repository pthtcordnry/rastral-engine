#define TINYGLTF_NOEXCEPTION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_IMPLEMENTATION
#include "tiny_gltf.h"

#include <cfloat>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

#include "math_helper.h"
#include "renderer.h"

// ============================================================================
// Public draw record
struct GLTFDraw {
    GLsizei indexCount = 0;
    GLsizei indexOffset = 0;
    GLuint  texture = 0;
    float   baseColor[4] = { 1,1,1,1 };

    bool    skinned = false;
    int     boneCount = 0;
    std::vector<float> bones16; // 16 * boneCount

    Mat4    localModel = matIdentity(); // mesh-node WM (for skinned)
    int     skinIndex = -1;            // which skin
};

// Exposed to the renderer
std::vector<GLTFDraw> gGLTFDraws;
float gModelFitRadius = 1.0f;
bool  gPlaceOnGround = true;

// ============================================================================
// Minimal Animation Runtime (idle-only, LINEAR/STEP)
enum AnimPath { AP_Translation, AP_Rotation, AP_Scale };
struct AnimSampler {
    int   inputAcc = -1;  // times
    int   outputAcc = -1; // values
    int   comps = 0;      // 3 for T/S, 4 for R
    std::string interp;   // "LINEAR" (default), "STEP", "CUBICSPLINE"
};
struct AnimChannel {
    int       sampler = -1;
    int       targetNode = -1;
    AnimPath  path = AP_Translation;
};
struct GLTFAnimation {
    std::string name;
    std::vector<AnimSampler> samplers;
    std::vector<AnimChannel> channels;
    float durationSec = 0.f;
};

struct GLTFSkin {
    std::vector<int>  joints;   // node indices
    std::vector<Mat4> invBind;  // per-joint
};

struct NodeTRS { float T[3]; float R[4]; float S[3]; };

static std::vector<GLTFSkin>      gSkins;
static std::vector<NodeTRS>       gBaseTRS;         // size = model.nodes
static std::vector<Mat4>          gGlobalsAnimated; // recomputed each frame
static std::vector<GLTFAnimation> gAnims;
static int   gIdleAnim = -1;
static float gIdleDuration = 0.f;

// Keep the loaded model for sampling
static tinygltf::Model gModelStatic;
float gModelTarget[3] = { 0.f, 0.f, 0.f };

// ============================================================================
// Small general 4x4 inverse (column-major)
static Mat4 matInverse(const Mat4& m) {
    const float* a = m.m;
    float inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    Mat4 r{};
    if (std::fabs(det) < 1e-8f) return matIdentity();
    float invDet = 1.0f / det;
    for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * invDet;
    return r;
}

// ============================================================================
// TRS builders and helpers
static Mat4 NodeLocalMatrix(const tinygltf::Node& n) {
    if (n.matrix.size() == 16) {
        Mat4 M{}; for (int i = 0; i < 16; ++i) M.m[i] = (float)n.matrix[i]; return M;
    }
    float tx = 0, ty = 0, tz = 0, qx = 0, qy = 0, qz = 0, qw = 1, sx = 1, sy = 1, sz = 1;
    if (n.translation.size() == 3) { tx = (float)n.translation[0]; ty = (float)n.translation[1]; tz = (float)n.translation[2]; }
    if (n.rotation.size() == 4) { qx = (float)n.rotation[0];   qy = (float)n.rotation[1];   qz = (float)n.rotation[2];   qw = (float)n.rotation[3]; }
    if (n.scale.size() == 3) { sx = (float)n.scale[0];      sy = (float)n.scale[1];      sz = (float)n.scale[2]; }
    return matMul(Mat4Translate(tx, ty, tz), matMul(matFromQuat(qx, qy, qz, qw), matScale(sx, sy, sz)));
}

static void ComputeGlobalTransforms(const tinygltf::Model& model, std::vector<Mat4>& globals) {
    globals.assign(model.nodes.size(), matIdentity());
    int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : (model.scenes.empty() ? -1 : 0);
    if (sceneIndex < 0) return;

    struct Item { int node; Mat4 parent; };
    std::vector<Item> st;
    for (int root : model.scenes[sceneIndex].nodes) st.push_back({ root, matIdentity() });

    while (!st.empty()) {
        auto it = st.back(); st.pop_back();
        const auto& n = model.nodes[it.node];
        Mat4 G = matMul(it.parent, NodeLocalMatrix(n));
        globals[it.node] = G;
        for (int c : n.children) st.push_back({ c, G });
    }
}

// ============================================================================
// Helpers to read accessors into float vectors (normalized-aware)
static bool getAsFloat(const tinygltf::Model& model, int accessorIndex, std::vector<float>& out, int& comps) {
    out.clear(); comps = 0;
    if (accessorIndex < 0 || accessorIndex >= (int)model.accessors.size()) return false;
    const tinygltf::Accessor& acc = model.accessors[accessorIndex];
    const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
    const tinygltf::Buffer& buf = model.buffers[bv.buffer];
    const uint8_t* base = buf.data.data() + bv.byteOffset + acc.byteOffset;
    const size_t stride = acc.ByteStride(bv);
    comps = tinygltf::GetNumComponentsInType(acc.type);
    out.resize((size_t)acc.count * comps);

    auto readUNorm8 = [](uint8_t  v)->float { return (float)v / 255.0f; };
    auto readSNorm8 = [](int8_t   v)->float { float f = (float)v / 127.0f;  return f < -1.f ? -1.f : (f > 1.f ? 1.f : f); };
    auto readUNorm16 = [](uint16_t v)->float { return (float)v / 65535.0f; };
    auto readSNorm16 = [](int16_t  v)->float { float f = (float)v / 32767.0f; return f < -1.f ? -1.f : (f > 1.f ? 1.f : f); };

    for (size_t i = 0; i < acc.count; ++i) {
        const uint8_t* p = base + i * stride;
        for (int c = 0; c < comps; ++c) {
            switch (acc.componentType) {
            case TINYGLTF_COMPONENT_TYPE_FLOAT:
                out[i * comps + c] = ((const float*)p)[c]; break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                out[i * comps + c] = acc.normalized ? readUNorm8(((const uint8_t*)p)[c]) : (float)((const uint8_t*)p)[c]; break;
            case TINYGLTF_COMPONENT_TYPE_BYTE:
                out[i * comps + c] = acc.normalized ? readSNorm8(((const int8_t*)p)[c]) : (float)((const int8_t*)p)[c]; break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                out[i * comps + c] = acc.normalized ? readUNorm16(((const uint16_t*)p)[c]) : (float)((const uint16_t*)p)[c]; break;
            case TINYGLTF_COMPONENT_TYPE_SHORT:
                out[i * comps + c] = acc.normalized ? readSNorm16(((const int16_t*)p)[c]) : (float)((const int16_t*)p)[c]; break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                out[i * comps + c] = (float)((const uint32_t*)p)[c]; break;
            case TINYGLTF_COMPONENT_TYPE_INT:
                out[i * comps + c] = (float)((const int32_t*)p)[c]; break;
            default:
                out[i * comps + c] = 0.0f; break;
            }
        }
    }
    return true;
}

// ============================================================================
// Tiny GL texture creation
static bool NeedsMips(GLint minF) {
    return minF == GL_NEAREST_MIPMAP_NEAREST || minF == GL_NEAREST_MIPMAP_LINEAR ||
        minF == GL_LINEAR_MIPMAP_NEAREST || minF == GL_LINEAR_MIPMAP_LINEAR;
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

// ============================================================================
// Public API: load GLTF into a single VBO/EBO + draw records + pre-xform
bool CreateMeshFromGLTF_PosUV_Textured(
    const char* path,
    GLuint& outVAO, GLuint& outVBO, GLuint& outEBO,
    Mat4& outPreXform)
{
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err, warn;

    bool ok = false;
    std::string p(path);
    auto endsWith = [&](const char* s) {
        size_t n = std::strlen(s), m = p.size();
        return m >= n && _stricmp(p.c_str() + m - n, s) == 0;
        };
    if (endsWith(".glb")) ok = loader.LoadBinaryFromFile(&model, &err, &warn, p);
    else                  ok = loader.LoadASCIIFromFile(&model, &err, &warn, p);
    if (!ok) { if (!warn.empty()) OutputDebugStringA(warn.c_str()); if (!err.empty()) OutputDebugStringA(err.c_str()); return false; }

    gModelStatic = model; // keep for animation sampling

    // Prepare base TRS + globals for animation
    gBaseTRS.assign(model.nodes.size(), { 0,0,0, 0,0,0,1, 1,1,1 });
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        const auto& n = model.nodes[i];
        NodeTRS b{ {0,0,0},{0,0,0,1},{1,1,1} };
        if (n.translation.size() == 3) { b.T[0] = (float)n.translation[0]; b.T[1] = (float)n.translation[1]; b.T[2] = (float)n.translation[2]; }
        if (n.rotation.size() == 4) { b.R[0] = (float)n.rotation[0];    b.R[1] = (float)n.rotation[1];    b.R[2] = (float)n.rotation[2];    b.R[3] = (float)n.rotation[3]; }
        if (n.scale.size() == 3) { b.S[0] = (float)n.scale[0];       b.S[1] = (float)n.scale[1];       b.S[2] = (float)n.scale[2]; }
        gBaseTRS[i] = b;
    }
    gGlobalsAnimated.assign(model.nodes.size(), matIdentity());

    // Cache skins
    gSkins.clear(); gSkins.resize(model.skins.size());
    for (size_t si = 0; si < model.skins.size(); ++si) {
        const auto& skin = model.skins[si];
        GLTFSkin S;
        S.joints.assign(skin.joints.begin(), skin.joints.end());
        std::vector<float> ib; int comps = 0;
        if (skin.inverseBindMatrices >= 0) getAsFloat(model, skin.inverseBindMatrices, ib, comps);
        S.invBind.resize(S.joints.size(), matIdentity());
        for (size_t j = 0; j < S.joints.size(); ++j) {
            if ((int)ib.size() >= (int)((j + 1) * 16)) {
                for (int k = 0; k < 16; ++k) S.invBind[j].m[k] = ib[j * 16 + k];
            }
        }
        gSkins[si] = std::move(S);
    }

    // Parse animations and pick an "idle" if present
    auto accessorMaxTime = [&](int accIdx)->float {
        if (accIdx < 0) return 0.f;
        const auto& acc = model.accessors[accIdx];
        if (acc.maxValues.empty()) return 0.f;
        return (float)acc.maxValues[0];
        };
    gAnims.clear(); gIdleAnim = -1; gIdleDuration = 0.f;
    for (const auto& a : model.animations) {
        GLTFAnimation A; A.name = a.name;
        A.samplers.resize(a.samplers.size());
        for (size_t si = 0; si < a.samplers.size(); ++si) {
            const auto& s = a.samplers[si];
            A.samplers[si].inputAcc = s.input;
            A.samplers[si].outputAcc = s.output;
            A.samplers[si].interp = s.interpolation.empty() ? std::string("LINEAR") : s.interpolation;
            if (s.output >= 0) {
                const auto& acc = model.accessors[s.output];
                A.samplers[si].comps = tinygltf::GetNumComponentsInType(acc.type);
            }
            A.durationSec = std::max(A.durationSec, accessorMaxTime(s.input));
        }
        for (const auto& c : a.channels) {
            AnimChannel C;
            C.sampler = c.sampler;
            C.targetNode = c.target_node;
            if (c.target_path == "translation") C.path = AP_Translation;
            else if (c.target_path == "rotation")    C.path = AP_Rotation;
            else                                      C.path = AP_Scale;
            A.channels.push_back(C);
        }
        int idx = (int)gAnims.size();
        gAnims.push_back(std::move(A));
        if (gIdleAnim < 0) gIdleAnim = idx;
        std::string low = a.name; for (auto& ch : low) ch = (char)tolower(ch);
        if (!low.empty() && low.find("idle") != std::string::npos) gIdleAnim = idx;
    }
    if (gIdleAnim >= 0) gIdleDuration = gAnims[gIdleAnim].durationSec;

    // Gather mesh nodes with their world matrices
    struct MeshNode { int nodeIndex; int meshIndex; Mat4 WM; };
    std::vector<MeshNode> meshNodes;

    std::vector<Mat4> globalXf;
    ComputeGlobalTransforms(model, globalXf);

    int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : (model.scenes.empty() ? -1 : 0);
    if (sceneIndex < 0) return false;
    // DFS
    std::vector<std::pair<int, Mat4>> st;
    for (int root : model.scenes[sceneIndex].nodes) st.push_back({ root, matIdentity() });
    while (!st.empty()) {
        auto it = st.back(); st.pop_back();
        const auto& node = model.nodes[it.first];
        Mat4 WM = matMul(it.second, NodeLocalMatrix(node));
        if (node.mesh >= 0) meshNodes.push_back({ it.first, node.mesh, WM });
        for (int c : node.children) st.push_back({ c, WM });
    }

    // Material textures cache
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
            const auto& smp = model.samplers[sIdx];
            auto convF = [](int f, GLint def)->GLint {
                if (f == TINYGLTF_TEXTURE_FILTER_NEAREST) return GL_NEAREST;
                if (f == TINYGLTF_TEXTURE_FILTER_LINEAR)  return GL_LINEAR;
                if (f == TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST) return GL_NEAREST_MIPMAP_NEAREST;
                if (f == TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST)  return GL_LINEAR_MIPMAP_NEAREST;
                if (f == TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR)  return GL_NEAREST_MIPMAP_LINEAR;
                if (f == TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR)   return GL_LINEAR_MIPMAP_LINEAR;
                return def;
                };
            auto convW = [](int w, GLint def)->GLint {
                if (w == TINYGLTF_TEXTURE_WRAP_REPEAT) return GL_REPEAT;
                if (w == TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE) return GL_CLAMP_TO_EDGE;
                if (w == TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT) return GL_MIRRORED_REPEAT;
                return def;
                };
            minF = convF(smp.minFilter, minF);
            magF = convF(smp.magFilter, magF);
            wrapS = convW(smp.wrapS, wrapS);
            wrapT = convW(smp.wrapT, wrapT);
        }
        texForTextureIdx[texIdx] = CreateGLTextureFromImage(model.images[imgIdx], minF, magF, wrapS, wrapT);
        return texForTextureIdx[texIdx];
        };

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

    // Accumulators
    std::vector<float> interleaved; // pos3 uv2 joints4 weights4 = 13 floats
    std::vector<uint32_t> indices;
    interleaved.reserve(1 << 20);
    indices.reserve(1 << 20);

    float minX = +FLT_MAX, minY = +FLT_MAX, minZ = +FLT_MAX;
    float maxX = -FLT_MAX, maxY = -FLT_MAX, maxZ = -FLT_MAX;

    for (const auto& mn : meshNodes) {
        const auto& node = model.nodes[mn.nodeIndex];
        const auto& mesh = model.meshes[mn.meshIndex];
        const Mat4& WM = mn.WM;
        const int skinIndex = node.skin; // -1 if none

        for (const auto& prim : mesh.primitives) {
            int posAcc = -1;
            if (auto it = prim.attributes.find("POSITION"); it != prim.attributes.end()) posAcc = it->second;
            int uvSet = 0;
            if (prim.material >= 0 && prim.material < (int)model.materials.size()) {
                uvSet = model.materials[prim.material].pbrMetallicRoughness.baseColorTexture.texCoord;
            }
            int uvAcc = findUVacc(prim, uvSet);

            // --- base vertex in global VBO (before writing this prim's verts)
            const uint32_t vbase = (uint32_t)(interleaved.size() / 13);

            // Indices
            std::vector<uint32_t> localIdx;
            if (prim.indices >= 0) {
                const auto& acc = model.accessors[prim.indices];
                const auto& bv = model.bufferViews[acc.bufferView];
                const auto& buf = model.buffers[bv.buffer];
                const uint8_t* base = buf.data.data() + bv.byteOffset + acc.byteOffset;
                size_t stride = acc.ByteStride(bv);
                switch (acc.componentType) {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                    for (size_t i = 0; i < acc.count; ++i) localIdx.push_back(((const uint32_t*)(base + i * stride))[0]); break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                    for (size_t i = 0; i < acc.count; ++i) localIdx.push_back(((const uint16_t*)(base + i * stride))[0]); break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                    for (size_t i = 0; i < acc.count; ++i) localIdx.push_back(((const uint8_t*)(base + i * stride))[0]); break;
                default: break;
                }
            }
            else {
                int vertCount = (posAcc >= 0) ? model.accessors[posAcc].count : 0;
                localIdx.resize(std::max(0, vertCount));
                for (int i = 0; i < vertCount; ++i) localIdx[i] = (uint32_t)i;
            }

            // --- offset indices into the global VBO
            for (auto& idx : localIdx) idx += vbase;

            std::vector<float> pos; int posComps = 0; getAsFloat(model, posAcc, pos, posComps);
            const size_t vertCount = posComps == 3 ? pos.size() / 3 : 0;

            std::vector<float> uv;  int uvComps = 0; if (uvAcc >= 0) getAsFloat(model, uvAcc, uv, uvComps);
            if (uvComps != 2) uv.assign(vertCount * 2, 0.0f);

            bool hasSkin = false;
            int jointsAcc = -1, weightsAcc = -1;
            if (auto it = prim.attributes.find("JOINTS_0"); it != prim.attributes.end())  jointsAcc = it->second;
            if (auto it = prim.attributes.find("WEIGHTS_0"); it != prim.attributes.end()) weightsAcc = it->second;
            hasSkin = (jointsAcc >= 0 && weightsAcc >= 0 && skinIndex >= 0);

            std::vector<float> jn; int jnComps = 0;
            std::vector<float> wt; int wtComps = 0;
            if (hasSkin) {
                std::vector<float> jtmp; int jc = 0; getAsFloat(model, jointsAcc, jtmp, jc);
                jn.resize(vertCount * 4);
                for (size_t i = 0; i < vertCount; ++i) for (int c = 0; c < 4; ++c) jn[i * 4 + c] = (jc ? jtmp[i * jc + c] : 0.f);
                getAsFloat(model, weightsAcc, wt, wtComps);
                if (wtComps != 4) hasSkin = false;
            }

            // Clamp joint indices & renormalize weights
            if (hasSkin) {
                const size_t jointCount = (skinIndex >= 0 && skinIndex < (int)model.skins.size())
                    ? model.skins[skinIndex].joints.size() : 0;
                for (size_t i = 0; i < vertCount; ++i) {
                    for (int c = 0; c < 4; ++c) {
                        int ji = (int)std::round(jn[i * 4 + c]);
                        if (ji < 0 || (jointCount > 0 && ji >= (int)jointCount)) ji = 0;
                        jn[i * 4 + c] = (float)ji;
                    }
                    float w0 = std::max(0.f, wt[i * 4 + 0]);
                    float w1 = std::max(0.f, wt[i * 4 + 1]);
                    float w2 = std::max(0.f, wt[i * 4 + 2]);
                    float w3 = std::max(0.f, wt[i * 4 + 3]);
                    float s = w0 + w1 + w2 + w3;
                    if (s > 1e-8f) { w0 /= s; w1 /= s; w2 /= s; w3 /= s; }
                    else { w0 = 1.f; w1 = w2 = w3 = 0.f; }
                    wt[i * 4 + 0] = w0; wt[i * 4 + 1] = w1; wt[i * 4 + 2] = w2; wt[i * 4 + 3] = w3;
                }
            }

            // Material
            float factor[4] = { 1,1,1,1 }; GLuint tex = 0;
            if (prim.material >= 0 && prim.material < (int)model.materials.size()) {
                const auto& m = model.materials[prim.material];
                if (m.pbrMetallicRoughness.baseColorFactor.size() == 4)
                    for (int i = 0; i < 4; ++i) factor[i] = (float)m.pbrMetallicRoughness.baseColorFactor[i];
                tex = getGLTexForMaterial(prim.material);
            }

            // Append indices with offset
            size_t indexOffset = indices.size();
            indices.insert(indices.end(), localIdx.begin(), localIdx.end());

            // Interleave (pos3 uv2 joints4 weights4)
            const size_t base = interleaved.size();
            interleaved.resize(base + vertCount * 13);
            float* dst = interleaved.data() + base;

            for (size_t i = 0; i < vertCount; ++i) {
                float x = pos[i * 3 + 0], y = pos[i * 3 + 1], z = pos[i * 3 + 2];

                // AABB in WORLD space (always WM for bounds)
                float wx, wy, wz; xformPoint(WM, x, y, z, wx, wy, wz);
                minX = std::min(minX, wx); minY = std::min(minY, wy); minZ = std::min(minZ, wz);
                maxX = std::max(maxX, wx); maxY = std::max(maxY, wy); maxZ = std::max(maxZ, wz);

                // Vertex payload: bake WM for non-skinned; keep mesh-space for skinned
                float vx = x, vy = y, vz = z;
                if (!hasSkin) xformPoint(WM, x, y, z, vx, vy, vz);

                dst[0] = vx; dst[1] = vy; dst[2] = vz;
                dst[3] = uv[i * 2 + 0]; dst[4] = uv[i * 2 + 1];

                if (hasSkin) {
                    dst[5] = jn[i * 4 + 0]; dst[6] = jn[i * 4 + 1]; dst[7] = jn[i * 4 + 2]; dst[8] = jn[i * 4 + 3];
                    dst[9] = wt[i * 4 + 0]; dst[10] = wt[i * 4 + 1]; dst[11] = wt[i * 4 + 2]; dst[12] = wt[i * 4 + 3];
                }
                else {
                    dst[5] = 0; dst[6] = 0; dst[7] = 0; dst[8] = 0;
                    dst[9] = 1; dst[10] = 0; dst[11] = 0; dst[12] = 0;
                }
                dst += 13;
            }

            GLTFDraw d{};
            d.indexCount = (GLsizei)localIdx.size();
            d.indexOffset = (GLsizei)indexOffset;
            d.texture = tex;
            d.baseColor[0] = factor[0]; d.baseColor[1] = factor[1]; d.baseColor[2] = factor[2]; d.baseColor[3] = factor[3];
            d.skinned = hasSkin;
            d.skinIndex = hasSkin ? skinIndex : -1;
            d.localModel = hasSkin ? WM : matIdentity();

            if (hasSkin) {
                const tinygltf::Skin& skin = model.skins[skinIndex];
                const int jointCount = (int)skin.joints.size();
                d.bones16.resize(jointCount * 16);
                d.boneCount = jointCount;

                std::vector<float> invBind; int ibComps = 0;
                if (skin.inverseBindMatrices >= 0) getAsFloat(model, skin.inverseBindMatrices, invBind, ibComps);
                for (int j = 0; j < jointCount; ++j) {
                    int jointNode = skin.joints[j];
                    Mat4 G = globalXf[jointNode];
                    Mat4 IB = matIdentity();
                    if ((int)invBind.size() >= (j + 1) * 16) for (int k = 0; k < 16; ++k) IB.m[k] = invBind[j * 16 + k];
                    Mat4 B = matMul(G, IB); // rest-pose
                    for (int k = 0; k < 16; ++k) d.bones16[j * 16 + k] = B.m[k];
                }
            }

            gGLTFDraws.push_back(std::move(d));
        }
    }

    // Fit to a nice frame
    float cx = 0.5f * (minX + maxX);
    float cy = 0.5f * (minY + maxY);
    float cz = 0.5f * (minZ + maxZ);
    float height = std::max(0.0001f, (maxY - minY));
    gModelFitRadius = 0.5f * std::max(std::max((maxX - minX), (maxY - minY)), (maxZ - minZ));

    float s = (height > 0.0001f) ? (2.0f / height) : 1.0f;
    outPreXform = matMul(Mat4Translate(-cx, -cy, -cz), matScale(s, s, s));

    Mat4 axisFix = matIdentity();
    axisFix = matRotateX(-1.5707963f);
    outPreXform = matMul(axisFix, outPreXform);

    if (gPlaceOnGround) outPreXform = matMul(Mat4Translate(0.f, 1.f, 0.f), outPreXform);
    
    float tx, ty, tz;
    xformPoint(outPreXform, cx, cy, cz, tx, ty, tz);
    gModelTarget[0] = tx;      // usually 0
    gModelTarget[1] = ty;      // usually 1 if gPlaceOnGround=true, else 0
    gModelTarget[2] = tz;

    // --- IMPORTANT: scale the fit radius to match the pre-xform we just applied
    gModelFitRadius *= s;

    // Upload buffers and build VAO
    glGenVertexArrays(1, &outVAO);
    glBindVertexArray(outVAO);

    glGenBuffers(1, &outVBO);
    glBindBuffer(GL_ARRAY_BUFFER, outVBO);
    glBufferData(GL_ARRAY_BUFFER, interleaved.size() * sizeof(float), interleaved.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &outEBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, outEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint32_t), indices.data(), GL_STATIC_DRAW);

    const GLsizei stride = (GLsizei)(13 * sizeof(float)); // pos3 uv2 j4 w4

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
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

// ============================================================================
// Animation runtime API
const tinygltf::Model& GLTF_GetModel() { return gModelStatic; }

static void normalizeQ(float q[4]) {
    float L = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (L > 1e-8f) { q[0] /= L; q[1] /= L; q[2] /= L; q[3] /= L; }
}
static void sampleVec3(const std::vector<float>& times, const std::vector<float>& vals, float t, float out[3], bool step) {
    if (times.empty() || vals.empty()) { out[0] = out[1] = out[2] = 0.f; return; }
    if (t <= times.front()) { out[0] = vals[0]; out[1] = vals[1]; out[2] = vals[2]; return; }
    if (t >= times.back()) { size_t n = vals.size(); out[0] = vals[n - 3]; out[1] = vals[n - 2]; out[2] = vals[n - 1]; return; }
    size_t i = 1; while (i<times.size() && t>times[i]) ++i; size_t i0 = i - 1, i1 = i;
    float u = step ? 0.f : (t - times[i0]) / std::max(1e-6f, times[i1] - times[i0]);
    for (int c = 0; c < 3; ++c) { float a = vals[i0 * 3 + c], b = vals[i1 * 3 + c]; out[c] = a * (1.f - u) + b * u; }
}
static void sampleQuat(const std::vector<float>& times, const std::vector<float>& vals, float t, float out[4], bool step) {
    if (times.empty() || vals.empty()) { out[0] = out[1] = out[2] = 0.f; out[3] = 1.f; return; }
    if (t <= times.front()) { out[0] = vals[0]; out[1] = vals[1]; out[2] = vals[2]; out[3] = vals[3]; normalizeQ(out); return; }
    if (t >= times.back()) { size_t n = vals.size(); out[0] = vals[n - 4]; out[1] = vals[n - 3]; out[2] = vals[n - 2]; out[3] = vals[n - 1]; normalizeQ(out); return; }
    size_t i = 1; while (i<times.size() && t>times[i]) ++i; size_t i0 = i - 1, i1 = i;
    if (step) { for (int c = 0; c < 4; ++c) out[c] = vals[i0 * 4 + c]; normalizeQ(out); return; }
    float q0[4] = { vals[i0 * 4 + 0],vals[i0 * 4 + 1],vals[i0 * 4 + 2],vals[i0 * 4 + 3] };
    float q1[4] = { vals[i1 * 4 + 0],vals[i1 * 4 + 1],vals[i1 * 4 + 2],vals[i1 * 4 + 3] };
    normalizeQ(q0); normalizeQ(q1);
    float dot = q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3];
    if (dot < 0.f) { for (int c = 0; c < 4; ++c) q1[c] = -q1[c]; dot = -dot; }
    float u = (t - times[i0]) / std::max(1e-6f, times[i1] - times[i0]);
    if (dot > 0.9995f) { for (int c = 0; c < 4; ++c) out[c] = q0[c] * (1.f - u) + q1[c] * u; normalizeQ(out); return; }
    float th = std::acos(dot);
    float s0 = std::sin((1.f - u) * th) / std::sin(th);
    float s1 = std::sin(u * th) / std::sin(th);
    for (int c = 0; c < 4; ++c) out[c] = q0[c] * s0 + q1[c] * s1;
}

void GLTF_UpdateAnimation_Pose(const tinygltf::Model& model, float tSec) {
    if (gIdleAnim < 0 || gAnims.empty()) {
        ComputeGlobalTransforms(model, gGlobalsAnimated);
        return;
    }
    const GLTFAnimation& A = gAnims[gIdleAnim];
    float t = (gIdleDuration > 0.f) ? std::fmod(std::max(0.f, tSec), gIdleDuration) : tSec;

    std::vector<NodeTRS> cur = gBaseTRS;

    for (const auto& ch : A.channels) {
        if (ch.targetNode < 0) continue;
        const AnimSampler& S = A.samplers[ch.sampler];

        std::vector<float> times; int tComps = 0; getAsFloat(model, S.inputAcc, times, tComps);
        std::vector<float> vals;  int vComps = 0; getAsFloat(model, S.outputAcc, vals, vComps);

        bool step = (S.interp == "STEP");
        if (ch.path == AP_Translation) {
            float v[3]; sampleVec3(times, vals, t, v, step);
            cur[ch.targetNode].T[0] = v[0]; cur[ch.targetNode].T[1] = v[1]; cur[ch.targetNode].T[2] = v[2];
        }
        else if (ch.path == AP_Scale) {
            float v[3]; sampleVec3(times, vals, t, v, step);
            cur[ch.targetNode].S[0] = v[0]; cur[ch.targetNode].S[1] = v[1]; cur[ch.targetNode].S[2] = v[2];
        }
        else { // rotation
            float q[4]; sampleQuat(times, vals, t, q, step);
            cur[ch.targetNode].R[0] = q[0]; cur[ch.targetNode].R[1] = q[1]; cur[ch.targetNode].R[2] = q[2]; cur[ch.targetNode].R[3] = q[3];
        }
    }

    // Rebuild globals: parent * (T*R*S)
    gGlobalsAnimated.assign(model.nodes.size(), matIdentity());
    int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : (model.scenes.empty() ? -1 : 0);
    if (sceneIndex < 0) return;

    struct Item { int node; Mat4 parent; };
    std::vector<Item> st;
    for (int root : model.scenes[sceneIndex].nodes) st.push_back({ root, matIdentity() });

    while (!st.empty()) {
        auto it = st.back(); st.pop_back();
        const auto& n = model.nodes[it.node];
        const NodeTRS& B = cur[it.node];
        Mat4 M = matMul(Mat4Translate(B.T[0], B.T[1], B.T[2]), matMul(matFromQuat(B.R[0], B.R[1], B.R[2], B.R[3]), matScale(B.S[0], B.S[1], B.S[2])));
        Mat4 G = matMul(it.parent, M);
        gGlobalsAnimated[it.node] = G;
        for (int c : n.children) st.push_back({ c, G });
    }
}

// Build animated bone palette for a draw (uses G * IB)
void GLTF_GetBonesForDraw(const GLTFDraw& d, std::vector<float>& out16) {
    out16.clear();
    if (!d.skinned || d.skinIndex < 0 || d.boneCount <= 0) return;
    const GLTFSkin& S = gSkins[(size_t)d.skinIndex];
    out16.resize(d.boneCount * 16);
    for (int j = 0; j < d.boneCount; ++j) {
        int jointNode = S.joints[j];
        Mat4 B = matMul(gGlobalsAnimated[jointNode], S.invBind[j]);
        for (int k = 0; k < 16; ++k) out16[j * 16 + k] = B.m[k];
    }
}
