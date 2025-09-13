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
#include <memory>

#include "math_helper.h"
#include "renderer.h"

// ============================================================
// Public draw record
struct GLTFDraw {
    GLsizei indexCount = 0;
    GLsizei indexOffset = 0;
    GLuint  texture = 0;
    float   baseColor[4] = { 1,1,1,1 };

    bool    skinned = false;
    int     boneCount = 0;
    std::vector<float> bones16; // 16 * boneCount

    Mat4    localModel = matIdentity();
    int     skinIndex = -1;
};

// Exposed to the renderer
std::vector<GLTFDraw> gGLTFDraws;
float gModelFitRadius = 1.0f;
bool  gPlaceOnGround = true;

// ============================================================
// Animation state
enum AnimPath { AP_Translation, AP_Rotation, AP_Scale };

struct AnimSampler {
    int   inputAcc;    // times
    int   outputAcc;   // values
    int   comps;       // 3 or 4
    std::string interp;
    const tinygltf::Model* srcModel; // owning model for the accessors
    AnimSampler() : inputAcc(-1), outputAcc(-1), comps(0), interp("LINEAR"), srcModel(NULL) {}
};

struct AnimChannel {
    int       sampler;
    int       targetNode;
    AnimPath  path;
    AnimChannel() : sampler(-1), targetNode(-1), path(AP_Translation) {}
};

struct GLTFAnimation {
    std::string name;
    std::vector<AnimSampler> samplers;
    std::vector<AnimChannel> channels;
    float durationSec;
    GLTFAnimation() : durationSec(0.f) {}
};

struct GLTFSkin {
    std::vector<int>  joints;   // node indices
    std::vector<Mat4> invBind;  // per-joint
};

struct NodeTRS { float T[3]; float R[4]; float S[3]; };

std::vector<GLTFSkin>      gSkins;
std::vector<NodeTRS>       gBaseTRS;
std::vector<Mat4>          gGlobalsAnimated;
std::vector<GLTFAnimation> gAnims;

int   gIdleAnim = -1;
float gIdleDuration = 0.f;
int   gActiveAnim = -1;
float gActiveDuration = 0.f;
float gAnimT0 = 0.f;
static int   gBlendFrom = -1;
static int   gBlendTo = -1;
static float gBlendStart = 0.f;
static float gBlendDur = 0.f;
static float gBlendFromT0 = 0.f;
static float gBlendToT0 = 0.f;
static bool  gBlendActive = false;

tinygltf::Model gModelStatic;
std::vector<std::unique_ptr<tinygltf::Model> > gAnimSources; // keep donors alive
float gModelTarget[3] = { 0.f, 0.f, 0.f };

// ============================================================
// Helpers (no lambdas / no auto)
bool EndsWithNoCase(const std::string& s, const char* suf) {
    size_t n = std::strlen(suf);
    size_t m = s.size();
    if (m < n) return false;
#if defined(_WIN32)
    return _stricmp(s.c_str() + (m - n), suf) == 0;
#else
    // simple case-insensitive compare
    for (size_t i = 0; i < n; ++i) {
        char a = (char)tolower((unsigned char)s[m - n + i]);
        char b = (char)tolower((unsigned char)suf[i]);
        if (a != b) return false;
    }
    return true;
#endif
}

float ReadUNorm8(uint8_t v) { return (float)v / 255.0f; }
float ReadSNorm8(int8_t v) { float f = (float)v / 127.0f;  if (f < -1.f) f = -1.f; if (f > 1.f) f = 1.f; return f; }
float ReadUNorm16(uint16_t v) { return (float)v / 65535.0f; }
float ReadSNorm16(int16_t v) { float f = (float)v / 32767.0f; if (f < -1.f) f = -1.f; if (f > 1.f) f = 1.f; return f; }

int FindUVAccessor(const tinygltf::Primitive& prim, int set) {
    if (set == 0) {
        std::map<std::string, int>::const_iterator it = prim.attributes.find("TEXCOORD_0");
        if (it != prim.attributes.end()) return it->second;
    }
    else {
        char key[16];
        std::snprintf(key, sizeof(key), "TEXCOORD_%d", set);
        std::map<std::string, int>::const_iterator it = prim.attributes.find(key);
        if (it != prim.attributes.end()) return it->second;
    }
    return -1;
}

float AccessorMaxTime(const tinygltf::Model& m, int accIdx) {
    if (accIdx < 0 || accIdx >= (int)m.accessors.size()) return 0.f;
    const tinygltf::Accessor& acc = m.accessors[accIdx];
    if (acc.maxValues.empty()) return 0.f;
    return (float)acc.maxValues[0];
}

