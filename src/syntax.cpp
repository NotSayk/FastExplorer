#include "syntax.h"

#include <cwctype>
#include <string_view>
#include <unordered_set>

namespace {

struct Lang {
    const wchar_t* exts;      // « .c .h .cpp »
    const wchar_t* line1;     // commentaire de fin de ligne (ou nullptr)
    const wchar_t* line2;
    const wchar_t* open;      // commentaire de bloc
    const wchar_t* close;
    const wchar_t* quotes;    // délimiteurs de chaînes
    const wchar_t* keywords;  // séparés par des espaces
    bool ci = false;          // mots-clés sans casse
    bool hashPre = false;     // « #include » en début de ligne
    bool triple = false;      // """chaînes""" (Python)
    bool markup = false;      // XML / HTML
    bool ini = false;         // [section], clé=
    bool diff = false;        // + / - / @@
    bool remBatch = false;    // REM et :: (fichiers de commandes)
};

const wchar_t kC[] =
    L"auto break case char const continue default do double else enum extern float for goto if inline int long register "
    L"restrict return short signed sizeof static struct switch typedef union unsigned void volatile while bool true false "
    L"nullptr NULL alignas alignof and asm catch class constexpr consteval constinit concept co_await co_return co_yield "
    L"decltype delete explicit export friend mutable namespace new noexcept not operator or private protected public "
    L"requires static_assert static_cast dynamic_cast reinterpret_cast const_cast template this thread_local throw try "
    L"typeid typename using virtual wchar_t char8_t char16_t char32_t override final size_t uint8_t uint16_t uint32_t "
    L"uint64_t int8_t int16_t int32_t int64_t std";
const wchar_t kCs[] =
    L"abstract as base bool break byte case catch char checked class const continue decimal default delegate do double else "
    L"enum event explicit extern false finally fixed float for foreach goto if implicit in int interface internal is lock "
    L"long namespace new null object operator out override params private protected public readonly ref return sbyte "
    L"sealed short sizeof stackalloc static string struct switch this throw true try typeof uint ulong unchecked unsafe "
    L"ushort using virtual void volatile while var async await dynamic get set value yield record init required when where";
const wchar_t kJava[] =
    L"abstract assert boolean break byte case catch char class const continue default do double else enum extends final "
    L"finally float for goto if implements import instanceof int interface long native new package private protected "
    L"public return short static strictfp super switch synchronized this throw throws transient try void volatile while "
    L"true false null var record sealed permits yield fun val when object companion data is in out override open "
    L"internal lateinit suspend typealias let func guard extension protocol struct self Self nil async await late "
    L"required dynamic";
const wchar_t kJs[] =
    L"break case catch class const continue debugger default delete do else export extends finally for function if import "
    L"in instanceof let new return super switch this throw try typeof var void while with yield async await of static "
    L"get set true false null undefined NaN Infinity interface type enum implements private protected public readonly "
    L"abstract declare namespace module keyof as any number string boolean unknown never from";
const wchar_t kGo[] =
    L"break case chan const continue default defer else fallthrough for func go goto if import interface map package range "
    L"return select struct switch type var true false nil iota int int8 int16 int32 int64 uint uint8 uint16 uint32 uint64 "
    L"float32 float64 string bool byte rune error any";
const wchar_t kRust[] =
    L"as async await break const continue crate dyn else enum extern false fn for if impl in let loop match mod move mut pub "
    L"ref return self Self static struct super trait true type unsafe use where while i8 i16 i32 i64 i128 isize u8 u16 u32 "
    L"u64 u128 usize f32 f64 bool char str String Vec Option Some None Result Ok Err Box";
const wchar_t kPy[] =
    L"False None True and as assert async await break class continue def del elif else except finally for from global if "
    L"import in is lambda nonlocal not or pass raise return try while with yield match case self print len range";
const wchar_t kRb[] =
    L"alias and begin break case class def defined do else elsif end ensure false for if in module next nil not or redo "
    L"rescue retry return self super then true undef unless until when while yield require attr_accessor puts";
const wchar_t kSh[] =
    L"if then else elif fi case esac for while until do done in function return exit export local readonly echo set unset "
    L"shift source true false";
const wchar_t kPs[] =
    L"begin break catch class continue data define do dynamicparam else elseif end exit filter finally for foreach from "
    L"function if in param process return switch throw trap try until using var while workflow true false null";
const wchar_t kSql[] =
    L"select from where and or not insert into values update set delete create table view index drop alter add column "
    L"primary key foreign references join inner left right outer full cross on group by order having limit offset as "
    L"distinct union all case when then else end null is in like between exists begin commit rollback transaction int "
    L"integer varchar char text date datetime timestamp boolean default unique asc desc count sum avg min max with";
const wchar_t kLua[] =
    L"and break do else elseif end false for function goto if in local nil not or repeat return then true until while";
const wchar_t kBat[] =
    L"echo set if else goto call exit for do in not exist defined errorlevel setlocal endlocal shift pushd popd cd start "
    L"equ neq lss leq gtr geq off on";
const wchar_t kJson[] = L"true false null";
const wchar_t kPhp[] =
    L"abstract and array as break callable case catch class clone const continue declare default do echo else elseif empty "
    L"enddeclare endfor endforeach endif endswitch endwhile extends final finally fn for foreach function global goto if "
    L"implements include instanceof insteadof interface isset list match namespace new or print private protected public "
    L"readonly require return static switch throw trait try unset use var while yield true false null";
const wchar_t kZig[] =
    L"const var fn pub return if else while for switch break continue struct enum union error try catch defer errdefer "
    L"comptime inline test true false null undefined unreachable usize isize u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 bool void";

const Lang kLangs[] = {
    {L".c .cc .cpp .cxx .h .hh .hpp .hxx .ino", L"//", nullptr, L"/*", L"*/", L"\"'", kC, false, true},
    {L".cs", L"//", nullptr, L"/*", L"*/", L"\"'", kCs, false, true},
    {L".java .kt .kts .swift .dart .gradle .scala", L"//", nullptr, L"/*", L"*/", L"\"'", kJava},
    {L".js .mjs .cjs .ts .tsx .jsx .vue .svelte", L"//", nullptr, L"/*", L"*/", L"\"'`", kJs},
    {L".go", L"//", nullptr, L"/*", L"*/", L"\"'`", kGo},
    {L".rs", L"//", nullptr, L"/*", L"*/", L"\"", kRust},
    {L".zig", L"//", nullptr, nullptr, nullptr, L"\"'", kZig},
    {L".php", L"//", L"#", L"/*", L"*/", L"\"'", kPhp},
    {L".css .scss .less", L"//", nullptr, L"/*", L"*/", L"\"'", L""},
    {L".py", L"#", nullptr, nullptr, nullptr, L"\"'", kPy, false, false, true},
    {L".rb", L"#", nullptr, nullptr, nullptr, L"\"'", kRb},
    {L".sh .bash .zsh", L"#", nullptr, nullptr, nullptr, L"\"'", kSh},
    {L".ps1 .psm1 .psd1", L"#", nullptr, L"<#", L"#>", L"\"'", kPs, true},
    {L".yml .yaml .toml .r .pl .cmake .mk .makefile .gitignore .gitattributes", L"#", nullptr, nullptr, nullptr, L"\"'",
     kJson},
    {L".ini .cfg .conf .properties .env .editorconfig .reg .inf", L";", L"#", nullptr, nullptr, L"\"", L"", false, false,
     false, false, true},
    {L".sql", L"--", nullptr, L"/*", L"*/", L"'\"", kSql, true},
    {L".lua", L"--", nullptr, L"--[[", L"]]", L"\"'", kLua},
    {L".bat .cmd", nullptr, nullptr, nullptr, nullptr, L"\"", kBat, true, false, false, false, false, false, true},
    {L".json .jsonc", L"//", nullptr, L"/*", L"*/", L"\"", kJson},
    {L".xml .xaml .svg .csproj .vcxproj .props .targets .config .resx .plist .html .htm", nullptr, nullptr, nullptr,
     nullptr, L"\"'", L"", false, false, false, true},
    {L".diff .patch", nullptr, nullptr, nullptr, nullptr, L"", L"", false, false, false, false, false, true},
};

bool HasExt(const wchar_t* list, const wchar_t* ext) {
    const size_t n = wcslen(ext);
    for (const wchar_t* p = list; *p;) {
        const wchar_t* e = p;
        while (*e && *e != L' ') ++e;
        if ((size_t)(e - p) == n && _wcsnicmp(p, ext, n) == 0) return true;
        p = *e ? e + 1 : e;
    }
    return false;
}

const Lang* LangOf(const wchar_t* ext) {
    if (!ext || !*ext) return nullptr;
    for (const Lang& l : kLangs)
        if (HasExt(l.exts, ext)) return &l;
    return nullptr;
}

std::wstring Lower(std::wstring_view s) {
    std::wstring o(s);
    for (auto& c : o) c = (wchar_t)towlower(c);
    return o;
}

const std::unordered_set<std::wstring>& KeywordsOf(const Lang& l) {
    static std::unordered_set<std::wstring> sets[std::size(kLangs)];
    static bool ready[std::size(kLangs)] = {};
    const size_t i = (size_t)(&l - kLangs);
    if (!ready[i]) {
        for (const wchar_t* p = l.keywords; *p;) {
            const wchar_t* e = p;
            while (*e && *e != L' ') ++e;
            if (e > p) sets[i].insert(l.ci ? Lower({p, (size_t)(e - p)}) : std::wstring(p, (size_t)(e - p)));
            p = *e ? e + 1 : e;
        }
        ready[i] = true;
    }
    return sets[i];
}

bool At(const std::wstring& t, size_t i, const wchar_t* s) { return s && *s && t.compare(i, wcslen(s), s) == 0; }
bool AtI(const std::wstring& t, size_t i, const wchar_t* s) {
    const size_t n = wcslen(s);
    return i + n <= t.size() && _wcsnicmp(t.c_str() + i, s, n) == 0;
}
bool IdStart(wchar_t c) { return iswalpha(c) || c == L'_'; }
bool IdChar(wchar_t c) { return iswalnum(c) || c == L'_'; }
size_t LineEnd(const std::wstring& t, size_t i) {
    const size_t e = t.find(L'\n', i);
    return e == std::wstring::npos ? t.size() : e;
}

struct Out {
    std::vector<SynSpan>& v;
    void Add(size_t s, size_t e, SynKind k) {
        if (e > s) v.push_back({(uint32_t)s, (uint32_t)(e - s), k});
    }
};

void Markup(const std::wstring& t, Out& o) {
    size_t i = 0;
    while (i < t.size()) {
        if (At(t, i, L"<!--")) {
            size_t e = t.find(L"-->", i + 4);
            e = e == std::wstring::npos ? t.size() : e + 3;
            o.Add(i, e, SYN_COMMENT);
            i = e;
            continue;
        }
        if (t[i] != L'<') {
            ++i;
            continue;
        }
        size_t j = i + 1;  // nom de la balise, avec / ? !
        while (j < t.size() && (t[j] == L'/' || t[j] == L'?' || t[j] == L'!')) ++j;
        while (j < t.size() && (IdChar(t[j]) || t[j] == L':' || t[j] == L'-' || t[j] == L'.')) ++j;
        o.Add(i, j, SYN_TAG);
        i = j;
        while (i < t.size() && t[i] != L'>' && t[i] != L'<') {
            if (t[i] == L'"' || t[i] == L'\'') {
                const wchar_t q = t[i];
                size_t e = t.find(q, i + 1);
                e = e == std::wstring::npos ? t.size() : e + 1;
                o.Add(i, e, SYN_STRING);
                i = e;
            } else if (IdStart(t[i])) {
                size_t e = i;
                while (e < t.size() && (IdChar(t[e]) || t[e] == L':' || t[e] == L'-')) ++e;
                o.Add(i, e, SYN_PREPROC);  // nom d'attribut
                i = e;
            } else {
                ++i;
            }
        }
        if (i < t.size() && t[i] == L'>') {
            size_t s = i;
            if (s > 0 && (t[s - 1] == L'/' || t[s - 1] == L'?')) --s;
            o.Add(s, i + 1, SYN_TAG);
            ++i;
        }
    }
}

void Diff(const std::wstring& t, Out& o) {
    for (size_t i = 0; i < t.size();) {
        const size_t e = LineEnd(t, i);
        if (At(t, i, L"@@")) o.Add(i, e, SYN_KEYWORD);
        else if (At(t, i, L"+++") || At(t, i, L"---") || At(t, i, L"diff ") || At(t, i, L"index ")) o.Add(i, e, SYN_PREPROC);
        else if (t[i] == L'+') o.Add(i, e, SYN_NUMBER);
        else if (t[i] == L'-') o.Add(i, e, SYN_STRING);
        i = e + 1;
    }
}

}  // namespace

