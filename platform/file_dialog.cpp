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

std::wstring openFileDialog(HWND owner, const wchar_t* filter, const wchar_t* title) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool comOk = (hrCo == S_OK || hrCo == S_FALSE);
    std::wstring result;
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
            std::vector<std::wstring> owned;
            std::vector<COMDLG_FILTERSPEC> specs;
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
                    COMDLG_FILTERSPEC spec{};
                    spec.pszName = owned[owned.size() - 2].c_str();
                    spec.pszSpec = owned[owned.size() - 1].c_str();
                    specs.push_back(spec);
                }
            }
            if (!specs.empty())
                dlg->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
            DWORD opts = 0;
            dlg->GetOptions(&opts);
            opts |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
            dlg->SetOptions(opts);
            hr = dlg->Show(owner);
            if (SUCCEEDED(hr)) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                        result = path;
                        CoTaskMemFree(path);
                    }
                    item->Release();
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