GLuint GetMaterialBaseColorTexture(std::vector<GLuint>& cache, const tinygltf::Model& model, int matIndex) {
    if (matIndex < 0 || matIndex >= (int)model.materials.size()) return 0;
    const tinygltf::Material& m = model.materials[matIndex];
    int texIdx = m.pbrMetallicRoughness.baseColorTexture.index;
    if (texIdx < 0 || texIdx >= (int)model.textures.size()) return 0;
    if (cache[texIdx]) return cache[texIdx];

    const tinygltf::Texture& t = model.textures[texIdx];
    int imgIdx = t.source;
    if (imgIdx < 0 || imgIdx >= (int)model.images.size()) return 0;

    GLint minF = GL_LINEAR_MIPMAP_LINEAR;
    GLint magF = GL_LINEAR;
    GLint wrapS = GL_REPEAT;
    GLint wrapT = GL_REPEAT;

    int sIdx = t.sampler;
    if (sIdx >= 0 && sIdx < (int)model.samplers.size()) {
        const tinygltf::Sampler& smp = model.samplers[sIdx];
        // filters
        if (smp.minFilter == TINYGLTF_TEXTURE_FILTER_NEAREST)                  minF = GL_NEAREST;
        else if (smp.minFilter == TINYGLTF_TEXTURE_FILTER_LINEAR)              minF = GL_LINEAR;
        else if (smp.minFilter == TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST) minF = GL_NEAREST_MIPMAP_NEAREST;
        else if (smp.minFilter == TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST)  minF = GL_LINEAR_MIPMAP_NEAREST;
        else if (smp.minFilter == TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR)  minF = GL_NEAREST_MIPMAP_LINEAR;
        else if (smp.minFilter == TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR)   minF = GL_LINEAR_MIPMAP_LINEAR;

        if (smp.magFilter == TINYGLTF_TEXTURE_FILTER_NEAREST) magF = GL_NEAREST;
        else if (smp.magFilter == TINYGLTF_TEXTURE_FILTER_LINEAR) magF = GL_LINEAR;

        // wraps
        if (smp.wrapS == TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE) wrapS = GL_CLAMP_TO_EDGE;
        else if (smp.wrapS == TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT) wrapS = GL_MIRRORED_REPEAT;
        if (smp.wrapT == TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE) wrapT = GL_CLAMP_TO_EDGE;
        else if (smp.wrapT == TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT) wrapT = GL_MIRRORED_REPEAT;
    }

    extern GLuint CreateTexture2D(int, int, GLenum, GLenum, GLenum, const void*, GLint, GLint, GLint, GLint);

    const tinygltf::Image& img = model.images[imgIdx];
    if (img.width <= 0 || img.height <= 0 || img.image.empty()) return 0;

    const unsigned char* pixels = NULL;
    std::vector<unsigned char> rgba;
    if (img.component == 4) {
        pixels = img.image.data();
    }
    else {
        rgba.resize((size_t)img.width * img.height * 4);
        int total = img.width * img.height;
        for (int i = 0, j = 0; i < total; ++i) {
            rgba[j + 0] = img.image[i * 3 + 0];
            rgba[j + 1] = img.image[i * 3 + 1];
            rgba[j + 2] = img.image[i * 3 + 2];
            rgba[j + 3] = 255;
            j += 4;
        }
        pixels = rgba.data();
    }

    GLuint tex = CreateTexture2D(img.width, img.height, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, pixels, minF, magF, wrapS, wrapT);
    if (minF == GL_NEAREST_MIPMAP_NEAREST || minF == GL_NEAREST_MIPMAP_LINEAR ||
        minF == GL_LINEAR_MIPMAP_NEAREST || minF == GL_LINEAR_MIPMAP_LINEAR) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    cache[texIdx] = tex;
    return tex;
}

Mat4 NodeLocalMatrix(const tinygltf::Node& n) {
    if (n.matrix.size() == 16) {
        Mat4 M; for (int i = 0; i < 16; ++i) M.m[i] = (float)n.matrix[i]; return M;
    }
    float tx = 0, ty = 0, tz = 0;
    float qx = 0, qy = 0, qz = 0, qw = 1;
    float sx = 1, sy = 1, sz = 1;
    if (n.translation.size() == 3) { tx = (float)n.translation[0]; ty = (float)n.translation[1]; tz = (float)n.translation[2]; }
    if (n.rotation.size() == 4) { qx = (float)n.rotation[0];    qy = (float)n.rotation[1];    qz = (float)n.rotation[2];    qw = (float)n.rotation[3]; }
    if (n.scale.size() == 3) { sx = (float)n.scale[0];       sy = (float)n.scale[1];       sz = (float)n.scale[2]; }
    return matMul(Mat4Translate(tx, ty, tz), matMul(matFromQuat(qx, qy, qz, qw), matScale(sx, sy, sz)));
}

void ComputeGlobalTransforms(const tinygltf::Model& model, std::vector<Mat4>& globals) {
    globals.assign(model.nodes.size(), matIdentity());
    int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : (model.scenes.empty() ? -1 : 0);
    if (sceneIndex < 0) return;

    struct Item { int node; Mat4 parent; };
    std::vector<Item> st;
    for (size_t i = 0; i < model.scenes[sceneIndex].nodes.size(); ++i) {
        int root = model.scenes[sceneIndex].nodes[i];
        Item it; it.node = root; it.parent = matIdentity();
        st.push_back(it);
    }

    while (!st.empty()) {
        Item it = st.back(); st.pop_back();
        const tinygltf::Node& n = model.nodes[it.node];
        Mat4 G = matMul(it.parent, NodeLocalMatrix(n));
        globals[it.node] = G;
        for (size_t c = 0; c < n.children.size(); ++c) {
            Item child; child.node = n.children[c]; child.parent = G;
            st.push_back(child);
        }
    }
}

