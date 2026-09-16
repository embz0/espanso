// Windows search surface: DirectWrite colour glyphs on a premultiplied
// DirectComposition swap chain. Kept separate from the cross-platform popup.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include "../common/common.h"
#include "../interop/interop.h"
#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/dcclient.h>
#include <algorithm>
#include <memory>
#include <vector>
#include <string>
#include <climits>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")

using Microsoft::WRL::ComPtr;
typedef void (*QueryCallback)(const char *, void *, void *);
typedef void (*ResultCallback)(const char *, void *);

namespace glass_search {
struct Item { wxString id, label, trigger, key; long uses = 0; };

// IDs can change when Espanso reloads its configuration. Use the actual label
// and trigger as a stable, collision-free, path-safe history key instead.
wxString HistoryKey(const wxString &label, const wxString &trigger) {
    wxString value = label + wxString(wxChar(0x1f)) + trigger;
    const auto bytes = value.ToUTF8();
    wxString key = "usage/";
    for (size_t n = 0; n < bytes.length(); ++n)
        key += wxString::Format("%02x", static_cast<unsigned char>(bytes[n]));
    return key;
}

class Frame : public wxFrame {
    QueryCallback query;
    ResultCallback result;
    void *queryData, *resultData;
    wxTextCtrl *input = nullptr;
    std::unique_ptr<wxFileConfig> history;
    std::vector<Item> items;
    int selected = 0, first = 0;
    bool ready = false, backdrop = false, highContrast = false;
    ComPtr<ID3D11Device> gpu;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> ctx;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<IDCompositionDevice> composition;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDWriteFactory> write;
    ComPtr<IDWriteTextFormat> body, captionFormat, title;
    ComPtr<ID2D1SolidColorBrush> brush;

