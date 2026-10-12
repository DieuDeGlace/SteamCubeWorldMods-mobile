#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "cwsdk.h"
#include "../NativeScene.h"
#include "WindRules.h"
#include <d3dcompiler.h>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdio>
#include <atomic>
#include <algorithm>
namespace motion {
template <class T> void drop(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}
static void log(const char *s) {
    FILE *f = nullptr;
    fopen_s(&f, "Mods\\WorldMotion.log", "a");
    if (f) {
        fprintf(f, "%s\n", s);
        fclose(f);
    }
}
using DI = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, INT);
using D = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT);
using Map = HRESULT(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP,
                                         UINT, D3D11_MAPPED_SUBRESOURCE *);
using Unmap = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
using Update = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT,
                                         const D3D11_BOX *, const void *, UINT, UINT);
static Map oldMap;
static Unmap oldUnmap;
static Update oldUpdate;
static const GUID cbGuid = {
    0x753be3a7, 0x2f69, 0x4b97, {0x87, 0xf8, 0x11, 0x12, 0x39, 0xef, 0x6e, 0x94}};
struct Mapped {
    void *data;
    UINT size;
};
static thread_local std::unordered_map<ID3D11Resource *, Mapped> mappedBuffers;
static DI oldDI = nullptr;
static D oldD = nullptr;
static void **table = nullptr;
static thread_local bool internal = false;
static bool enabled = true, leavesEnabled = true, cloudsEnabled = true;
static float leafAmplitude = .16f, leafRange = 96.f, cloudDistance = 64.f, windSpeed = 1.f,
             cloudSpeed = 1.25f;
