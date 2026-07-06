#include "file_dialog.h"

#include <ShObjIdl.h>
#include <objbase.h>
#include <commdlg.h>
#include <windows.h>
#include <cwchar>
#include <vector>
#include <string>

namespace {
void CenterDialogOnOwner(HWND owner, HWND dialog) {
    if (!dialog)
        return;
    RECT rcOwner{};
    RECT rcDlg{};
    bool haveOwnerRect = owner && GetWindowRect(owner, &rcOwner);
    if (!GetWindowRect(dialog, &rcDlg))
        return;
    int dlgW = rcDlg.right - rcDlg.left;
    int dlgH = rcDlg.bottom - rcDlg.top;
    int x = 0;
    int y = 0;
    if (haveOwnerRect) {
        x = rcOwner.left + ((rcOwner.right - rcOwner.left) - dlgW) / 2;
        y = rcOwner.top + ((rcOwner.bottom - rcOwner.top) - dlgH) / 2;
    } else {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        HMONITOR mon = MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST);
        if (!GetMonitorInfo(mon, &mi))
            return;
        RECT rcWork = mi.rcWork;
        x = rcWork.left + ((rcWork.right - rcWork.left) - dlgW) / 2;
        y = rcWork.top + ((rcWork.bottom - rcWork.top) - dlgH) / 2;
    }
    SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}

class FileDialogEvents : public IFileDialogEvents {
public:
    explicit FileDialogEvents(HWND owner) : ref_(1), owner_(owner) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IFileDialogEvents) {
            *ppvObject = static_cast<IFileDialogEvents*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&ref_);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG res = InterlockedDecrement(&ref_);
        if (res == 0)
            delete this;
        return res;
    }

    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* pfd) override {
        CenterOnce(pfd);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog* pfd, IShellItem*) override {
        CenterOnce(pfd);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog* pfd) override {
        CenterOnce(pfd);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog* pfd) override {
        CenterOnce(pfd);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* pResponse) override {
        if (pResponse)
            *pResponse = FDESVR_DEFAULT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog* pfd) override {
        CenterOnce(pfd);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* pResponse) override {
        if (pResponse)
            *pResponse = FDEOR_DEFAULT;
        return S_OK;
    }

private:
    void CenterOnce(IFileDialog* pfd) {
        if (centered_ || !pfd)
            return;
        HWND hwnd = nullptr;
        IOleWindow* ole = nullptr;
        if (SUCCEEDED(pfd->QueryInterface(IID_PPV_ARGS(&ole))) && ole) {
            if (SUCCEEDED(ole->GetWindow(&hwnd)) && hwnd) {
                CenterDialogOnOwner(owner_, hwnd);
                centered_ = true;
            }
            ole->Release();
        }
    }

    LONG ref_;
    HWND owner_;
    bool centered_ = false;
};
} // namespace

static std::vector<std::wstring> runOpenDialog(HWND owner, const wchar_t* filter,
                                               const wchar_t* title, bool multi) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool comOk = (hrCo == S_OK || hrCo == S_FALSE);
    std::vector<std::wstring> result;
    if (comOk) {
        IFileOpenDialog* dlg = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
        if (SUCCEEDED(hr) && dlg) {
            FileDialogEvents* events = new FileDialogEvents(owner);
            DWORD cookie = 0;
            if (events)
                dlg->Advise(events, &cookie);
            if (title && *title)
                dlg->SetTitle(title);
            // Parse every filter string first, THEN take c_str() pointers:
            // growing `owned` after capturing a pointer would dangle it on a
            // vector reallocation.
            std::vector<std::wstring> owned;
            if (filter) {
                const wchar_t* p = filter;
                while (*p) {
                    const wchar_t* name = p;
                    p += wcslen(p) + 1;
                    if (!*p)
                        break;
                    const wchar_t* pattern = p;
                    p += wcslen(p) + 1;
                    owned.emplace_back(name);
                    owned.emplace_back(pattern);
                }
            }
            std::vector<COMDLG_FILTERSPEC> specs;
            specs.reserve(owned.size() / 2);
            for (size_t i = 0; i + 1 < owned.size(); i += 2) {
                COMDLG_FILTERSPEC spec{};
                spec.pszName = owned[i].c_str();
                spec.pszSpec = owned[i + 1].c_str();
                specs.push_back(spec);
            }
            if (!specs.empty())
                dlg->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
            DWORD opts = 0;
            dlg->GetOptions(&opts);
            opts |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
            if (multi)
                opts |= FOS_ALLOWMULTISELECT;
            dlg->SetOptions(opts);
            hr = dlg->Show(owner);
            if (SUCCEEDED(hr)) {
                IShellItemArray* items = nullptr;
                if (SUCCEEDED(dlg->GetResults(&items)) && items) {
                    DWORD count = 0;
                    items->GetCount(&count);
                    for (DWORD i = 0; i < count; ++i) {
                        IShellItem* item = nullptr;
                        if (SUCCEEDED(items->GetItemAt(i, &item)) && item) {
                            PWSTR path = nullptr;
                            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                                result.emplace_back(path);
                                CoTaskMemFree(path);
                            }
                            item->Release();
                        }
                    }
                    items->Release();
                }
            }
            if (cookie != 0)
                dlg->Unadvise(cookie);
            if (events)
                events->Release();
            dlg->Release();
        }
        if (hrCo == S_OK)
            CoUninitialize();
    }
    return result;
}

std::wstring openFileDialog(HWND owner, const wchar_t* filter, const wchar_t* title) {
    const auto paths = runOpenDialog(owner, filter, title, false);
    return paths.empty() ? std::wstring() : paths.front();
}

std::vector<std::wstring> openFileDialogMulti(HWND owner, const wchar_t* filter,
                                              const wchar_t* title) {
    return runOpenDialog(owner, filter, title, true);
}