    float Width() const { return float(GetClientSize().x) / GetDPIScaleFactor(); }
    float Height() const { return float(GetClientSize().y) / GetDPIScaleFactor(); }
    int Visible() const { return (std::max)(1, int((Height() - 148) / 58)); }
    void KeepVisible() {
        selected = (std::max)(0, (std::min)(selected, int(items.size()) - 1));
        if (selected < first) first = selected;
        if (selected >= first + Visible()) first = selected - Visible() + 1;
    }
    void LayoutInput() {
        const double scale = GetDPIScaleFactor();
        input->SetSize(int(60 * scale), int(56 * scale),
                       (std::max)(40, GetClientSize().x - int(90 * scale)), int(32 * scale));
    }
    bool Graphics() {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            flags, nullptr, 0, D3D11_SDK_VERSION, &gpu, nullptr, nullptr);
        if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            flags, nullptr, 0, D3D11_SDK_VERSION, &gpu, nullptr, nullptr);
        if (FAILED(hr)) return false;
        ComPtr<IDXGIDevice> dxgi;
        if (FAILED(gpu.As(&dxgi))) return false;
        ComPtr<ID2D1Factory1> factory;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1), nullptr, reinterpret_cast<void **>(factory.GetAddressOf())))) return false;
        if (FAILED(factory->CreateDevice(dxgi.Get(), &device)) ||
            FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &ctx))) return false;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> dxgiFactory;
        if (FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory)))) return false;
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = (std::max)(1, GetClientSize().x);
        desc.Height = (std::max)(1, GetClientSize().y);
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(dxgiFactory->CreateSwapChainForComposition(gpu.Get(), &desc, nullptr, &swap))) return false;
        if (FAILED(DCompositionCreateDevice(dxgi.Get(), __uuidof(IDCompositionDevice),
            reinterpret_cast<void **>(composition.GetAddressOf())))) return false;
        if (FAILED(composition->CreateTargetForHwnd(reinterpret_cast<HWND>(GetHandle()), FALSE, &target)) ||
            FAILED(composition->CreateVisual(&visual)) || FAILED(visual->SetContent(swap.Get())) ||
            FAILED(target->SetRoot(visual.Get())) || FAILED(composition->Commit())) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown **>(write.GetAddressOf())))) return false;
        if (FAILED(write->CreateTextFormat(L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16, L"", &body)) ||
            FAILED(write->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12, L"", &captionFormat)) ||
            FAILED(write->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13, L"", &title))) return false;
        body->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        captionFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (FAILED(ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush))) return false;
        return BindBuffer();
    }
    bool BindBuffer() {
        ComPtr<IDXGISurface> surface;
        if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
        ComPtr<ID2D1Bitmap1> bitmap;
        const float dpi = float(96 * GetDPIScaleFactor());
        auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
        if (FAILED(ctx->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &bitmap))) return false;
        ctx->SetTarget(bitmap.Get());
        ctx->SetDpi(dpi, dpi);
        ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        return true;
    }
    void Color(float r, float g, float b, float a = 1) { brush->SetColor(D2D1::ColorF(r,g,b,a)); }
    void Round(float x, float y, float w, float h, float radius) {
        auto rect = D2D1::RoundedRect(D2D1::RectF(x,y,x+w,y+h),radius,radius);
        ctx->FillRoundedRectangle(rect, brush.Get());
    }
    void Text(const wxString &value, IDWriteTextFormat *font, float x, float y, float w, float h) {
        // DirectWrite performs Unicode shaping/font fallback, including ZWJ and
        // skin-tone sequences. Explicitly opt into the font's colour layers.
        ctx->DrawText(value.wc_str(), UINT32(value.length()), font,
            D2D1::RectF(x,y,x+w,y+h), brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void Draw() {
        if (!ready) return;
        const float w = Width(), h = Height();
        ctx->BeginDraw();
        ctx->Clear(D2D1::ColorF(0,0,0,0));
        // Real system backdrop is visible through this translucent tint.
        Color(.055f,.065f,.095f,backdrop ? .50f : 1.f); Round(0,0,w,h,18);
        Color(1,1,1,.18f);
        ctx->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(.5f,.5f,w-.5f,h-.5f),18,18),brush.Get(),1);
        Color(.77f,.81f,.92f); Text("ESPANSO",title.Get(),22,16,160,20);
        Color(.58f,.64f,.76f); Text("SEARCH",captionFormat.Get(),w-78,18,60,18);
        Color(.105f,.12f,.16f); Round(16,44,w-32,54,12);
        Color(.62f,.69f,.84f);
        ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(36,68),6,6),brush.Get(),1.7f);
        ctx->DrawLine(D2D1::Point2F(40,73),D2D1::Point2F(46,79),brush.Get(),1.7f);
        Color(.62f,.68f,.79f);
        Text(input->IsEmpty() ? "YOUR MOST USED" : "MATCHING ENTRIES",captionFormat.Get(),22,110,w-44,18);
        const int limit = (std::min)(int(items.size()),first+Visible());
        for (int i = first; i < limit; ++i) {
            const float y = 136 + (i-first)*58.f;
            if (i == selected) {
                Color(.44f,.59f,1.f,.24f); Round(12,y-4,w-24,54,11);
                Color(.54f,.68f,1.f,.9f); Round(13,y+9,3,22,1.5f);
            }
            Color(.95f,.96f,1.f);
            Text(items[i].label,body.Get(),26,y+1,w-115,24);
            Color(.64f,.70f,.82f);
            wxString subtitle = items[i].trigger;
            if (items[i].uses > 0) subtitle += (subtitle.empty() ? "" : "   ·   ") + wxString::Format("Used %ld",items[i].uses);
            Text(subtitle,captionFormat.Get(),26,y+28,w-115,17);
            if (i < 9) Text(wxString::Format("Alt+%d",i+1),captionFormat.Get(),w-67,y+13,50,20);
        }
        if (items.empty()) {
            Color(.80f,.84f,.92f); Text("No matching entries",body.Get(),26,155,w-52,26);
            Color(.61f,.67f,.78f); Text("Try a different word or trigger.",captionFormat.Get(),26,187,w-52,20);
        }
        if (int(items.size()) > Visible()) {
            const float track = (std::max)(20.f,h-174);
            const float thumb = (std::max)(20.f,track*Visible()/items.size());
            const float top = 136+(track-thumb)*first/(std::max)(1,int(items.size())-Visible());
            Color(1,1,1,.25f); Round(w-7,top,3,thumb,1.5f);
        }
        Color(.60f,.66f,.77f); Text("↑ ↓  Navigate     Enter  Insert     Esc  Close",captionFormat.Get(),22,h-25,w-44,18);
        HRESULT hr = ctx->EndDraw();
        if (SUCCEEDED(hr)) hr = swap->Present(1,0);
        if (FAILED(hr)) { ready = false; Close(); }
    }
    void Submit() {
        if (items.empty()) return;
        Item item = items[selected];
        history->Write(item.key, (std::min)(LONG_MAX-1,item.uses)+1);
        history->Flush();
        if (result) result(item.id.ToUTF8(),resultData);
        Close();
    }
    void Key(wxKeyEvent &event) {
        const int key = event.GetKeyCode();
        if (key == WXK_ESCAPE) { Close(); return; }
        if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) { Submit(); return; }
        if (event.AltDown() && key >= '1' && key <= '9') {
            const int n = key-'1';
            if (n < int(items.size())) { selected=n; Submit(); }
            return;
        }
        int delta = 0;
        if (key == WXK_DOWN || (event.ControlDown() && key == 'N')) delta = 1;
        if (key == WXK_UP || (event.ControlDown() && key == 'P')) delta = -1;
        if (key == WXK_TAB) delta = event.ShiftDown() ? -1 : 1;
        if (key == WXK_PAGEDOWN) delta = Visible();
        if (key == WXK_PAGEUP) delta = -Visible();
        if (delta && !items.empty()) {
            const int count = int(items.size());
            selected = ((selected + delta) % count + count) % count;
            KeepVisible(); Draw(); return;
        }
        event.Skip();
    }
