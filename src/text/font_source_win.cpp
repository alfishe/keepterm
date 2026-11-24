// IFontSource for Windows: DirectWrite, loaded at run time.
//
// Only APIs of build 14393 and older: IDWriteFactory (system collection, family lookup, file
// paths through IDWriteLocalFontFileLoader), IDWriteFont1::IsMonospacedFont (Windows 8) and
// IDWriteFactory2::GetSystemFontFallback / IDWriteFontFallback::MapCharacters (Windows 8.1).
// DirectWrite only finds fonts here; FreeType rasterizes them.
// Without dwrite.dll (or IDWriteFactory2 for fallback) the Windows font folders are scanned
// with DirectoryFontIndex instead.
#include "text/font_source.h"

#include <windows.h>
#include <dwrite_2.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace kt::text {

namespace {

template <class T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    T** put() {
        reset();
        return &p_;
    }
    explicit operator bool() const { return p_ != nullptr; }
    void reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }

private:
    T* p_ = nullptr;
};

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool ascii_only(std::wstring_view w) {
    for (wchar_t c : w) {
        if (c >= 0x80) return false;
    }
    return true;
}

// FontSystem opens paths with narrow file APIs; a path outside ASCII (a per-user font under a
// non-ASCII profile name) is read here with the wide API and handed over as memory.
std::optional<FontFileRef> ref_for_path(const std::wstring& path, uint32_t index) {
    FontFileRef ref;
    ref.face_index = index;
    if (ascii_only(path)) {
        ref.path = narrow(path);
        return ref;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    std::optional<FontFileRef> out;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= (LONGLONG{256} << 20)) {
        auto data = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (ReadFile(h, data->data(), static_cast<DWORD>(data->size()), &got, nullptr) && got == data->size()) {
            ref.memory = std::move(data);
            ref.path = narrow(path);   // for diagnostics only; memory wins
            out = std::move(ref);
        }
    }
    CloseHandle(h);
    return out;
}

std::optional<FontFileRef> ref_of_font(IDWriteFont* font) {
    Com<IDWriteFontFace> face;
    if (FAILED(font->CreateFontFace(face.put()))) return std::nullopt;
    UINT32 n = 0;
    if (FAILED(face->GetFiles(&n, nullptr)) || n == 0) return std::nullopt;
    std::vector<IDWriteFontFile*> files(n, nullptr);
    std::optional<FontFileRef> out;
    if (SUCCEEDED(face->GetFiles(&n, files.data()))) {
        const void* key = nullptr;
        UINT32 key_size = 0;
        Com<IDWriteFontFileLoader> loader;
        Com<IDWriteLocalFontFileLoader> local;
        if (files[0] && SUCCEEDED(files[0]->GetReferenceKey(&key, &key_size)) && SUCCEEDED(files[0]->GetLoader(loader.put())) &&
            SUCCEEDED(loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader), reinterpret_cast<void**>(local.put())))) {
            UINT32 len = 0;
            if (SUCCEEDED(local->GetFilePathLengthFromKey(key, key_size, &len))) {
                std::wstring path(len + 1, L'\0');
                if (SUCCEEDED(local->GetFilePathFromKey(key, key_size, path.data(), len + 1))) {
                    path.resize(len);
                    out = ref_for_path(path, face->GetIndex());
                }
            }
        }
    }
    for (IDWriteFontFile* f : files) {
        if (f) f->Release();
    }
    return out;
}

std::string family_name(IDWriteFontFamily* fam) {
    Com<IDWriteLocalizedStrings> names;
    if (FAILED(fam->GetFamilyNames(names.put()))) return {};
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(names->FindLocaleName(L"en-us", &index, &exists)) || !exists) index = 0;
    UINT32 len = 0;
    if (FAILED(names->GetStringLength(index, &len))) return {};
    std::wstring w(len + 1, L'\0');
    if (FAILED(names->GetString(index, w.data(), len + 1))) return {};
    w.resize(len);
    return narrow(w);
}

