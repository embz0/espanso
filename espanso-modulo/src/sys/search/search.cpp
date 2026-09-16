#ifdef _WIN32
#include "search_windows.h"
#else
/*
 * This file is part of modulo.
 *
 * Copyright (C) 2020-2021 Federico Terzi
 *
 * modulo is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * modulo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with modulo.  If not, see <https://www.gnu.org/licenses/>.
 */

// Mouse dragging mechanism greatly inspired by:
// https://developpaper.com/wxwidgets-implementing-the-drag-effect-of-titleless-bar-window/

#define _UNICODE

#include "../common/common.h"
#include "../interop/interop.h"

#include "wx/fileconf.h"
#include "wx/filename.h"
#include "wx/htmllbox.h"
#include "wx/stdpaths.h"

#ifdef __WXMSW__
#include <windows.h>
#endif

#include <algorithm>
#include <memory>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef __WXOSX__
// Implemented in search_mac.mm. Returns true if the key window's first
// responder currently has marked (uncommitted) text from an IME
// composition (e.g. selecting a CJK candidate).
extern bool IsImeComposingInKeyWindow();
#endif

// Platform-specific styles
#ifdef __WXMSW__
const int SEARCH_BAR_FONT_SIZE = 16;
const long DEFAULT_STYLE = wxSTAY_ON_TOP | wxFRAME_TOOL_WINDOW | wxRESIZE_BORDER;
#endif
#ifdef __WXOSX__
const int SEARCH_BAR_FONT_SIZE = 20;
const long DEFAULT_STYLE = wxSTAY_ON_TOP | wxRESIZE_BORDER;
#endif
#ifdef __LINUX__
const int SEARCH_BAR_FONT_SIZE = 20;
const long DEFAULT_STYLE = wxSTAY_ON_TOP | wxRESIZE_BORDER;
#endif

const int HELP_TEXT_FONT_SIZE = 10;

const wxColour SELECTION_LIGHT_BG = wxColour(184, 215, 255);
const wxColour SELECTION_DARK_BG = wxColour(64, 107, 168);
const wxColour GLASS_LIGHT_BG = wxColour(239, 246, 255);
const wxColour GLASS_DARK_BG = wxColour(30, 36, 48);

// https://docs.wxwidgets.org/stable/classwx_frame.html
const int MIN_WIDTH = 500;
const int MIN_HEIGHT = 80;

typedef void (*QueryCallback)(const char *query, void *app, void *data);
typedef void (*ResultCallback)(const char *id, void *data);

SearchMetadata *searchMetadata = nullptr;
QueryCallback queryCallback = nullptr;
ResultCallback resultCallback = nullptr;
void *data = nullptr;
void *resultData = nullptr;
wxArrayString wxItems;
wxArrayString wxTriggers;
wxArrayString wxIds;

wxString UsageFilePath() {
    wxString dir = wxStandardPaths::Get().GetUserLocalDataDir();
    wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return dir + wxFILE_SEP_PATH + "emoji-search-usage.ini";
}

wxString UsageKey(const wxString &id) {
    wxString key = id;
    key.Replace("/", "_");
    key.Replace("\\", "_");
    key.Replace(":", "_");
    return "usage/" + key;
}

long UsageCount(const wxString &id) {
    wxFileConfig config("espanso-emoji-search", wxEmptyString, UsageFilePath(),
                        wxEmptyString, wxCONFIG_USE_LOCAL_FILE);
    long count = 0;
    config.Read(UsageKey(id), &count, 0L);
    return count;
}

void RecordUsage(const wxString &id) {
    wxFileConfig config("espanso-emoji-search", wxEmptyString, UsageFilePath(),
                        wxEmptyString, wxCONFIG_USE_LOCAL_FILE);
    const wxString key = UsageKey(id);
    long count = 0;
    config.Read(key, &count, 0L);
    config.Write(key, count + 1);
    config.Flush();
}