bool getAsFloat(const tinygltf::Model& model, int accessorIndex, std::vector<float>& out, int& comps) {
    out.clear(); comps = 0;
    if (accessorIndex < 0 || accessorIndex >= (int)model.accessors.size()) return false;
    const tinygltf::Accessor& acc = model.accessors[accessorIndex];
    const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
    const tinygltf::Buffer& buf = model.buffers[bv.buffer];
    const uint8_t* base = buf.data.data() + bv.byteOffset + acc.byteOffset;
    size_t stride = acc.ByteStride(bv);
    comps = tinygltf::GetNumComponentsInType(acc.type);
    out.resize((size_t)acc.count * comps);

    for (size_t i = 0; i < acc.count; ++i) {
        const uint8_t* p = base + i * stride;
        for (int c = 0; c < comps; ++c) {
            switch (acc.componentType) {
            case TINYGLTF_COMPONENT_TYPE_FLOAT:
                out[i * comps + c] = ((const float*)p)[c]; break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                out[i * comps + c] = acc.normalized ? ReadUNorm8(((const uint8_t*)p)[c]) : (float)((const uint8_t*)p)[c]; break;
            case TINYGLTF_COMPONENT_TYPE_BYTE:
                out[i * comps + c] = acc.normalized ? ReadSNorm8(((const int8_t*)p)[c]) : (float)((const int8_t*)p)[c]; break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                out[i * comps + c] = acc.normalized ? ReadUNorm16(((const uint16_t*)p)[c]) : (float)((const uint16_t*)p)[c]; break;
            case TINYGLTF_COMPONENT_TYPE_SHORT:
                out[i * comps + c] = acc.normalized ? ReadSNorm16(((const int16_t*)p)[c]) : (float)((const int16_t*)p)[c]; break;
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

// ============================================================
// Mesh + textures
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
    if (EndsWithNoCase(p, ".glb")) ok = loader.LoadBinaryFromFile(&model, &err, &warn, p);
    else                           ok = loader.LoadASCIIFromFile(&model, &err, &warn, p);
    if (!ok) return false;

    gModelStatic = model;

    gBaseTRS.assign(model.nodes.size(), NodeTRS{ {0,0,0},{0,0,0,1},{1,1,1} });
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        const tinygltf::Node& n = model.nodes[i];
        NodeTRS b; b.T[0] = b.T[1] = b.T[2] = 0.f; b.R[0] = b.R[1] = b.R[2] = 0.f; b.R[3] = 1.f; b.S[0] = b.S[1] = b.S[2] = 1.f;
        if (n.translation.size() == 3) { b.T[0] = (float)n.translation[0]; b.T[1] = (float)n.translation[1]; b.T[2] = (float)n.translation[2]; }
        if (n.rotation.size() == 4) { b.R[0] = (float)n.rotation[0];    b.R[1] = (float)n.rotation[1];    b.R[2] = (float)n.rotation[2];    b.R[3] = (float)n.rotation[3]; }
        if (n.scale.size() == 3) { b.S[0] = (float)n.scale[0];       b.S[1] = (float)n.scale[1];       b.S[2] = (float)n.scale[2]; }
        gBaseTRS[i] = b;
    }
    gGlobalsAnimated.assign(model.nodes.size(), matIdentity());

    gSkins.clear();
    gSkins.resize(model.skins.size());
    for (size_t si = 0; si < model.skins.size(); ++si) {
        const tinygltf::Skin& skin = model.skins[si];
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
        gSkins[si] = S;
    }

    gAnims.clear(); gIdleAnim = -1; gIdleDuration = 0.f;
    for (size_t ai = 0; ai < model.animations.size(); ++ai) {
        const tinygltf::Animation& a = model.animations[ai];
        GLTFAnimation A;
        A.name = a.name;
        A.samplers.resize(a.samplers.size());
        for (size_t si = 0; si < a.samplers.size(); ++si) {
            const tinygltf::AnimationSampler& s = a.samplers[si];
            A.samplers[si].inputAcc = s.input;
            A.samplers[si].outputAcc = s.output;
            A.samplers[si].interp = s.interpolation.empty() ? "LINEAR" : s.interpolation;
            if (s.output >= 0) {
                const tinygltf::Accessor& acc = model.accessors[s.output];
                A.samplers[si].comps = tinygltf::GetNumComponentsInType(acc.type);
            }
            A.samplers[si].srcModel = &gModelStatic;
            float mt = AccessorMaxTime(model, s.input);
            if (mt > A.durationSec) A.durationSec = mt;
        }
        for (size_t ci = 0; ci < a.channels.size(); ++ci) {
            const tinygltf::AnimationChannel& c = a.channels[ci];
            AnimChannel C;
            C.sampler = c.sampler;
            C.targetNode = c.target_node;
            if (c.target_path == "translation") C.path = AP_Translation;
            else if (c.target_path == "rotation") C.path = AP_Rotation;
            else C.path = AP_Scale;
            A.channels.push_back(C);
        }
        int idx = (int)gAnims.size();
        gAnims.push_back(A);
        if (gIdleAnim < 0) gIdleAnim = idx;
        std::string low = A.name;
        for (size_t k = 0; k < low.size(); ++k) low[k] = (char)tolower((unsigned char)low[k]);
        if (!low.empty() && low.find("idle") != std::string::npos) gIdleAnim = idx;
    }
    if (gIdleAnim >= 0) gIdleDuration = gAnims[gIdleAnim].durationSec;
    gActiveAnim = (gIdleAnim >= 0) ? gIdleAnim : (gAnims.empty() ? -1 : 0);
    gActiveDuration = (gActiveAnim >= 0) ? gAnims[gActiveAnim].durationSec : 0.f;
    gAnimT0 = 0.f;

    // collect mesh nodes
    struct MeshNode { int nodeIndex; int meshIndex; Mat4 WM; };
    std::vector<MeshNode> meshNodes;

    std::vector<Mat4> globalXf;
    ComputeGlobalTransforms(model, globalXf);

    int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : (model.scenes.empty() ? -1 : 0);
    if (sceneIndex < 0) return false;

    std::vector<int> stackNode;
    std::vector<Mat4> stackMat;
    for (size_t i = 0; i < model.scenes[sceneIndex].nodes.size(); ++i) {
        stackNode.push_back(model.scenes[sceneIndex].nodes[i]);
        stackMat.push_back(matIdentity());
    }
    while (!stackNode.empty()) {
        int ni = stackNode.back(); stackNode.pop_back();
        Mat4 parent = stackMat.back(); stackMat.pop_back();

        const tinygltf::Node& node = model.nodes[ni];
        Mat4 WM = matMul(parent, NodeLocalMatrix(node));
        if (node.mesh >= 0) {
            MeshNode mn; mn.nodeIndex = ni; mn.meshIndex = node.mesh; mn.WM = WM;
            meshNodes.push_back(mn);
        }
        for (size_t c = 0; c < node.children.size(); ++c) {
            stackNode.push_back(node.children[c]);
            stackMat.push_back(WM);
        }
    }

    std::vector<GLuint> texForTextureIdx(model.textures.size(), 0);

    // accumulators
    std::vector<float>    interleaved; // pos3 uv2 j4 w4
    std::vector<uint32_t> indices;
    interleaved.reserve(1 << 20);
    indices.reserve(1 << 20);

    float minX = +FLT_MAX, minY = +FLT_MAX, minZ = +FLT_MAX;
    float maxX = -FLT_MAX, maxY = -FLT_MAX, maxZ = -FLT_MAX;

    for (size_t mn_i = 0; mn_i < meshNodes.size(); ++mn_i) {
        const MeshNode& mn = meshNodes[mn_i];
        const tinygltf::Node& node = model.nodes[mn.nodeIndex];
        const tinygltf::Mesh& mesh = model.meshes[mn.meshIndex];
        const Mat4& WM = mn.WM;
        const int skinIndex = node.skin;

        for (size_t pi = 0; pi < mesh.primitives.size(); ++pi) {
            const tinygltf::Primitive& prim = mesh.primitives[pi];

            int posAcc = -1;
            std::map<std::string, int>::const_iterator itp = prim.attributes.find("POSITION");
            if (itp != prim.attributes.end()) posAcc = itp->second;

            int uvSet = 0;
            if (prim.material >= 0 && prim.material < (int)model.materials.size()) {
                uvSet = model.materials[prim.material].pbrMetallicRoughness.baseColorTexture.texCoord;
            }
            int uvAcc = FindUVAccessor(prim, uvSet);

            const uint32_t vbase = (uint32_t)(interleaved.size() / 13);

            std::vector<uint32_t> localIdx;
            if (prim.indices >= 0) {
                const tinygltf::Accessor& acc = model.accessors[prim.indices];
                const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
                const tinygltf::Buffer& buf = model.buffers[bv.buffer];
                const uint8_t* basePtr = buf.data.data() + bv.byteOffset + acc.byteOffset;
                size_t stride = acc.ByteStride(bv);
                for (size_t i = 0; i < acc.count; ++i) {
                    if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                        uint32_t v = ((const uint32_t*)(basePtr + i * stride))[0];
                        localIdx.push_back(v);
                    }
                    else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                        uint32_t v = ((const uint16_t*)(basePtr + i * stride))[0];
                        localIdx.push_back(v);
                    }
                    else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                        uint32_t v = ((const uint8_t*)(basePtr + i * stride))[0];
                        localIdx.push_back(v);
                    }
                }
            }
            else {
                int vertCount = (posAcc >= 0) ? model.accessors[posAcc].count : 0;
                localIdx.resize((size_t)std::max(0, vertCount));
                for (int i = 0; i < vertCount; ++i) localIdx[(size_t)i] = (uint32_t)i;
            }
            for (size_t i = 0; i < localIdx.size(); ++i) localIdx[i] += vbase;

            std::vector<float> pos; int posComps = 0; getAsFloat(model, posAcc, pos, posComps);
            size_t vertCount = (posComps == 3) ? pos.size() / 3 : 0;

            std::vector<float> uv;  int uvComps = 0;
            if (uvAcc >= 0) getAsFloat(model, uvAcc, uv, uvComps);
            if (uvComps != 2) uv.assign(vertCount * 2, 0.0f);

            bool hasSkin = false;
            int jointsAcc = -1, weightsAcc = -1;
            std::map<std::string, int>::const_iterator itJ = prim.attributes.find("JOINTS_0");
            if (itJ != prim.attributes.end()) jointsAcc = itJ->second;
            std::map<std::string, int>::const_iterator itW = prim.attributes.find("WEIGHTS_0");
            if (itW != prim.attributes.end()) weightsAcc = itW->second;
            hasSkin = (jointsAcc >= 0 && weightsAcc >= 0 && skinIndex >= 0);

            std::vector<float> jn; int jnComps = 0;
            std::vector<float> wt; int wtComps = 0;
            if (hasSkin) {
                std::vector<float> jtmp; int jc = 0; getAsFloat(model, jointsAcc, jtmp, jc);
                jn.resize(vertCount * 4);
                for (size_t i = 0; i < vertCount; ++i) {
                    for (int c = 0; c < 4; ++c) jn[i * 4 + c] = (jc ? jtmp[i * jc + c] : 0.f);
                }
                getAsFloat(model, weightsAcc, wt, wtComps);
                if (wtComps != 4) hasSkin = false;
            }

            if (hasSkin) {
                size_t jointCount = (skinIndex >= 0 && skinIndex < (int)model.skins.size())
                    ? model.skins[skinIndex].joints.size() : 0;
                for (size_t i = 0; i < vertCount; ++i) {
                    for (int c = 0; c < 4; ++c) {
                        int ji = (int)std::round(jn[i * 4 + c]);
                        if (ji < 0 || (jointCount > 0 && ji >= (int)jointCount)) ji = 0;
                        jn[i * 4 + c] = (float)ji;
                    }
                    float w0 = wt[i * 4 + 0]; if (w0 < 0.f) w0 = 0.f;
                    float w1 = wt[i * 4 + 1]; if (w1 < 0.f) w1 = 0.f;
                    float w2 = wt[i * 4 + 2]; if (w2 < 0.f) w2 = 0.f;
                    float w3 = wt[i * 4 + 3]; if (w3 < 0.f) w3 = 0.f;
                    float s = w0 + w1 + w2 + w3;
                    if (s > 1e-8f) { w0 /= s; w1 /= s; w2 /= s; w3 /= s; }
                    else { w0 = 1.f; w1 = w2 = w3 = 0.f; }
                    wt[i * 4 + 0] = w0; wt[i * 4 + 1] = w1; wt[i * 4 + 2] = w2; wt[i * 4 + 3] = w3;
                }
            }

            float factor[4] = { 1,1,1,1 };
            GLuint tex = 0;
            if (prim.material >= 0 && prim.material < (int)model.materials.size()) {
                const tinygltf::Material& m = model.materials[prim.material];
                if (m.pbrMetallicRoughness.baseColorFactor.size() == 4) {
                    for (int i = 0; i < 4; ++i) factor[i] = (float)m.pbrMetallicRoughness.baseColorFactor[i];
                }
                tex = GetMaterialBaseColorTexture(texForTextureIdx, model, prim.material);
            }

            size_t indexOffset = indices.size();
            indices.insert(indices.end(), localIdx.begin(), localIdx.end());

            size_t base = interleaved.size();
            interleaved.resize(base + vertCount * 13);
            float* dst = interleaved.data() + base;

            for (size_t i = 0; i < vertCount; ++i) {
                float x = pos[i * 3 + 0], y = pos[i * 3 + 1], z = pos[i * 3 + 2];

                float wx, wy, wz; xformPoint(WM, x, y, z, wx, wy, wz);
                if (wx < minX) minX = wx; if (wy < minY) minY = wy; if (wz < minZ) minZ = wz;
                if (wx > maxX) maxX = wx; if (wy > maxY) maxY = wy; if (wz > maxZ) maxZ = wz;

                float vx = x, vy = y, vz = z;
                if (!hasSkin) xformPoint(WM, x, y, z, vx, vy, vz);

                dst[0] = vx; dst[1] = vy; dst[2] = vz;
                dst[3] = (uvComps == 2) ? uv[i * 2 + 0] : 0.f;
                dst[4] = (uvComps == 2) ? uv[i * 2 + 1] : 0.f;

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

            GLTFDraw d;
            d.indexCount = (GLsizei)localIdx.size();
            d.indexOffset = (GLsizei)indexOffset;
            d.texture = tex;
            d.baseColor[0] = factor[0]; d.baseColor[1] = factor[1]; d.baseColor[2] = factor[2]; d.baseColor[3] = factor[3];
            d.skinned = hasSkin;
            d.skinIndex = hasSkin ? skinIndex : -1;
            d.localModel = hasSkin ? WM : matIdentity();

            if (hasSkin) {
                const tinygltf::Skin& skin = model.skins[skinIndex];
                int jointCount = (int)skin.joints.size();
                d.bones16.resize((size_t)jointCount * 16);
                d.boneCount = jointCount;

                std::vector<float> invBind; int ibComps = 0;
                if (skin.inverseBindMatrices >= 0) getAsFloat(model, skin.inverseBindMatrices, invBind, ibComps);
                for (int j = 0; j < jointCount; ++j) {
                    int jointNode = skin.joints[(size_t)j];
                    Mat4 G = globalXf[(size_t)jointNode];
                    Mat4 IB = matIdentity();
                    if ((int)invBind.size() >= (j + 1) * 16) {
                        for (int k = 0; k < 16; ++k) IB.m[k] = invBind[j * 16 + k];
                    }
                    Mat4 B = matMul(G, IB);
                    for (int k = 0; k < 16; ++k) d.bones16[(size_t)j * 16 + k] = B.m[k];
                }
            }
            gGLTFDraws.push_back(d);
        }
    }

    float cx = 0.5f * (minX + maxX);
    float cy = 0.5f * (minY + maxY);
    float cz = 0.5f * (minZ + maxZ);
    float height = std::max(0.0001f, (maxY - minY));
    gModelFitRadius = 0.5f * std::max(std::max((maxX - minX), (maxY - minY)), (maxZ - minZ));

    float s = (height > 0.0001f) ? (2.0f / height) : 1.0f;
    outPreXform = matMul(Mat4Translate(-cx, -cy, -cz), matScale(s, s, s));

    Mat4 axisFix = matRotateX(-1.5707963f);
    outPreXform = matMul(axisFix, outPreXform);

    if (gPlaceOnGround) outPreXform = matMul(Mat4Translate(0.f, 1.f, 0.f), outPreXform);

    float tx, ty, tz;
    xformPoint(outPreXform, cx, cy, cz, tx, ty, tz);
    gModelTarget[0] = tx;
    gModelTarget[1] = ty;
    gModelTarget[2] = tz;

    gModelFitRadius *= s;

    glGenVertexArrays(1, &outVAO);
    glBindVertexArray(outVAO);

    glGenBuffers(1, &outVBO);
    glBindBuffer(GL_ARRAY_BUFFER, outVBO);
    glBufferData(GL_ARRAY_BUFFER, interleaved.size() * sizeof(float), interleaved.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &outEBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, outEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint32_t), indices.data(), GL_STATIC_DRAW);

    const GLsizei stride = (GLsizei)(13 * sizeof(float));
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