static std::atomic<int> toggle{0};
static unsigned long long origin = 0, refresh = 0, lastLog = 0;
static unsigned built = 0, cloudDraws = 0, leafDraws = 0, leafVertices = 0;
static cube::World *currentWorld = nullptr;
static nativeScene::Chunks chunks;
static ID3D11Device *device = nullptr;
static ID3D11DeviceContext *context = nullptr;
static ID3D11InputLayout *floatLayout = nullptr;
static ID3D11Buffer *cloudMatrix = nullptr;
static ID3D11ComputeShader *leafCompute = nullptr;
static ID3D11Buffer *windConstants = nullptr;
struct WindParams {
    float draw[4], phase[4];
    unsigned count[4];
};
static const char *leafCode = R"(
cbuffer Wind:register(b0){float4 draw,phase;uint4 count;};
RWByteAddressBuffer vertices:register(u0);StructuredBuffer<uint> indices:register(t0);
[numthreads(64,1,1)]void main(uint3 id:SV_DispatchThreadID){if(id.x>=count.x)return;uint packed=indices[id.x];if(packed&0x80000000)return;uint address=(packed&0x7fffffff)*20;float2 xy=round(asfloat(vertices.Load2(address))*255);float fade=saturate((phase.z-length(draw.xy+xy))/32);float a=(packed&0x80000000)?0:draw.w*fade;float p=phase.x+dot(xy,float2(.035,.025)),d=phase.y+dot(xy,float2(.022,.014));float2 sway=float2(a*(.7*sin(draw.z*1.7+p)+.3*sin(draw.z*3.1+d)),a*.65*sin(draw.z*1.35+p+.8));vertices.Store2(address,asuint((xy+sway)/255));}
)";
struct Vertex {
    float p[4];
    unsigned char color[4];
};
static_assert(sizeof(Vertex) == 20);
struct Mesh {
    ID3D11Buffer *original = nullptr, *animated = nullptr, *indices = nullptr;
    ID3D11ShaderResourceView *indexView = nullptr;
    ID3D11UnorderedAccessView *vertexView = nullptr;
    std::vector<Vertex> vertices;
    std::vector<unsigned> leaves;
    size_t bytes = 0;
    UINT leafCount = 0;
    nativeScene::Chunk info;
    unsigned long long updated = 0, lastUsed = 0;
    bool published = false;
    ~Mesh() {
        if (original && published)
            original->SetPrivateDataInterface(nativeScene::animatedGuid, nullptr);
        drop(original);
        drop(animated);
        drop(indices);
        drop(indexView);
        drop(vertexView);
    }
};
static std::unordered_map<ID3D11Buffer *, std::unique_ptr<Mesh>> meshes;
static size_t cacheBytes = 0;
static constexpr size_t cacheLimit = 32 * 1024 * 1024;
struct Readback {
    ID3D11Buffer *source = nullptr, *staging = nullptr;
    UINT bytes = 0;
    unsigned long long started = 0;
    ~Readback() {
        drop(source);
        drop(staging);
    }
};
static std::unordered_map<ID3D11Buffer *, std::unique_ptr<Readback>> pending;
static std::vector<unsigned char> read(ID3D11DeviceContext *, ID3D11Buffer *);
static UINT constantSize(ID3D11Resource *r) {
    ID3D11Buffer *b = nullptr;
    if (FAILED(r->QueryInterface(__uuidof(ID3D11Buffer), (void **)&b)))
        return 0;
    D3D11_BUFFER_DESC d{};
    b->GetDesc(&d);
    b->Release();
    return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && d.ByteWidth <= 512 ? d.ByteWidth : 0;
}
static bool readConstant(ID3D11DeviceContext *, ID3D11Buffer *b, float *out) {
    if (!b)
        return false;
    UINT size = 64;
    return SUCCEEDED(b->GetPrivateData(cbGuid, &size, out)) && size == 64;
}
static std::vector<unsigned char> read(ID3D11DeviceContext *c, ID3D11Buffer *b) {
    std::vector<unsigned char> out;
    if (!b)
        return out;
    auto found = pending.find(b);
    if (found == pending.end()) {
        // Queue a copy, then let native rendering proceed. Never wait for its GPU completion.
        if (pending.size() >= 2)
            return out;
        D3D11_BUFFER_DESC desc{};
        b->GetDesc(&desc);
        if (desc.ByteWidth > 2097152)
            return out;
        auto job = std::make_unique<Readback>();
        job->bytes = desc.ByteWidth;
        job->started = GetTickCount64();
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        desc.StructureByteStride = 0;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &job->staging)))
            return out;
        job->source = b;
        b->AddRef();
        c->CopyResource(job->staging, b);
        pending.emplace(b, std::move(job));
        return out;
    }
    auto &job = *found->second;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = oldMap(c, job.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        return out;
    if (SUCCEEDED(hr)) {
        out.assign((unsigned char *)mapped.pData, (unsigned char *)mapped.pData + job.bytes);
        oldUnmap(c, job.staging, 0);
    }
    pending.erase(found);
    return out;
}
static bool initialize(ID3D11DeviceContext *c) {
    if (device)
        return context == c && floatLayout && cloudMatrix && leafCompute && windConstants;
    context = c;
    c->GetDevice(&device);
    context->AddRef();
    const char *hlsl =
        "struct V{float4 p:POSITION;float4 c:COLOR;};float4 main(V v):SV_POSITION{return v.p+v.c*0.00001;}";
    ID3DBlob *code = nullptr;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "WorldMotionLayout", nullptr, nullptr, "main",
                            "vs_4_0", 0, 0, &code, nullptr);
    D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    if (SUCCEEDED(hr))
        hr = device->CreateInputLayout(elements, 2, code->GetBufferPointer(), code->GetBufferSize(),
                                       &floatLayout);
    drop(code);
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 64;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.Usage = D3D11_USAGE_DEFAULT;
    if (SUCCEEDED(hr))
        hr = device->CreateBuffer(&desc, nullptr, &cloudMatrix);
    if (SUCCEEDED(hr))
        hr = D3DCompile(leafCode, strlen(leafCode), "LeafWind", nullptr, nullptr, "main", "cs_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, nullptr);
    if (SUCCEEDED(hr))
        hr = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                         &leafCompute);
    drop(code);
    desc.ByteWidth = sizeof(WindParams);
    if (SUCCEEDED(hr))
        hr = device->CreateBuffer(&desc, nullptr, &windConstants);
    log(SUCCEEDED(hr)
            ? "WorldMotion GPU leaf animation ready; original voxel shader and lighting retained."
            : "WorldMotion resource creation failed; native rendering retained.");
    return SUCCEEDED(hr);
}
static bool upload(Mesh &m) {
    if (m.leaves.empty())
        return true;
    m.bytes = m.vertices.size() * sizeof(Vertex);
    m.leafCount = (UINT)m.leaves.size();
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = (UINT)m.bytes;
    d.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_SUBRESOURCE_DATA data{m.vertices.data(), 0, 0};
    if (FAILED(device->CreateBuffer(&d, &data, &m.animated)))
        return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = (UINT)m.bytes / 4;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(device->CreateUnorderedAccessView(m.animated, &ud, &m.vertexView)))
        return false;
    d.ByteWidth = m.leafCount * sizeof(UINT);
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    d.StructureByteStride = sizeof(UINT);
    data.pSysMem = m.leaves.data();
    if (FAILED(device->CreateBuffer(&d, &data, &m.indices)))
        return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.NumElements = m.leafCount;
    if (FAILED(device->CreateShaderResourceView(m.indices, &sd, &m.indexView)))
        return false;
    m.vertices.clear();
    m.vertices.shrink_to_fit();
    m.leaves.clear();
    m.leaves.shrink_to_fit();
    return true;
}
static void maintain(cube::World *w, unsigned long long now) {
    if (currentWorld != w) {
        meshes.clear();
        pending.clear();
        cacheBytes = 0;
        chunks.clear();
        currentWorld = w;
        refresh = 0;
    }
    if (now - refresh < 500)
        return;
    refresh = now;
    built = 0;
    if (nativeScene::snapshot(w, chunks, leafRange + 64))
        for (auto it = meshes.begin(); it != meshes.end();) {
            if (!chunks.count(it->first) || now - it->second->lastUsed > 3000) {
                cacheBytes -= it->second->bytes;
                it = meshes.erase(it);
            } else
                ++it;
        }
    for (auto it = pending.begin(); it != pending.end();)
        if (!chunks.count(it->first) || now - it->second->started > 3000)
            it = pending.erase(it);
        else
            ++it;
    if (toggle.exchange(0) || (GetAsyncKeyState(VK_F8) & 1)) {
        enabled = !enabled;
        if (!enabled)
            for (auto &entry : meshes) {
                auto &m = *entry.second;
                if (m.published) {
                    m.original->SetPrivateDataInterface(nativeScene::animatedGuid, nullptr);
                    m.published = false;
                }
            }
        log(enabled ? "World motion enabled." : "World motion disabled.");
    }
    if (now - lastLog > 60000) {
        char s[256];
        sprintf_s(
            s,
            "Animated draws: clouds=%u foliage=%u; marked leaf vertices=%u; cached meshes=%zu, bytes=%zu",
            cloudDraws, leafDraws, leafVertices, meshes.size(), cacheBytes);
        log(s);
        lastLog = now;
    }
}
static Mesh *build(ID3D11DeviceContext *c, ID3D11Buffer *b, cube::World *w,
                   const nativeScene::Chunk &info) {
    if (built >= 2 || meshes.size() >= 64)
        return nullptr;
    auto bytes = read(c, b);
    if (bytes.empty() || bytes.size() % 32)
        return nullptr;
    size_t count = bytes.size() / 8;
    if (count * sizeof(Vertex) + cacheBytes > cacheLimit)
        return nullptr;
    built++;
    auto mesh = std::make_unique<Mesh>();
    mesh->info = info;
    {
        nativeScene::Locks locks(w);
        cube::Zone *zone = nullptr;
        if (!locks.held || !nativeScene::resident(w, info, b, zone))
            return nullptr;
        const int normal[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                  {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        for (size_t i = 0; i < count; i += 4) {
            int n = bytes[i * 8 + 3];
            if (n > 5)
                continue;
            float center[3]{};
            bool quad = true;
            for (int j = 0; j < 4; j++) {
                if (bytes[(i + j) * 8 + 3] != n)
                    quad = false;
                for (int axis = 0; axis < 3; axis++)
                    center[axis] += bytes[(i + j) * 8 + axis] * .25f;
            }
            if (!quad)
                continue;
            int x = (int)floorf(center[0] - normal[n][0] * .25f),
                y = (int)floorf(center[1] - normal[n][1] * .25f),
                z = info.z + (int)floorf(center[2] - normal[n][2] * .25f);
            int type = nativeScene::blockType(zone, x, y, z);
            if (wind::foliage(type))
                for (unsigned j = 0; j < 4; j++) {
                    int vx = bytes[(i + j) * 8], vy = bytes[(i + j) * 8 + 1],
                        vz = info.z + bytes[(i + j) * 8 + 2];
                    bool anchored = false;
                    // Every duplicate corner inspects the same eight touching voxels. Pin it
                    // when it meets a stationary surface, including across chunk boundaries.
                    for (int dx = -1; dx <= 0 && !anchored; dx++)
                        for (int dy = -1; dy <= 0 && !anchored; dy++) {
                            int lx = vx + dx, ly = vy + dy;
                            auto neighbor = zone;
                            if (lx < 0 || lx >= 64 || ly < 0 || ly >= 64) {
                                IntVector2 coords(info.zone.x + (lx < 0     ? -1
                                                                 : lx >= 64 ? 1
                                                                            : 0),
                                                  info.zone.y + (ly < 0     ? -1
                                                                 : ly >= 64 ? 1
                                                                            : 0));
                                auto found = w->zones.find(coords);
                                neighbor = found == w->zones.end() ? nullptr : found->second;
                                lx = (lx + 64) % 64;
                                ly = (ly + 64) % 64;
                            }
                            if (!neighbor) {
                                anchored = true;
                                break;
                            }
                            for (int dz = -1; dz <= 0; dz++)
                                if (wind::anchorMaterial(
                                        nativeScene::blockType(neighbor, lx, ly, vz + dz))) {
                                    anchored = true;
                                    break;
                                }
                        }
                    mesh->leaves.push_back(wind::windIndex((unsigned)i + j, anchored));
                }
        }
    }
    // Empty meshes are cached too, so bare ground is not repeatedly read back.
    if (!mesh->leaves.empty()) {
        mesh->vertices.resize(count);
        for (size_t i = 0; i < count; i++) {
            for (int j = 0; j < 4; j++)
                mesh->vertices[i].p[j] = bytes[i * 8 + j] / 255.f;
            memcpy(mesh->vertices[i].color, bytes.data() + i * 8 + 4, 4);
        }
        if (!upload(*mesh))
            return nullptr;
        leafVertices += mesh->leafCount;
    }
    mesh->original = b;
    b->AddRef();
    if (mesh->animated) {
        nativeScene::ChunkTag tag{b, info};
        mesh->animated->SetPrivateData(nativeScene::chunkTagGuid, sizeof(tag), &tag);
    }
    cacheBytes += mesh->bytes;
    auto result = mesh.get();
    meshes.emplace(b, std::move(mesh));
    return result;
}
static bool animate(Mesh &m, ID3D11DeviceContext *c, const float *world, unsigned long long now) {
    // All visible meshes use one quantized time so shared corners agree. CPU
    // uploads remain only 48 bytes per mesh; vertex work stays on the GPU.
    auto tick = (now - origin) / 33;
    if (m.updated == tick && m.published)
        return true;
    double t = wind::windTime(now - origin, windSpeed);
    double zx = double(m.info.zone.x) * 64, zy = double(m.info.zone.y) * 64;
    WindParams p{{world[3], world[7], (float)t, leafAmplitude},
                 {(float)std::fmod(zx * .035 + zy * .025, 6.283185307179586),
                  (float)std::fmod(zx * .022 + zy * .014, 6.283185307179586), leafRange, 0},
                 {m.leafCount, 0, 0, 0}};
    // Only a 48-byte parameter block changes on the CPU. The GPU updates marked
    // leaf vertices in place; there is no per-frame mesh copy or dynamic-buffer Map.
    ID3D11ComputeShader *saved = nullptr;
    ID3D11ClassInstance *classes[256]{};
    UINT number = 256;
    c->CSGetShader(&saved, classes, &number);
    ID3D11Buffer *cb = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    ID3D11UnorderedAccessView *uav = nullptr;
    c->CSGetConstantBuffers(0, 1, &cb);
    c->CSGetShaderResources(0, 1, &srv);
    c->CSGetUnorderedAccessViews(0, 1, &uav);
    oldUpdate(c, windConstants, 0, nullptr, &p, 0, 0);
    c->CSSetShader(leafCompute, nullptr, 0);
    c->CSSetConstantBuffers(0, 1, &windConstants);
    c->CSSetShaderResources(0, 1, &m.indexView);
    c->CSSetUnorderedAccessViews(0, 1, &m.vertexView, nullptr);
    c->Dispatch((m.leafCount + 63) / 64, 1, 1);
    ID3D11UnorderedAccessView *empty = nullptr;
    c->CSSetUnorderedAccessViews(0, 1, &empty, nullptr);
    c->CSSetShader(saved, classes, number);
    c->CSSetConstantBuffers(0, 1, &cb);
    c->CSSetShaderResources(0, 1, &srv);
    c->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    drop(saved);
    drop(cb);
    drop(srv);
    drop(uav);
    for (UINT i = 0; i < number; i++)
        drop(classes[i]);
    m.updated = tick;
    if (!m.published) {
        m.original->SetPrivateDataInterface(nativeScene::animatedGuid, m.animated);
        m.published = true;
    }
    return true;
}
struct Saved {
    ID3D11Buffer *vb = nullptr, *cb = nullptr;
    ID3D11InputLayout *layout = nullptr;
    UINT stride = 0, offset = 0;
    bool changed = false;
    ~Saved() {
        drop(vb);
        drop(cb);
        drop(layout);
    }
    void restore(ID3D11DeviceContext *c) {
        if (!changed)
            return;
        c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        c->IASetInputLayout(layout);
        c->VSSetConstantBuffers(2, 1, &cb);
    }
};
static unsigned long long animationFrameTime = 0;
static unsigned long long animationTime(unsigned long long now) {
    return animationFrameTime && animationFrameTime >= origin && animationFrameTime <= now &&
                   now - animationFrameTime < 1000
               ? animationFrameTime
               : now;
}
static void prepare(ID3D11DeviceContext *c, Saved &s) {
    if (internal || c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return;
    internal = true;
    auto g = nativeScene::game();
    auto w = g ? g->world : nullptr;
    auto now = GetTickCount64();
    maintain(w, now);
    if (!enabled || !w || !initialize(c)) {
        internal = false;
        return;
    }
    c->IAGetVertexBuffers(0, 1, &s.vb, &s.stride, &s.offset);
    if (!s.vb || s.stride != 8 || s.offset != 0) {
        internal = false;
        return;
    }
    ID3D11Buffer *buffers[6]{};
    c->VSGetConstantBuffers(0, 6, buffers);
    UINT expected[] = {64, 64, 64, 16, 320, 48};
    bool native = true;
    for (int i = 0; i < 6; i++) {
        D3D11_BUFFER_DESC desc{};
        if (buffers[i])
            buffers[i]->GetDesc(&desc);
        if (desc.ByteWidth != expected[i])
            native = false;
    }
    float world[16]{}, projection[16]{}, view[16]{};
    if (native)
        native = readConstant(c, buffers[2], world) && readConstant(c, buffers[0], projection) &&
                 readConstant(c, buffers[1], view);
    if (native) {
        s.cb = buffers[2];
        s.cb->AddRef();
    }
    for (auto &b : buffers)
        drop(b);
    if (!native || fabsf(projection[15]) > .5f ||
        fabsf(view[3]) + fabsf(view[7]) + fabsf(view[11]) < .01f) {
        internal = false;
        return;
    }
    bool isCloud = cloudsEnabled && nativeScene::cloud(w, s.vb);
    if (isCloud) {
        double t = (animationTime(now) - origin) * .001 * windSpeed * cloudSpeed;
        auto offset = wind::cloud(t, cloudDistance);
        world[3] += offset.x;
        world[7] += offset.y;
        float appliedDrift[2] = {offset.x, offset.y};
        cloudMatrix->SetPrivateData(nativeScene::cloudDriftGuid, sizeof(appliedDrift),
                                    appliedDrift);
        c->UpdateSubresource(cloudMatrix, 0, nullptr, world, 0, 0);
        c->IAGetInputLayout(&s.layout);
        c->VSSetConstantBuffers(2, 1, &cloudMatrix);
        s.changed = true;
        cloudDraws++;
    } else if (leavesEnabled && fabsf(world[0] - 1) < .001f && fabsf(world[5] - 1) < .001f &&
               fabsf(world[10] - 1) < .001f) {
        auto found = chunks.find(s.vb);
        if (found != chunks.end()) {
            auto cached = meshes.find(s.vb);
            auto m =
                cached == meshes.end() ? build(c, s.vb, w, found->second) : cached->second.get();
            if (m) {
                m->lastUsed = now;
                if (m->animated && animate(*m, c, world, animationTime(now))) {
                    c->IAGetInputLayout(&s.layout);
                    UINT stride = sizeof(Vertex), offset = 0;
                    c->IASetVertexBuffers(0, 1, &m->animated, &stride, &offset);
                    c->IASetInputLayout(floatLayout);
                    s.changed = true;
                    leafDraws++;
                }
            }
        }
    }
    internal = false;
}
static void STDMETHODCALLTYPE di(ID3D11DeviceContext *c, UINT n, UINT start, INT base) {
    Saved saved;
    prepare(c, saved);
    oldDI(c, n, start, base);
    saved.restore(c);
}
static void STDMETHODCALLTYPE draw(ID3D11DeviceContext *c, UINT n, UINT start) {
    Saved saved;
    prepare(c, saved);
    oldD(c, n, start);
    saved.restore(c);
}
static HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub,
                                     D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *out) {
    HRESULT hr = oldMap(c, r, sub, type, flags, out);
    if (SUCCEEDED(hr) && sub == 0 && type != D3D11_MAP_READ) {
        UINT size = constantSize(r);
        if (size)
            mappedBuffers[r] = {out->pData, size};
    }
    return hr;
}
static void STDMETHODCALLTYPE unmap(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub) {
    auto it = mappedBuffers.find(r);
    if (it != mappedBuffers.end()) {
        r->SetPrivateData(cbGuid, it->second.size, it->second.data);
        mappedBuffers.erase(it);
    }
    oldUnmap(c, r, sub);
}
static void STDMETHODCALLTYPE update(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub,
                                     const D3D11_BOX *box, const void *data, UINT row, UINT slice) {
    if (!box && sub == 0 && data) {
        UINT size = constantSize(r);
        if (size)
            r->SetPrivateData(cbGuid, size, data);
    }
    oldUpdate(c, r, sub, box, data, row, slice);
}
static bool patch(void **slot, void *fn) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
        return false;
    InterlockedExchangePointer(slot, fn);
    DWORD unused;
    VirtualProtect(slot, sizeof(void *), old, &unused);
    return true;
}
static DWORD attach() {
    if (table)
        return 1;
    if (!nativeScene::supported()) {
        log("Unsupported executable; WorldMotion disabled.");
        return 0;
    }
    auto integer = [](const wchar_t *key, int value) {
        return (int)GetPrivateProfileIntW(L"Motion", key, value, L".\\Mods\\WorldMotion.ini");
    };
    enabled = integer(L"Enabled", 1) != 0;
    cloudsEnabled = integer(L"Clouds", 1) != 0;
    leavesEnabled = integer(L"Leaves", 1) != 0;
    leafAmplitude = std::clamp(integer(L"LeafAmplitudePercent", 16), 0, 35) * .01f;
    leafRange = (float)std::clamp(integer(L"LeafRange", 96), 32, 192);
    cloudDistance = (float)std::clamp(integer(L"CloudDriftDistance", 64), 0, 128);
    windSpeed = std::clamp(integer(L"WindSpeedPercent", 100), 10, 300) * .01f;
    cloudSpeed = std::clamp(integer(L"CloudSpeedPercent", 125), 10, 300) * .01f;
    ID3D11Device *d = nullptr;
    ID3D11DeviceContext *c = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &d, nullptr, &c)))
        return 0;
    auto v = *(void ***)c;
    struct Hook {
        UINT slot;
        void *fn;
        void **old;
    };
    Hook hooks[] = {{12, (void *)di, (void **)&oldDI},
                    {13, (void *)draw, (void **)&oldD},
                    {14, (void *)map, (void **)&oldMap},
                    {15, (void *)unmap, (void **)&oldUnmap},
                    {48, (void *)update, (void **)&oldUpdate}};
    for (auto &h : hooks)
        *h.old = v[h.slot];
    bool ok = true;
    int done = 0;
    for (auto &h : hooks) {
        if (!patch(v + h.slot, h.fn)) {
            ok = false;
            while (done--) {
                auto &undo = hooks[done];
                patch(v + undo.slot, *undo.old);
            }
            break;
        }
        done++;
    }
    if (ok) {
        table = v;
        origin = GetTickCount64();
        log("WorldMotion attached, loader API 7.3; GPU leaf compute, nonblocking initial readback, CPU-only constant cache; F8 toggles motion.");
    }
    drop(c);
    drop(d);
    return ok ? 1 : 0;
}
} // namespace motion
class WorldMotion : public GenericMod {
  public:
    void Initialize() override {
        motion::attach();
    }
};
static_assert(sizeof(GenericMod) == 0x428, "Loader API 7.3 required");
EXPORT GenericMod *MakeMod() {
    return new WorldMotion();
}
EXPORT void ModPreInitialize() {}
EXPORT int ModMajorVersion() {
    return 7;
}
EXPORT int ModMinorVersion() {
    return 3;
}
extern "C" __declspec(dllexport) DWORD WINAPI AttachWorldMotion(void *) {
    return motion::attach();
}
extern "C" __declspec(dllexport) DWORD WINAPI ToggleWorldMotion(void *) {
    motion::toggle = 1;
    return 1;
}

// Shared drift for cloud shadow casters; no mesh readback or cross-mod hook.
extern "C" __declspec(dllexport) BOOL WINAPI QueryCloudDrift(float *out) {
    if (!out)
        return FALSE;
    out[0] = out[1] = 0;
    if (motion::enabled && motion::cloudsEnabled && motion::origin) {
        auto offset = wind::cloud((motion::animationTime(GetTickCount64()) - motion::origin) *
                                      .001 * motion::windSpeed * motion::cloudSpeed,
                                  motion::cloudDistance);
        out[0] = offset.x;
        out[1] = offset.y;
    }
    return TRUE;
}

// Called at native depth clears by the experimental shadow mod. No vertex work.
extern "C" __declspec(dllexport) void WINAPI BeginWorldMotionFrame() {
    motion::animationFrameTime = GetTickCount64();
}
