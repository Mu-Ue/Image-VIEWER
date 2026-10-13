#include "platform/handle_stream.h"

#include <new>

namespace {

class HandleStream final : public IStream {
public:
    explicit HandleStream(HANDLE file) : file_(file) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(ISequentialStream) || iid == __uuidof(IStream)) {
            *out = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = ULONG(InterlockedDecrement(&refs_));
        if (refs == 0) delete this;
        return refs;
    }

    HRESULT STDMETHODCALLTYPE Read(void* data, ULONG size, ULONG* read) override {
        DWORD n = 0;
        const BOOL ok = ReadFile(file_, data, size, &n, nullptr);
        if (read) *read = n;
        return !ok ? HRESULT_FROM_WIN32(GetLastError()) : n < size ? S_FALSE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* data, ULONG size, ULONG* written) override {
        DWORD n = 0;
        const BOOL ok = WriteFile(file_, data, size, &n, nullptr);
        if (written) *written = n;
        return !ok ? HRESULT_FROM_WIN32(GetLastError()) : n < size ? STG_E_MEDIUMFULL : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* position) override {
        LARGE_INTEGER to;
        if (origin > STREAM_SEEK_END) return STG_E_INVALIDFUNCTION;  // the same values as FILE_BEGIN and so on
        if (!SetFilePointerEx(file_, move, &to, origin)) return HRESULT_FROM_WIN32(GetLastError());
        if (position) position->QuadPart = ULONGLONG(to.QuadPart);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override {
        FILE_END_OF_FILE_INFO end{.EndOfFile = {.QuadPart = LONGLONG(size.QuadPart)}};
        return SetFileInformationByHandle(file_, FileEndOfFileInfo, &end, sizeof(end))
                   ? S_OK
                   : HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD) override {
        if (!stat) return STG_E_INVALIDPOINTER;
        LARGE_INTEGER size;
        if (!GetFileSizeEx(file_, &size)) return HRESULT_FROM_WIN32(GetLastError());
        *stat = {};
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = ULONGLONG(size.QuadPart);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    ~HandleStream() { CloseHandle(file_); }

    HANDLE file_;
    LONG refs_ = 1;
};

}  // namespace

HRESULT CreateHandleStream(HANDLE file, Microsoft::WRL::ComPtr<IStream>& out) {
    IStream* stream = new (std::nothrow) HandleStream(file);
    if (!stream) return E_OUTOFMEMORY;
    out.Attach(stream);
    return S_OK;
}