// ============================================================
// Animation runtime API
const tinygltf::Model& GLTF_GetModel() { return gModelStatic; }

void normalizeQ(float q[4]) {
    float L = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (L > 1e-8f) { q[0] /= L; q[1] /= L; q[2] /= L; q[3] /= L; }
}

void sampleVec3(const std::vector<float>& times, const std::vector<float>& vals, float t, float out[3], bool step) {
    if (times.empty() || vals.empty()) { out[0] = out[1] = out[2] = 0.f; return; }
    if (t <= times.front()) { out[0] = vals[0]; out[1] = vals[1]; out[2] = vals[2]; return; }
    if (t >= times.back()) { size_t n = vals.size(); out[0] = vals[n - 3]; out[1] = vals[n - 2]; out[2] = vals[n - 1]; return; }
    size_t i = 1; while (i<times.size() && t>times[i]) ++i; size_t i0 = i - 1, i1 = i;
    float u = step ? 0.f : (t - times[i0]) / std::max(1e-6f, times[i1] - times[i0]);
    for (int c = 0; c < 3; ++c) { float a = vals[i0 * 3 + c], b = vals[i1 * 3 + c]; out[c] = a * (1.f - u) + b * u; }
}

void sampleQuat(const std::vector<float>& times, const std::vector<float>& vals, float t, float out[4], bool step) {
    if (times.empty() || vals.empty()) { out[0] = out[1] = out[2] = 0.f; out[3] = 1.f; return; }
    if (t <= times.front()) { out[0] = vals[0]; out[1] = vals[1]; out[2] = vals[2]; out[3] = vals[3]; normalizeQ(out); return; }
    if (t >= times.back()) { size_t n = vals.size(); out[0] = vals[n - 4]; out[1] = vals[n - 3]; out[2] = vals[n - 2]; out[3] = vals[n - 1]; normalizeQ(out); return; }
    size_t i = 1; while (i<times.size() && t>times[i]) ++i; size_t i0 = i - 1, i1 = i;
    if (step) { for (int c = 0; c < 4; ++c) out[c] = vals[i0 * 4 + c]; normalizeQ(out); return; }
    float q0[4] = { vals[i0 * 4 + 0], vals[i0 * 4 + 1], vals[i0 * 4 + 2], vals[i0 * 4 + 3] };
    float q1[4] = { vals[i1 * 4 + 0], vals[i1 * 4 + 1], vals[i1 * 4 + 2], vals[i1 * 4 + 3] };
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

static void slerpQ(const float a[4], const float bIn[4], float u, float out[4]) {
    float b[4] = { bIn[0], bIn[1], bIn[2], bIn[3] };
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0.f) { b[0] = -b[0]; b[1] = -b[1]; b[2] = -b[2]; b[3] = -b[3]; dot = -dot; }
    if (dot > 0.9995f) {
        out[0] = a[0] * (1.f - u) + b[0] * u;
        out[1] = a[1] * (1.f - u) + b[1] * u;
        out[2] = a[2] * (1.f - u) + b[2] * u;
        out[3] = a[3] * (1.f - u) + b[3] * u;
        normalizeQ(out);
        return;
    }
    float th = std::acos(dot);
    float s0 = std::sin((1.f - u) * th) / std::sin(th);
    float s1 = std::sin(u * th) / std::sin(th);
    out[0] = a[0] * s0 + b[0] * s1;
    out[1] = a[1] * s0 + b[1] * s1;
    out[2] = a[2] * s0 + b[2] * s1;
    out[3] = a[3] * s0 + b[3] * s1;
}

