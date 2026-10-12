#include "WorldMotion.cpp"
#include <cassert>
static HRESULT STDMETHODCALLTYPE checkedMap(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub,
                                            D3D11_MAP mode, UINT flags,
                                            D3D11_MAPPED_SUBRESOURCE *out) {
    assert(mode == D3D11_MAP_READ);
    assert(flags == D3D11_MAP_FLAG_DO_NOT_WAIT);
    return c->Map(r, sub, mode, flags, out);
}
static void STDMETHODCALLTYPE directUnmap(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub) {
    c->Unmap(r, sub);
}
int main() {
    ID3D11Device *d = nullptr;
    ID3D11DeviceContext *c = nullptr;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &d, nullptr, &c)));
    motion::device = d;
    motion::oldMap = checkedMap;
    motion::oldUnmap = directUnmap;
    unsigned char expected[4096];
    for (size_t i = 0; i < sizeof(expected); i++)
        expected[i] = (unsigned char)(i * 37);
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(expected);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{expected, 0, 0};
    ID3D11Buffer *b[3]{};
    for (auto &buffer : b)
        assert(SUCCEEDED(d->CreateBuffer(&desc, &initial, &buffer)));
    assert(motion::read(c, b[0]).empty());
    assert(motion::pending.size() == 1);
    assert(motion::read(c, b[1]).empty());
    assert(motion::pending.size() == 2);
    assert(motion::read(c, b[2]).empty());
    assert(motion::pending.size() == 2);
    c->Flush();
    std::vector<unsigned char> actual;
    ULONGLONG start = GetTickCount64();
    while (actual.empty() && GetTickCount64() - start < 2000) {
        actual = motion::read(c, b[0]);
        if (actual.empty())
            Sleep(1);
    }
    assert(actual.size() == sizeof(expected));
    assert(memcmp(actual.data(), expected, sizeof(expected)) == 0);
    desc.ByteWidth = 64;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer *constant = nullptr;
    assert(SUCCEEDED(d->CreateBuffer(&desc, nullptr, &constant)));
    float matrix[16];
    auto queued = motion::pending.size();
    assert(!motion::readConstant(c, constant, matrix));
    assert(motion::pending.size() == queued);
    float cached[16]{};
    cached[0] = 42;
    constant->SetPrivateData(motion::cbGuid, 64, cached);
    assert(motion::readConstant(c, constant, matrix) && matrix[0] == 42);
    motion::pending.clear();
    for (auto &buffer : b)
        motion::drop(buffer);
    motion::drop(constant);
    motion::device = nullptr;
    motion::drop(c);
    motion::drop(d);
    puts(
        "Nonblocking readback, two-job bound, byte equivalence and zero-readback constant cache: PASS");
}