bool Syntax_Known(const wchar_t* ext) { return LangOf(ext) != nullptr; }

std::vector<SynSpan> Syntax_Spans(const std::wstring& t, const wchar_t* ext) {
    std::vector<SynSpan> v;
    const Lang* l = LangOf(ext);
    if (!l) return v;
    Out o{v};
    if (l->markup) {
        Markup(t, o);
        return v;
    }
    if (l->diff) {
        Diff(t, o);
        return v;
    }
    const auto& kw = KeywordsOf(*l);
    bool lineStart = true;
    size_t i = 0;
    while (i < t.size()) {
        const wchar_t c = t[i];
        if (c == L'\n') {
            lineStart = true;
            ++i;
            continue;
        }
        if (lineStart && (c == L' ' || c == L'\t')) {
            ++i;
            continue;
        }
        const bool first = lineStart;
        lineStart = false;
        if (first && l->hashPre && c == L'#') {
            const size_t e = LineEnd(t, i);
            o.Add(i, e, SYN_PREPROC);
            i = e;
            continue;
        }
        if (first && l->ini && c == L'[') {
            const size_t e = LineEnd(t, i);
            o.Add(i, e, SYN_KEYWORD);
            i = e;
            continue;
        }
        if (first && l->ini && c != L';' && c != L'#') {  // clé avant « = »
            const size_t e = LineEnd(t, i), eq = t.find(L'=', i);
            if (eq != std::wstring::npos && eq < e) {
                o.Add(i, eq, SYN_PREPROC);
                i = eq;
                continue;
            }
        }
        if (first && l->remBatch && (AtI(t, i, L"rem ") || AtI(t, i, L"rem\r") || At(t, i, L"::") || AtI(t, i, L"@rem "))) {
            const size_t e = LineEnd(t, i);
            o.Add(i, e, SYN_COMMENT);
            i = e;
            continue;
        }
        if (first && l->remBatch && c == L':') {  // étiquette
            const size_t e = LineEnd(t, i);
            o.Add(i, e, SYN_PREPROC);
            i = e;
            continue;
        }
        if (l->open && At(t, i, l->open)) {
            size_t e = t.find(l->close, i + wcslen(l->open));
            e = e == std::wstring::npos ? t.size() : e + wcslen(l->close);
            o.Add(i, e, SYN_COMMENT);
            i = e;
            continue;
        }
        if (At(t, i, l->line1) || At(t, i, l->line2)) {
            const size_t e = LineEnd(t, i);
            o.Add(i, e, SYN_COMMENT);
            i = e;
            continue;
        }
        if (wcschr(l->quotes, c) && c) {
            if (l->triple && i + 2 < t.size() && t[i + 1] == c && t[i + 2] == c) {
                const wchar_t q3[] = {c, c, c, 0};
                size_t e = t.find(q3, i + 3);
                e = e == std::wstring::npos ? t.size() : e + 3;
                o.Add(i, e, SYN_STRING);
                i = e;
                continue;
            }
            const bool multi = c == L'`';
            size_t e = i + 1;
            while (e < t.size() && t[e] != c && (multi || t[e] != L'\n')) {
                if (t[e] == L'\\' && e + 1 < t.size() && t[e + 1] != L'\n') ++e;
                ++e;
            }
            if (e < t.size() && t[e] == c) ++e;
            o.Add(i, e, SYN_STRING);
            i = e;
            continue;
        }
        if (iswdigit(c) && (i == 0 || !IdChar(t[i - 1]))) {
            size_t e = i + 1;
            while (e < t.size() && (IdChar(t[e]) || (t[e] == L'.' && e + 1 < t.size() && iswdigit(t[e + 1])))) ++e;
            o.Add(i, e, SYN_NUMBER);
            i = e;
            continue;
        }
        if (IdStart(c)) {
            size_t e = i + 1;
            while (e < t.size() && IdChar(t[e])) ++e;
            const std::wstring_view w(t.data() + i, e - i);
            if (!kw.empty() && kw.count(l->ci ? Lower(w) : std::wstring(w))) o.Add(i, e, SYN_KEYWORD);
            i = e;
            continue;
        }
        ++i;
    }
    return v;
}