static void SampleAnimationPose(const tinygltf::Model& model, const GLTFAnimation& A, float tLocal, std::vector<NodeTRS>& out) {
    out = gBaseTRS;
    for (size_t ch = 0; ch < A.channels.size(); ++ch) {
        const AnimChannel& C = A.channels[ch];
        if (C.targetNode < 0) continue;
        const AnimSampler& S = A.samplers[(size_t)C.sampler];
        const tinygltf::Model& src = S.srcModel ? *S.srcModel : model;

        std::vector<float> times; int tComps = 0; getAsFloat(src, S.inputAcc, times, tComps);
        std::vector<float> vals;  int vComps = 0; getAsFloat(src, S.outputAcc, vals, vComps);
        bool step = (S.interp == "STEP");

        if (C.path == AP_Translation) {
            float v[3]; sampleVec3(times, vals, tLocal, v, step);
            out[(size_t)C.targetNode].T[0] = v[0]; out[(size_t)C.targetNode].T[1] = v[1]; out[(size_t)C.targetNode].T[2] = v[2];
        }
        else if (C.path == AP_Scale) {
            float v[3]; sampleVec3(times, vals, tLocal, v, step);
            out[(size_t)C.targetNode].S[0] = v[0]; out[(size_t)C.targetNode].S[1] = v[1]; out[(size_t)C.targetNode].S[2] = v[2];
        }
        else {
            float q[4]; sampleQuat(times, vals, tLocal, q, step);
            out[(size_t)C.targetNode].R[0] = q[0]; out[(size_t)C.targetNode].R[1] = q[1]; out[(size_t)C.targetNode].R[2] = q[2]; out[(size_t)C.targetNode].R[3] = q[3];
        }
    }
}