// IDWriteFontFallback::MapCharacters reads its text through this interface. One code point,
// on the stack, no reference counting. COM interfaces have no virtual destructor by design.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
class OneCharSource final : public IDWriteTextAnalysisSource {
public:
    OneCharSource(const wchar_t* text, UINT32 len, const wchar_t* locale) : text_(text), len_(len), locale_(locale) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** obj) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteTextAnalysisSource)) {
            *obj = static_cast<IDWriteTextAnalysisSource*>(this);
            return S_OK;
        }
        *obj = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 pos, WCHAR const** text, UINT32* len) override {
        if (pos >= len_) {
            *text = nullptr;
            *len = 0;
        } else {
            *text = text_ + pos;
            *len = len_ - pos;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 pos, WCHAR const** text, UINT32* len) override {
        if (pos == 0 || pos > len_) {
            *text = nullptr;
            *len = 0;
        } else {
            *text = text_;
            *len = pos;
        }
        return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 pos, UINT32* len, WCHAR const** locale) override {
        *len = pos < len_ ? len_ - pos : 0;
        *locale = locale_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 pos, UINT32* len, IDWriteNumberSubstitution** subst) override {
        *len = pos < len_ ? len_ - pos : 0;
        *subst = nullptr;
        return S_OK;
    }

private:
    const wchar_t* text_;
    UINT32 len_;
    const wchar_t* locale_;
};
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

std::vector<std::string> windows_font_dirs() {
    std::vector<std::string> dirs;
    wchar_t buf[MAX_PATH];
    const UINT n = GetWindowsDirectoryW(buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) dirs.push_back(narrow(std::wstring_view(buf, n)) + "\\Fonts");
    for (std::string& d : DirectoryFontIndex::standard_dirs()) {
        if (std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(std::move(d));
    }
    return dirs;
}

std::unique_ptr<DirectoryFontIndex> make_directory_index() {
    return std::make_unique<DirectoryFontIndex>(windows_font_dirs(), std::vector<std::string>{"Cascadia Mono", "Consolas"});
}

class DWriteFontSource final : public IFontSource {
public:
    DWriteFontSource(HMODULE dll, IDWriteFactory* factory) : dll_(dll) {
        *factory_.put() = factory;
        factory_->GetSystemFontCollection(collection_.put(), FALSE);
        Com<IDWriteFactory2> f2;
        if (SUCCEEDED(factory_->QueryInterface(__uuidof(IDWriteFactory2), reinterpret_cast<void**>(f2.put()))))
            f2->GetSystemFontFallback(fallback_.put());
    }
    ~DWriteFontSource() override {
        fallback_.reset();
        collection_.reset();
        factory_.reset();
        if (dll_) FreeLibrary(dll_);
    }
    bool ok() const { return static_cast<bool>(collection_); }

    std::vector<FontFamilyInfo> families() override {
        std::vector<FontFamilyInfo> out;
        const UINT32 n = collection_->GetFontFamilyCount();
        for (UINT32 i = 0; i < n; ++i) {
            Com<IDWriteFontFamily> fam;
            if (FAILED(collection_->GetFontFamily(i, fam.put()))) continue;
            FontFamilyInfo info;
            info.name = family_name(fam.get());
            if (info.name.empty()) continue;
            Com<IDWriteFont> font;
            Com<IDWriteFont1> font1;
            if (fam->GetFontCount() > 0 && SUCCEEDED(fam->GetFont(0, font.put())) &&
                SUCCEEDED(font->QueryInterface(__uuidof(IDWriteFont1), reinterpret_cast<void**>(font1.put()))))
                info.monospace = font1->IsMonospacedFont() != FALSE;
            out.push_back(std::move(info));
        }
        return out;
    }

    std::optional<FontFileRef> match(const FontQuery& q) override {
        const std::string family = q.family.empty() ? default_monospace() : q.family;
        Com<IDWriteFontFamily> fam;
        if (!find_family(family, fam)) return std::nullopt;
        const bool want_bold = q.style == Style::bold || q.style == Style::bold_italic;
        const bool want_italic = q.style == Style::italic || q.style == Style::bold_italic;
        Com<IDWriteFont> font;
        // DirectWrite weights are the CSS scale (font.weight).
        const auto weight = q.weight != 0     ? static_cast<DWRITE_FONT_WEIGHT>(q.weight)
                            : want_bold ? DWRITE_FONT_WEIGHT_BOLD
                                        : DWRITE_FONT_WEIGHT_NORMAL;
        if (FAILED(fam->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL,
                                             want_italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, font.put())))
            return std::nullopt;
        // A simulated or mismatched face means the family has no such style: FreeType synthesizes.
        if (font->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE) return std::nullopt;
        if (q.weight != 0) {
            if (std::abs(static_cast<int>(font->GetWeight()) - static_cast<int>(q.weight)) > 150) return std::nullopt;
        } else if ((font->GetWeight() >= DWRITE_FONT_WEIGHT_SEMI_BOLD) != want_bold) {
            return std::nullopt;
        }
        if ((font->GetStyle() != DWRITE_FONT_STYLE_NORMAL) != want_italic) return std::nullopt;
        return ref_of_font(font.get());
    }

    std::vector<FontFileRef> fallback_for(char32_t cp, const FontQuery& base, std::string_view lang) override {
        std::vector<FontFileRef> out;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return out;
        if (!fallback_) {
            if (!index_) index_ = make_directory_index();
            return index_->fallback_for(cp, base, lang);
        }
        wchar_t text[2];
        UINT32 len = 1;
        if (cp >= 0x10000) {
            const uint32_t v = static_cast<uint32_t>(cp) - 0x10000u;
            text[0] = static_cast<wchar_t>(0xD800u + (v >> 10));
            text[1] = static_cast<wchar_t>(0xDC00u + (v & 0x3FFu));
            len = 2;
        } else {
            text[0] = static_cast<wchar_t>(cp);
        }
        std::wstring locale = widen(lang);
        if (locale.empty()) {
            wchar_t buf[LOCALE_NAME_MAX_LENGTH];
            const int n = GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH);
            locale = n > 1 ? std::wstring(buf, static_cast<size_t>(n - 1)) : std::wstring(L"en-us");
        }
        OneCharSource src(text, len, locale.c_str());
        const std::wstring family = widen(base.family.empty() ? default_monospace() : base.family);
        const bool bold = base.style == Style::bold || base.style == Style::bold_italic;
        const bool italic = base.style == Style::italic || base.style == Style::bold_italic;
        UINT32 mapped = 0;
        Com<IDWriteFont> font;
        FLOAT scale = 1.0f;
        if (FAILED(fallback_->MapCharacters(&src, 0, len, collection_.get(), family.c_str(),
                                            bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                            italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, &mapped, font.put(), &scale)) ||
            !font)
            return out;
        BOOL has = FALSE;
        if (FAILED(font->HasCharacter(static_cast<UINT32>(cp), &has)) || !has) return out;
        if (auto ref = ref_of_font(font.get())) out.push_back(std::move(*ref));
        return out;
    }

    std::string default_monospace() override {
        // Cascadia Mono when installed (Windows 11, Windows Terminal), else Consolas (Server 2016).
        Com<IDWriteFontFamily> fam;
        return find_family("Cascadia Mono", fam) ? "Cascadia Mono" : "Consolas";
    }