std::string Syntax_Rtf(const std::wstring& text, const std::vector<SynSpan>& spans) {
    // Couleurs dans l'ordre de SynKind (1 = texte).
    std::string r =
        "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fmodern Consolas;}}"
        "{\\colortbl ;\\red30\\green30\\blue30;\\red0\\green0\\blue205;\\red163\\green21\\blue21;\\red0\\green128\\blue0;"
        "\\red9\\green134\\blue88;\\red128\\green0\\blue128;\\red128\\green0\\blue0;}\\f0\\cf1 ";
    r.reserve(r.size() + text.size() * 11 / 10 + spans.size() * 10);
    auto put = [&](size_t s, size_t e) {
        for (size_t i = s; i < e; ++i) {
            const wchar_t c = text[i];
            if (c == L'\\' || c == L'{' || c == L'}') {
                r += '\\';
                r += (char)c;
            } else if (c == L'\n') {
                r += "\\par\n";
            } else if (c == L'\r') {
            } else if (c == L'\t') {
                r += "\\tab ";
            } else if (c < 0x80) {
                r += (char)c;
            } else {
                r += "\\u" + std::to_string((int)(int16_t)c) + "?";
            }
        }
    };
    size_t at = 0;
    for (const SynSpan& s : spans) {
        if (s.start < at || s.start + s.len > text.size()) continue;
        put(at, s.start);
        r += "\\cf" + std::to_string((int)s.kind + 1) + " ";
        put(s.start, s.start + s.len);
        r += "\\cf1 ";
        at = s.start + s.len;
    }
    put(at, text.size());
    r += "}";
    return r;
}