void GLTF_CrossfadeToAnimationByIndex(int idx, float nowSec, float durationSec, bool syncNormalizedPhase) {
    if (idx < 0 || idx >= (int)gAnims.size()) return;
    if (gActiveAnim == idx) return;

    int from = gActiveAnim;
    int to = idx;

    gBlendFrom = from;
    gBlendTo = to;
    gBlendStart = nowSec;
    gBlendDur = durationSec > 0.f ? durationSec : 0.f;
    gBlendFromT0 = gAnimT0;

    float toDur = gAnims[(size_t)to].durationSec;
    if (syncNormalizedPhase && from >= 0) {
        float fromDur = gAnims[(size_t)from].durationSec;
        float tFromLocal = (fromDur > 0.f) ? std::fmod(std::max(0.f, nowSec - gAnimT0), fromDur) : std::max(0.f, nowSec - gAnimT0);
        float phase = (fromDur > 1e-6f) ? (tFromLocal / fromDur) : 0.f;
        float tToLocal = (toDur > 1e-6f) ? (phase * toDur) : 0.f;
        gBlendToT0 = nowSec - tToLocal;
    }
    else {
        gBlendToT0 = nowSec;
    }

    gActiveAnim = to;
    gActiveDuration = gAnims[(size_t)to].durationSec;
    gAnimT0 = gBlendToT0;

    gBlendActive = (gBlendDur > 0.f) && (from >= 0) && (to >= 0) && (from != to);
}