void EnableLiquidGlass(wxWindow *window) {
#ifdef __WXMSW__
    HWND hwnd = reinterpret_cast<HWND>(window->GetHandle());
    if (hwnd == nullptr) {
        return;
    }

    HMODULE dwmapi = LoadLibraryW(L"dwmapi.dll");
    if (dwmapi == nullptr) {
        return;
    }

    using DwmSetWindowAttributeFn = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    auto set_attribute = reinterpret_cast<DwmSetWindowAttributeFn>(
        GetProcAddress(dwmapi, "DwmSetWindowAttribute"));
    if (set_attribute != nullptr) {
        // Windows 11: rounded corners and the translucent system backdrop.
        const DWORD rounded_corners = 2;
        const DWORD transient_backdrop = 3;
        set_attribute(hwnd, 33, &rounded_corners,
                      static_cast<DWORD>(sizeof(rounded_corners)));
        set_attribute(hwnd, 38, &transient_backdrop,
                      static_cast<DWORD>(sizeof(transient_backdrop)));
    }
    FreeLibrary(dwmapi);
#endif
}

// App Code

class SearchApp : public wxApp {
  public:
    virtual bool OnInit();
};

class ResultListBox : public wxHtmlListBox {
  public:
    ResultListBox() {}
    ResultListBox(wxWindow *parent, bool isDark, const wxWindowID id,
                  const wxPoint &pos, const wxSize &size);

  protected:
    // override this method to return data to be shown in the listbox (this is
    // mandatory)
    virtual wxString OnGetItem(size_t n) const;

    // change the appearance by overriding these functions (this is optional)
    virtual void OnDrawBackground(wxDC &dc, const wxRect &rect, size_t n) const;

    bool isDark = false;

  public:
    wxDECLARE_NO_COPY_CLASS(ResultListBox);
    wxDECLARE_DYNAMIC_CLASS(ResultListBox);
};

wxIMPLEMENT_DYNAMIC_CLASS(ResultListBox, wxHtmlListBox);

ResultListBox::ResultListBox(wxWindow *parent, bool isDark, const wxWindowID id,
                             const wxPoint &pos, const wxSize &size)
    : wxHtmlListBox(parent, id, pos, size, 0) {
    this->isDark = isDark;
    SetMargins(5, 5);
    Refresh();
}

void ResultListBox::OnDrawBackground(wxDC &dc, const wxRect &rect,
                                     size_t n) const {
    if (IsSelected(n)) {
        if (isDark) {
            dc.SetBrush(wxBrush(SELECTION_DARK_BG));
        } else {
            dc.SetBrush(wxBrush(SELECTION_LIGHT_BG));
        }
    } else {
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
    }
    dc.SetPen(*wxTRANSPARENT_PEN);
    wxRect pill = rect;
    pill.Deflate(4, 2);
    dc.DrawRoundedRectangle(pill, 10);
}

// Helper function to escape HTML special characters
wxString EscapeHtml(const wxString &str) {
    wxString escaped = str;
    escaped.Replace(wxT("&"), wxT("&amp;"));  // Must be first to avoid double-escaping
    escaped.Replace(wxT("<"), wxT("&lt;"));
    escaped.Replace(wxT(">"), wxT("&gt;"));
    escaped.Replace(wxT("\""), wxT("&quot;"));
    return escaped;
}

wxString ResultListBox::OnGetItem(size_t n) const {
    wxString textColor = isDark ? "white" : "";
    wxString shortcut =
        (n < 8) ? wxString::Format(wxT("Alt+%i"), (int)n + 1) : " ";

    // Escape HTML special characters in label and trigger to prevent them
    // from being interpreted as HTML tags (fixes issue #974)
    wxString escapedLabel = EscapeHtml(wxItems[n]);
    wxString escapedTrigger = EscapeHtml(wxTriggers[n]);

    wxString result = wxString::Format(
        wxT("<font face='Segoe UI Emoji' color='%s'><table width='100%%'><tr><td>%s</td><td "
            "align='right'><b>%s</b> <font color='#636e72'> "
            "%s</font></td></tr></table></font>"),
        textColor, escapedLabel, escapedTrigger, shortcut);

    return result;
}

class SearchFrame : public wxFrame {
  public:
    SearchFrame(const wxString &title, const wxPoint &pos, const wxSize &size);

    wxPanel *panel = nullptr;
    wxTextCtrl *searchBar = nullptr;
    wxStaticBitmap *iconPanel = nullptr;
    wxStaticText *helpText = nullptr;
    ResultListBox *resultBox = nullptr;
    void SetItems(SearchItem *items, int itemSize);

  private:
    void OnCharEvent(wxKeyEvent &event);
    void OnQueryChange(wxCommandEvent &event);
    void OnItemClickEvent(wxCommandEvent &event);
    void OnActivate(wxActivateEvent &event);