public:
    Frame(SearchMetadata *metadata, QueryCallback q, void *qd, ResultCallback r, void *rd)
        : wxFrame(nullptr,wxID_ANY,wxString::FromUTF8(metadata->windowTitle),wxDefaultPosition,
                  wxDefaultSize,wxSTAY_ON_TOP|wxFRAME_TOOL_WINDOW|wxBORDER_NONE),
          query(q),result(r),queryData(qd),resultData(rd) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetClientSize(FromDIP(wxSize(620,526)));
        CentreOnScreen();
        wxString directory=wxStandardPaths::Get().GetUserLocalDataDir();
        wxFileName::Mkdir(directory,wxS_DIR_DEFAULT,wxPATH_MKDIR_FULL);
        history.reset(new wxFileConfig("espanso-search",wxEmptyString,
            directory+wxFILE_SEP_PATH+"search-usage-v2.ini",wxEmptyString,wxCONFIG_USE_LOCAL_FILE));
        input=new wxTextCtrl(this,wxID_ANY,"",wxDefaultPosition,wxDefaultSize,wxBORDER_NONE);
        input->SetName("Search entries");
        input->SetHint("Search entries…");
        input->SetBackgroundColour(wxColour(27,31,41));
        input->SetForegroundColour(wxColour(244,246,255));
        input->SetFont(wxFontInfo(14).FaceName("Segoe UI"));
        LayoutInput();
        Bind(wxEVT_CHAR_HOOK,&Frame::Key,this);
        input->Bind(wxEVT_TEXT,[this](wxCommandEvent &) {
            const auto utf8=input->GetValue().ToUTF8();
            query(utf8.data(),this,queryData);
        });
        Bind(wxEVT_PAINT,[this](wxPaintEvent &) { wxPaintDC dc(this); Draw(); });
        Bind(wxEVT_ERASE_BACKGROUND,[](wxEraseEvent &) {});
        Bind(wxEVT_ACTIVATE,[this](wxActivateEvent &e) { if (!e.GetActive() && ready) Close(); e.Skip(); });
        Bind(wxEVT_LEFT_DOWN,[this](wxMouseEvent &e) {
            const float y=float(e.GetY())/GetDPIScaleFactor();
            if (y<42) { ReleaseCapture(); SendMessageW(reinterpret_cast<HWND>(GetHandle()),WM_NCLBUTTONDOWN,HTCAPTION,0); return; }
            const int row=int((y-132)/58)+first;
            if (y>=132 && row>=0 && row<int(items.size()) && row<first+Visible()) { selected=row; Submit(); }
        });
        Bind(wxEVT_MOUSEWHEEL,[this](wxMouseEvent &e) {
            first=(std::max)(0,(std::min)(first-(e.GetWheelRotation()>0?3:-3),int(items.size())-Visible())); Draw();
        });
        Bind(wxEVT_SIZE,[this](wxSizeEvent &) {
            if(input) LayoutInput();
            if(ready) { ctx->SetTarget(nullptr); HRESULT hr=swap->ResizeBuffers(0,(std::max)(1,GetClientSize().x),(std::max)(1,GetClientSize().y),DXGI_FORMAT_UNKNOWN,0); ready=SUCCEEDED(hr)&&BindBuffer(); KeepVisible(); Draw(); }
            // Consume this event: wxFrame's default layout stretches its only
            // child over the whole client area, hiding the composition surface.
        });
    }
    bool Start() {
        const HWND hwnd=reinterpret_cast<HWND>(GetHandle());
        // Remove the opaque GDI backing store beneath the composition tree.
        SetWindowLongPtrW(hwnd,GWL_EXSTYLE,GetWindowLongPtrW(hwnd,GWL_EXSTYLE)|WS_EX_NOREDIRECTIONBITMAP);
        // The native edit field keeps IME, selection and accessibility support.
        // Give this child its own backing store above the transparent parent.
        const HWND edit=reinterpret_cast<HWND>(input->GetHandle());
        SetWindowLongPtrW(edit,GWL_EXSTYLE,GetWindowLongPtrW(edit,GWL_EXSTYLE)|WS_EX_LAYERED);
        SetLayeredWindowAttributes(edit,0,255,LWA_ALPHA);
        HIGHCONTRASTW hc={sizeof(hc)};
        SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0);
        highContrast=(hc.dwFlags&HCF_HIGHCONTRASTON)!=0;
        const DWORD corners=2, dark=TRUE, acrylic=3;
        DwmSetWindowAttribute(hwnd,33,&corners,sizeof(corners));
        DwmSetWindowAttribute(hwnd,20,&dark,sizeof(dark));
        MARGINS margins={-1,-1,-1,-1};
        if(!highContrast) backdrop=SUCCEEDED(DwmSetWindowAttribute(hwnd,38,&acrylic,sizeof(acrylic))) && SUCCEEDED(DwmExtendFrameIntoClientArea(hwnd,&margins));
        ready=Graphics();
        if(!ready) { wxMessageBox("The search graphics could not be initialized. Update your display driver and try again.","Espanso search",wxOK|wxICON_ERROR); return false; }
        query("",this,queryData);
        Show(); Activate(this); input->SetFocus(); Draw();
        return true;
    }
    void SetItems(SearchItem *values,int count) {
        items.clear();
        for(int n=0;n<count;++n) {
            Item item;
            item.id=wxString::FromUTF8(values[n].id);
            item.label=wxString::FromUTF8(values[n].label);
            item.trigger=wxString::FromUTF8(values[n].trigger);
            item.key=HistoryKey(item.label,item.trigger);
            history->Read(item.key,&item.uses,0L);
            items.push_back(item);
        }
        std::stable_sort(items.begin(),items.end(),[](const Item &a,const Item &b){return a.uses>b.uses;});
        selected=first=0; Draw();
    }
};

class App : public wxApp {
public:
    SearchMetadata *metadata; QueryCallback query; ResultCallback result;
    void *queryData,*resultData;
    bool OnInit() override {
        Frame *frame=new Frame(metadata,query,queryData,result,resultData);
        if(!frame->Start()) { frame->Destroy(); return false; }
        return true;
    }
};
}

extern "C" void interop_show_search(SearchMetadata *metadata,QueryCallback query,void *data,ResultCallback result,void *resultData) {
    SetProcessDPIAware();
    auto *app=new glass_search::App();
    app->metadata=metadata; app->query=query; app->queryData=data;
    app->result=result; app->resultData=resultData;
    wxApp::SetInstance(app);
    int argc=0; wxEntry(argc,static_cast<char **>(nullptr));
}
extern "C" void update_items(void *app,SearchItem *items,int size) {
    static_cast<glass_search::Frame *>(app)->SetItems(items,size);
}