void GLTF_UpdateAnimation_Pose(const tinygltf::Model& model, float tSec) {
    if ((gActiveAnim < 0) || gAnims.empty()) {
        ComputeGlobalTransforms(model, gGlobalsAnimated);
        return;
    }

    std::vector<NodeTRS> cur;

    if (gBlendActive) {
        float w = (gBlendDur > 1e-6f) ? (tSec - gBlendStart) / gBlendDur : 1.f;
        if (w < 0.f) w = 0.f; if (w > 1.f) w = 1.f;

        const GLTFAnimation& A0 = gAnims[(size_t)gBlendFrom];
        const GLTFAnimation& A1 = gAnims[(size_t)gBlendTo];

        float d0 = (A0.durationSec > 0.f) ? A0.durationSec : 0.f;
        float d1 = (A1.durationSec > 0.f) ? A1.durationSec : 0.f;

        float t0 = (d0 > 0.f) ? std::fmod(std::max(0.f, tSec - gBlendFromT0), d0) : std::max(0.f, tSec - gBlendFromT0);
        float t1 = (d1 > 0.f) ? std::fmod(std::max(0.f, tSec - gBlendToT0), d1) : std::max(0.f, tSec - gBlendToT0);

        std::vector<NodeTRS> pose0; SampleAnimationPose(model, A0, t0, pose0);
        std::vector<NodeTRS> pose1; SampleAnimationPose(model, A1, t1, pose1);

        cur = gBaseTRS;
        size_t N = cur.size();
        for (size_t i = 0; i < N; ++i) {
            // T
            cur[i].T[0] = pose0[i].T[0] * (1.f - w) + pose1[i].T[0] * w;
            cur[i].T[1] = pose0[i].T[1] * (1.f - w) + pose1[i].T[1] * w;
            cur[i].T[2] = pose0[i].T[2] * (1.f - w) + pose1[i].T[2] * w;
            // S
            cur[i].S[0] = pose0[i].S[0] * (1.f - w) + pose1[i].S[0] * w;
            cur[i].S[1] = pose0[i].S[1] * (1.f - w) + pose1[i].S[1] * w;
            cur[i].S[2] = pose0[i].S[2] * (1.f - w) + pose1[i].S[2] * w;
            // R
            float q[4]; slerpQ(pose0[i].R, pose1[i].R, w, q);
            cur[i].R[0] = q[0]; cur[i].R[1] = q[1]; cur[i].R[2] = q[2]; cur[i].R[3] = q[3];
        }

        if (w >= 1.f) {
            gBlendActive = false;
            gBlendFrom = -1; gBlendTo = -1;
            gBlendDur = 0.f;
        }
    }
    else {
        const GLTFAnimation& A = gAnims[(size_t)gActiveAnim];
        float dur = (A.durationSec > 0.f) ? A.durationSec : 0.f;
        float tLocal = (dur > 0.f) ? std::fmod(std::max(0.f, tSec - gAnimT0), dur) : std::max(0.f, tSec - gAnimT0);
        SampleAnimationPose(model, A, tLocal, cur);
    }

    gGlobalsAnimated.assign(model.nodes.size(), matIdentity());
    int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : (model.scenes.empty() ? -1 : 0);
    if (sceneIndex < 0) return;

    struct Item { int node; Mat4 parent; };
    std::vector<Item> st;
    for (size_t i = 0; i < model.scenes[sceneIndex].nodes.size(); ++i) {
        Item it; it.node = model.scenes[sceneIndex].nodes[i]; it.parent = matIdentity(); st.push_back(it);
    }
    while (!st.empty()) {
        Item it = st.back(); st.pop_back();
        const tinygltf::Node& n = model.nodes[(size_t)it.node];
        const NodeTRS& B = cur[(size_t)it.node];
        Mat4 M = matMul(Mat4Translate(B.T[0], B.T[1], B.T[2]),
            matMul(matFromQuat(B.R[0], B.R[1], B.R[2], B.R[3]),
                matScale(B.S[0], B.S[1], B.S[2])));
        Mat4 G = matMul(it.parent, M);
        gGlobalsAnimated[(size_t)it.node] = G;
        for (size_t c = 0; c < n.children.size(); ++c) {
            Item child; child.node = n.children[c]; child.parent = G; st.push_back(child);
        }
    }
}