    // Mouse events
    void OnMouseCaptureLost(wxMouseCaptureLostEvent &event);
    void OnMouseLeave(wxMouseEvent &event);
    void OnMouseMove(wxMouseEvent &event);
    void OnMouseLUp(wxMouseEvent &event);
    void OnMouseLDown(wxMouseEvent &event);
    wxPoint mLastPt;

    // Selection
    void SelectNext();
    void SelectPrevious();
    void Submit();
};

bool SearchApp::OnInit() {
    SearchFrame *frame =
        new SearchFrame(wxString::FromUTF8(searchMetadata->windowTitle),
                        wxPoint(50, 50), wxSize(450, 340));
    frame->Show(true);
    EnableLiquidGlass(frame);
    SetupWindowStyle(frame);
    Activate(frame);
    return true;
}
SearchFrame::SearchFrame(const wxString &title, const wxPoint &pos,
                         const wxSize &size)
    : wxFrame(NULL, wxID_ANY, title, pos, size, DEFAULT_STYLE) {
    wxInitAllImageHandlers();

#if wxCHECK_VERSION(3, 1, 3)
    bool isDark = wxSystemSettings::GetAppearance().IsDark();
#else
    // Workaround needed for previous versions of wxWidgets
    const wxColour bg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    const wxColour fg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    unsigned int bgSum = (bg.Red() + bg.Blue() + bg.Green());
    unsigned int fgSum = (fg.Red() + fg.Blue() + fg.Green());
    bool isDark = fgSum > bgSum;
#endif

    panel = new wxPanel(this, wxID_ANY);
    panel->SetBackgroundColour(isDark ? GLASS_DARK_BG : GLASS_LIGHT_BG);
    wxBoxSizer *vbox = new wxBoxSizer(wxVERTICAL);
    panel->SetSizer(vbox);

    wxBoxSizer *topBox = new wxBoxSizer(wxHORIZONTAL);

    int iconId = NewControlId();
    iconPanel = nullptr;
    if (searchMetadata->iconPath) {
        wxString iconPath = wxString::FromUTF8(searchMetadata->iconPath);
        if (wxFileExists(iconPath)) {
            wxBitmap bitmap = wxBitmap(iconPath, wxBITMAP_TYPE_PNG);
            if (bitmap.IsOk()) {
                wxImage image = bitmap.ConvertToImage();
                image.Rescale(32, 32, wxIMAGE_QUALITY_HIGH);
                wxBitmap resizedBitmap = wxBitmap(image, -1);
                iconPanel =
                    new wxStaticBitmap(panel, iconId, resizedBitmap,
                                       wxDefaultPosition, wxSize(32, 32));
                topBox->Add(iconPanel, 0, wxEXPAND | wxLEFT | wxUP | wxDOWN,
                            10);
            }
        }
    }

    int textId = NewControlId();
    searchBar =
        new wxTextCtrl(panel, textId, "", wxDefaultPosition, wxDefaultSize);
    wxFont font = searchBar->GetFont();
    font.SetPointSize(SEARCH_BAR_FONT_SIZE);
#ifdef __WXMSW__
    font.SetFaceName("Segoe UI Variable Text");
    searchBar->SetBackgroundColour(isDark ? wxColour(42, 51, 68)
                                          : wxColour(255, 255, 255));
#endif
    searchBar->SetFont(font);
    topBox->Add(searchBar, 1, wxEXPAND | wxALL, 10);

    vbox->Add(topBox, 1, wxEXPAND);

    if (searchMetadata->hintText) {
        helpText = new wxStaticText(
            panel, wxID_ANY, wxString::FromUTF8(searchMetadata->hintText));
        vbox->Add(helpText, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        wxFont helpFont = helpText->GetFont();
        helpFont.SetPointSize(HELP_TEXT_FONT_SIZE);
        helpText->SetFont(helpFont);
    }

    wxArrayString choices;
    int resultId = NewControlId();
    resultBox = new ResultListBox(panel, isDark, resultId, wxDefaultPosition,
                                  wxSize(MIN_WIDTH, MIN_HEIGHT));
    resultBox->SetBackgroundColour(isDark ? GLASS_DARK_BG : GLASS_LIGHT_BG);
    vbox->Add(resultBox, 5, wxEXPAND | wxALL, 6);

    Bind(wxEVT_CHAR_HOOK, &SearchFrame::OnCharEvent, this, wxID_ANY);
    searchBar->Bind(wxEVT_CHAR, &SearchFrame::OnCharEvent, this, wxID_ANY);
    Bind(wxEVT_TEXT, &SearchFrame::OnQueryChange, this, textId);
    Bind(wxEVT_LISTBOX_DCLICK, &SearchFrame::OnItemClickEvent, this, resultId);
    Bind(wxEVT_ACTIVATE, &SearchFrame::OnActivate, this, wxID_ANY);

    // Events to handle the mouse drag
    if (iconPanel) {
        iconPanel->Bind(wxEVT_LEFT_UP, &SearchFrame::OnMouseLUp, this);
        iconPanel->Bind(wxEVT_LEFT_DOWN, &SearchFrame::OnMouseLDown, this);
        Bind(wxEVT_MOTION, &SearchFrame::OnMouseMove, this);
        Bind(wxEVT_LEFT_UP, &SearchFrame::OnMouseLUp, this);
        Bind(wxEVT_LEFT_DOWN, &SearchFrame::OnMouseLDown, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &SearchFrame::OnMouseCaptureLost, this);
        Bind(wxEVT_LEAVE_WINDOW, &SearchFrame::OnMouseLeave, this);
    }

    wxSize bestSize = panel->GetBestSize();
    this->SetClientSize(bestSize);
    this->SetSizeHints(MIN_WIDTH, MIN_HEIGHT);
    this->CentreOnScreen();

    // Trigger the first data update
    queryCallback("", (void *)this, data);
}

void SearchFrame::OnCharEvent(wxKeyEvent &event) {
    if (event.GetKeyCode() == WXK_ESCAPE) {
        Close(true);
    } else if (event.GetKeyCode() == WXK_TAB) {
        if (wxGetKeyState(WXK_SHIFT)) {
            SelectPrevious();
        } else {
            SelectNext();
        }
    } else if (event.GetUnicodeKey() >= '1' &&
               event.GetUnicodeKey() <= '9') { // Alt + num shortcut
        int index = event.GetUnicodeKey() - '1';
        if (wxGetKeyState(WXK_ALT)) {
            if (resultBox->GetItemCount() > index) {
                resultBox->SetSelection(index);
                Submit();
            }
        } else {
            event.Skip();
        }
    } else if (event.GetKeyCode() == WXK_DOWN) {
        SelectNext();
    } else if (event.GetKeyCode() == WXK_UP) {
        SelectPrevious();
    } else if (event.GetKeyCode() == 'N' && event.RawControlDown()) {
        SelectNext();
    } else if (event.GetKeyCode() == 'P' && event.RawControlDown()) {
        SelectPrevious();
    } else if (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_NUMPAD_ENTER) {
#ifdef __WXOSX__
        // Don't consume Enter while an IME composition is in progress —
        // the user is committing a candidate (e.g. selecting a Chinese
        // character), not confirming the highlighted match. Let the event
        // propagate so the IME can finalize its selection.
        if (IsImeComposingInKeyWindow()) {
            event.Skip();
            return;
        }
#endif
        Submit();
    } else {
        event.Skip();
    }
}

void SearchFrame::OnQueryChange(wxCommandEvent &event) {
    if (helpText != nullptr) {
        helpText->Destroy();
        panel->Layout();
        helpText = nullptr;
    }

    wxString queryString = searchBar->GetValue();
    const char *query = queryString.ToUTF8();
    queryCallback(query, (void *)this, data);
}

void SearchFrame::OnItemClickEvent(wxCommandEvent &event) {
    resultBox->SetSelection(event.GetInt());
    Submit();
}

void SearchFrame::OnActivate(wxActivateEvent &event) {
    if (!event.GetActive()) {
        Close(true);
    }
    event.Skip();
}

void SearchFrame::OnMouseMove(wxMouseEvent &event) {
    if (event.LeftIsDown() && event.Dragging()) {
        wxPoint pt = event.GetPosition();
        wxPoint wndLeftTopPt = GetPosition();
        int distanceX = pt.x - mLastPt.x;
        int distanceY = pt.y - mLastPt.y;

        wxPoint desPt;
        desPt.x = distanceX + wndLeftTopPt.x - 24;
        desPt.y = distanceY + wndLeftTopPt.y - 24;
        this->Move(desPt);
    }

    if (event.LeftDown()) {
        this->CaptureMouse();
        mLastPt = event.GetPosition();
    }
}

void SearchFrame::OnMouseLeave(wxMouseEvent &event) {
    if (event.LeftIsDown() && event.Dragging()) {
        wxPoint pt = event.GetPosition();
        wxPoint wndLeftTopPt = GetPosition();
        int distanceX = pt.x - mLastPt.x;
        int distanceY = pt.y - mLastPt.y;

        wxPoint desPt;
        desPt.x = distanceX + wndLeftTopPt.x - 24;
        desPt.y = distanceY + wndLeftTopPt.y - 24;
        this->Move(desPt);
    }
}

void SearchFrame::OnMouseLDown(wxMouseEvent &event) {
    if (!HasCapture())
        this->CaptureMouse();
}

void SearchFrame::OnMouseLUp(wxMouseEvent &event) {
    if (HasCapture())
        ReleaseMouse();
}

void SearchFrame::OnMouseCaptureLost(wxMouseCaptureLostEvent &event) {
    if (HasCapture())
        ReleaseMouse();
}

void SearchFrame::SetItems(SearchItem *items, int itemSize) {
    wxItems.Clear();
    wxIds.Clear();
    wxTriggers.Clear();

    std::vector<std::pair<int, long>> ranked_items;
    ranked_items.reserve(itemSize);
    for (int i = 0; i < itemSize; i++) {
        const wxString id = wxString::FromUTF8(items[i].id);
        ranked_items.emplace_back(i, UsageCount(id));
    }

    // The search algorithm has already filtered and relevance-ranked matches.
    // Keep that order for equal counts while bringing familiar matching entries up.
    std::stable_sort(ranked_items.begin(), ranked_items.end(),
                     [](const auto &left, const auto &right) {
                         return left.second > right.second;
                     });

    for (const auto &ranked_item : ranked_items) {
        const int item_index = ranked_item.first;
        wxItems.Add(wxString::FromUTF8(items[item_index].label));
        wxIds.Add(wxString::FromUTF8(items[item_index].id));
        wxTriggers.Add(wxString::FromUTF8(items[item_index].trigger));
    }

    resultBox->SetItemCount(itemSize);

    if (itemSize > 0) {
        resultBox->SetSelection(0);
    }
    resultBox->RefreshAll();
    resultBox->Refresh();
}

void SearchFrame::SelectNext() {
    if (resultBox->GetItemCount() > 0 &&
        resultBox->GetSelection() != wxNOT_FOUND) {
        int newSelected = 0;
        if (resultBox->GetSelection() < (resultBox->GetItemCount() - 1)) {
            newSelected = resultBox->GetSelection() + 1;
        }

        resultBox->SetSelection(newSelected);
    }
}

void SearchFrame::SelectPrevious() {
    if (resultBox->GetItemCount() > 0 &&
        resultBox->GetSelection() != wxNOT_FOUND) {
        int newSelected = resultBox->GetItemCount() - 1;
        if (resultBox->GetSelection() > 0) {
            newSelected = resultBox->GetSelection() - 1;
        }

        resultBox->SetSelection(newSelected);
    }
}

void SearchFrame::Submit() {
    if (resultBox->GetItemCount() > 0 &&
        resultBox->GetSelection() != wxNOT_FOUND) {
        long index = resultBox->GetSelection();
        wxString id = wxIds[index];
        RecordUsage(id);
        if (resultCallback) {
            resultCallback(id.ToUTF8(), resultData);
        }

        Close(true);
    }
}

extern "C" void interop_show_search(SearchMetadata *_metadata,
                                    QueryCallback _queryCallback, void *_data,
                                    ResultCallback _resultCallback,
                                    void *_resultData) {
// Setup high DPI support on Windows
#ifdef __WXMSW__
    SetProcessDPIAware();
#endif

    searchMetadata = _metadata;
    queryCallback = _queryCallback;
    resultCallback = _resultCallback;
    data = _data;
    resultData = _resultData;

    wxApp::SetInstance(new SearchApp());
    int argc = 0;
    wxEntry(argc, (char **)nullptr);
}

extern "C" void update_items(void *app, SearchItem *items, int itemSize) {
    SearchFrame *frame = (SearchFrame *)app;
    frame->SetItems(items, itemSize);
}

#endif
