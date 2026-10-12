#include "WorldMotion.cpp"
#include <cassert>
static void STDMETHODCALLTYPE updateDirect(ID3D11DeviceContext *c, ID3D11Resource *r, UINT s,
                                           const D3D11_BOX *box, const void *data, UINT row,
                                           UINT slice) {
    c->UpdateSubresource(r, s, box, data, row, slice);
}
int main() {
    ID3D11Device *d = nullptr;
    ID3D11DeviceContext *c = nullptr;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &d, nullptr, &c)));
    motion::oldUpdate = updateDirect;
    assert(motion::initialize(c));
    motion::Mesh m;
    m.info = {IntVector2(-80361, -13257), 64};
    std::vector<motion::Vertex> source(128);
    for (unsigned i = 0; i < source.size(); i++) {
        source[i] = {{float(i % 64) / 255, float(i % 37) / 255, .25f, 4.f / 255}, {1, 2, 3, 4}};
        if (i % 3 == 0)
            m.leaves.push_back(wind::windIndex(i, i % 6 == 0));
    }
    m.vertices = source;
    assert(motion::upload(m));
    assert(m.vertices.empty() && m.leaves.empty());
    D3D11_BUFFER_DESC originalDesc{};
    originalDesc.ByteWidth = 64;
    originalDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    assert(SUCCEEDED(d->CreateBuffer(&originalDesc, nullptr, &m.original)));
    ULONGLONG now = GetTickCount64();
    motion::origin = now - 1000;
    float world[16]{};
    world[3] = -16;
    world[7] = -12;
    assert(motion::animate(m, c, world, now));
    ID3D11UnorderedAccessView *active = nullptr;
    c->CSGetUnorderedAccessViews(0, 1, &active);
    assert(!active);
    ID3D11ComputeShader *shader = nullptr;
    c->CSGetShader(&shader, nullptr, nullptr);
    assert(!shader);
    D3D11_BUFFER_DESC desc{};
    m.animated->GetDesc(&desc);
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;
    ID3D11Buffer *staging = nullptr;
    assert(SUCCEEDED(d->CreateBuffer(&desc, nullptr, &staging)));
    c->CopyResource(staging, m.animated);
    D3D11_MAPPED_SUBRESOURCE read{};
    assert(SUCCEEDED(c->Map(staging, 0, D3D11_MAP_READ, 0, &read)));
    auto result = (const motion::Vertex *)read.pData;
    for (unsigned i = 0; i < source.size(); i++) {
        assert(memcmp(source[i].color, result[i].color, 4) == 0);
        assert(source[i].p[2] == result[i].p[2] && source[i].p[3] == result[i].p[3]);
        if (i % 3 || i % 6 == 0) {
            assert(memcmp(source[i].p, result[i].p, 16) == 0);
            continue;
        }
        double x = i % 64, y = i % 37;
        float fade = (float)std::clamp((96 - std::hypot(world[3] + x, world[7] + y)) / 32., 0., 1.);
        auto offset = wind::leaf(double(m.info.zone.x) * 64 + x, double(m.info.zone.y) * 64 + y,
                                 wind::windTime(1000, 1), .16f, fade);
        assert(std::abs(result[i].p[0] - (x + offset.x) / 255) < .000002 &&
               std::abs(result[i].p[1] - (y + offset.y) / 255) < .000002);
    }
    c->Unmap(staging, 0);
    motion::drop(staging);
    motion::drop(c);
    motion::drop(d);
    puts(
        "GPU wind matches CPU reference; non-leaves/colors/normals unchanged; CS state restored: PASS");
}