void GLTF_GetBonesForDraw(const GLTFDraw& d, std::vector<float>& out16) {
    out16.clear();
    if (!d.skinned || d.skinIndex < 0 || d.boneCount <= 0) return;
    const GLTFSkin& S = gSkins[(size_t)d.skinIndex];
    out16.resize((size_t)d.boneCount * 16);
    for (int j = 0; j < d.boneCount; ++j) {
        int jointNode = S.joints[(size_t)j];
        Mat4 B = matMul(gGlobalsAnimated[(size_t)jointNode], S.invBind[(size_t)j]);
        for (int k = 0; k < 16; ++k) out16[(size_t)j * 16 + k] = B.m[k];
    }
}

// ============================================================
// Animation utility API
int GLTF_GetAnimationCount() { return (int)gAnims.size(); }

const char* GLTF_GetAnimationName(int i) {
    if (i < 0 || i >= (int)gAnims.size()) return "";
    return gAnims[(size_t)i].name.c_str();
}

int GLTF_FindAnimationIndexContaining(const char* needle) {
    if (!needle) return -1;
    std::string n = needle;
    for (size_t k = 0; k < n.size(); ++k) n[k] = (char)tolower((unsigned char)n[k]);
    for (size_t i = 0; i < gAnims.size(); ++i) {
        std::string s = gAnims[i].name;
        for (size_t k = 0; k < s.size(); ++k) s[k] = (char)tolower((unsigned char)s[k]);
        if (!s.empty() && s.find(n) != std::string::npos) return (int)i;
    }
    return -1;
}

void GLTF_SetActiveAnimationByIndex(int idx, float nowSec) {
    if (idx < 0 || idx >= (int)gAnims.size()) return;
    gActiveAnim = idx;
    gActiveDuration = gAnims[(size_t)idx].durationSec;
    gAnimT0 = nowSec;
}

int GLTF_GetActiveAnimationIndex() { return gActiveAnim; }

float GLTF_GetAnimationDuration(int idx) {
    if (idx < 0 || idx >= (int)gAnims.size()) return 0.f;
    return gAnims[(size_t)idx].durationSec;
}

std::string normName(const std::string& s) {
    std::string r; r.reserve(s.size());
    size_t start = 0;
    if (s.rfind("mixamorig:", 0) == 0) start = 10;
    else if (s.rfind("Armature|", 0) == 0) start = 9;
    for (size_t i = start; i < s.size(); ++i) r.push_back((char)tolower((unsigned char)s[i]));
    return r;
}

bool GLTF_AppendAnimationsFromFile(const char* path) {
    std::unique_ptr<tinygltf::Model> donor(new tinygltf::Model());
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    bool ok = false;
    std::string p(path);
    if (EndsWithNoCase(p, ".glb")) ok = loader.LoadBinaryFromFile(donor.get(), &err, &warn, p);
    else                           ok = loader.LoadASCIIFromFile(donor.get(), &err, &warn, p);
    if (!ok) return false;

    std::unordered_map<std::string, int> baseByName;
    for (int i = 0; i < (int)gModelStatic.nodes.size(); ++i) {
        const tinygltf::Node& n = gModelStatic.nodes[(size_t)i];
        if (!n.name.empty()) baseByName[normName(n.name)] = i;
    }

    int appended = 0;
    for (size_t ai = 0; ai < donor->animations.size(); ++ai) {
        const tinygltf::Animation& a = donor->animations[ai];
        GLTFAnimation A; A.name = a.name;
        A.samplers.resize(a.samplers.size());
        for (size_t si = 0; si < a.samplers.size(); ++si) {
            const tinygltf::AnimationSampler& s = a.samplers[si];
            A.samplers[si].inputAcc = s.input;
            A.samplers[si].outputAcc = s.output;
            A.samplers[si].interp = s.interpolation.empty() ? "LINEAR" : s.interpolation;
            if (s.output >= 0) {
                const tinygltf::Accessor& acc = donor->accessors[(size_t)s.output];
                A.samplers[si].comps = tinygltf::GetNumComponentsInType(acc.type);
            }
            float mt = AccessorMaxTime(*donor, s.input);
            if (mt > A.durationSec) A.durationSec = mt;
            A.samplers[si].srcModel = donor.get();
        }

        for (size_t ci = 0; ci < a.channels.size(); ++ci) {
            const tinygltf::AnimationChannel& c = a.channels[ci];
            int donorNode = c.target_node;
            if (donorNode < 0 || donorNode >= (int)donor->nodes.size()) continue;
            std::string dn = normName(donor->nodes[(size_t)donorNode].name);
            if (dn.empty()) continue;

            int baseNode = -1;
            std::unordered_map<std::string, int>::const_iterator it = baseByName.find(dn);
            if (it != baseByName.end()) baseNode = it->second;
            if (baseNode < 0) continue;

            AnimChannel C;
            C.sampler = c.sampler;
            C.targetNode = baseNode;
            if (c.target_path == "translation") C.path = AP_Translation;
            else if (c.target_path == "rotation") C.path = AP_Rotation;
            else C.path = AP_Scale;
            A.channels.push_back(C);
        }

        if (!A.channels.empty()) { gAnims.push_back(A); appended++; }
    }

    if (appended > 0) {
        gAnimSources.emplace_back(std::move(donor));
        return true;
    }
    return false;
}