private:
    bool find_family(const std::string& family, Com<IDWriteFontFamily>& out) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        const std::wstring w = widen(family);
        if (FAILED(collection_->FindFamilyName(w.c_str(), &index, &exists)) || !exists) return false;
        return SUCCEEDED(collection_->GetFontFamily(index, out.put()));
    }

    HMODULE dll_;
    Com<IDWriteFactory> factory_;
    Com<IDWriteFontCollection> collection_;
    Com<IDWriteFontFallback> fallback_;
    std::unique_ptr<DirectoryFontIndex> index_;
};

}  // namespace

std::unique_ptr<IFontSource> create_font_source() {
    HMODULE dll = LoadLibraryExW(L"dwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dll) {
        using CreateFactory = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
        const FARPROC proc = GetProcAddress(dll, "DWriteCreateFactory");
        CreateFactory create = nullptr;
        static_assert(sizeof(create) == sizeof(proc));
        std::memcpy(&create, &proc, sizeof proc);
        IUnknown* unknown = nullptr;
        if (create && SUCCEEDED(create(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &unknown)) && unknown) {
            auto source = std::make_unique<DWriteFontSource>(dll, static_cast<IDWriteFactory*>(unknown));
            if (source->ok()) return source;
        } else {
            FreeLibrary(dll);
        }
    }
    return make_directory_index();
}

}  // namespace kt::text
