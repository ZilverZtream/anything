 #ifdef HAS_IMGUI
 #include <imgui.h>
 #include <imgui_impl_glfw.h>
 #include <imgui_impl_opengl3.h>
 #include <GLFW/glfw3.h>
 #include "stb_image.h"
#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#include <shlwapi.h>
#include <shellapi.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#else
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#endif // __APPLE__
#include <wchar.h>
 #include <algorithm>
 #include <vector>
 #include <string>
 #include <unordered_set>
 #include <cctype>
 #include <ctime>
 #include <cmath>
 #include <limits.h>
 #include <inttypes.h>
 #include <ctype.h>
 #include <string.h>
 #include <stdlib.h>
 #include <thread>
 #include <mutex>
 extern "C" {
 #include "lmdb.h"
 #include "database.h"
 #include "util.h"
 #include "anything.h"
 }
 #endif
 #include <stdio.h>

 #ifndef MAX_PATH
 #define MAX_PATH 260
 #endif
#ifndef MAX_LONG_PATH
#define MAX_LONG_PATH 32767
#endif

#ifdef HAS_IMGUI
struct StringMeta { uint32_t trigram_count; uint64_t bloom_offset; };
#ifdef _WIN32
static HANDLE bloom_mapping = NULL;
#endif
static const uint8_t* bloom_readonly_base = nullptr;
static size_t g_bloom_size = 0;
static bool open_bloom(const wchar_t* dbPath){
#ifdef _WIN32
    wchar_t bp[MAX_PATH]; swprintf(bp, MAX_PATH, L"%s\\bloom.dat", dbPath);
    HANDLE f = CreateFileW(bp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if(f==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(f,&sz); g_bloom_size = sz.QuadPart;
    bloom_mapping = CreateFileMappingW(f,NULL,PAGE_READONLY,0,0,NULL);
    CloseHandle(f);
    if(!bloom_mapping) return false;
    bloom_readonly_base = (const uint8_t*)MapViewOfFile(bloom_mapping, FILE_MAP_READ, 0,0,0);
    if(!bloom_readonly_base){ CloseHandle(bloom_mapping); bloom_mapping=NULL; return false; }
    return true;
#else
    char bp[MAX_PATH]; wcstombs(bp, dbPath, MAX_PATH);
    strncat(bp, "/bloom.dat", MAX_PATH - strlen(bp) - 1);
    int fd = open(bp, O_RDONLY);
    if(fd<0) return false;
    struct stat st; if(fstat(fd,&st)!=0){ close(fd); return false; }
    g_bloom_size = st.st_size;
    bloom_readonly_base = (const uint8_t*)mmap(NULL, g_bloom_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return bloom_readonly_base != MAP_FAILED;
#endif
}
static void close_bloom(){
    if(!bloom_readonly_base) return;
#ifdef _WIN32
    UnmapViewOfFile(bloom_readonly_base);
    if(bloom_mapping) CloseHandle(bloom_mapping);
    bloom_mapping=NULL;
#else
    munmap((void*)bloom_readonly_base, g_bloom_size);
#endif
    bloom_readonly_base = nullptr;
}
struct Result {
    std::string filename;
    std::string path;
    std::string snippet;
    int64_t size;
    time_t modified;
    std::string type;
    float score = 0.0f;
    GLuint texture = 0;
    int width = 0;
    int height = 0;
    uint64_t rec_id = 0;
    uint64_t name_str_id = 0;
    uint64_t content_str_id = 0;
};

struct DuplicateItem {
    std::string path;
    uint64_t size = 0;
    uint64_t hash = 0;
    bool selected = false;
};

static std::vector<DuplicateItem> g_duplicates;

#ifdef _WIN32
static bool taskbar_search_is_enabled(){
    HKEY key;
    bool ok = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\anything", 0, KEY_READ, &key) == ERROR_SUCCESS;
    if(ok) RegCloseKey(key);
    return ok;
}

static void set_taskbar_search(bool enable){
    if(enable){
        HKEY key;
        wchar_t exe[MAX_LONG_PATH];
        GetModuleFileNameW(NULL, exe, MAX_LONG_PATH);
        if(RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\anything", 0, NULL, 0,
            KEY_SET_VALUE | KEY_CREATE_SUB_KEY, NULL, &key, NULL) == ERROR_SUCCESS){
            const wchar_t* desc = L"URL:Anything search";
            RegSetValueExW(key, NULL, 0, REG_SZ, (const BYTE*)desc,
                (DWORD)((wcslen(desc)+1)*sizeof(wchar_t)));
            RegSetValueExW(key, L"URL Protocol", 0, REG_SZ, (const BYTE*)L"",
                (DWORD)sizeof(wchar_t));
            HKEY cmd;
            if(RegCreateKeyExW(key, L"shell\\open\\command", 0, NULL, 0,
                KEY_SET_VALUE, NULL, &cmd, NULL) == ERROR_SUCCESS){
                wchar_t cmdline[MAX_LONG_PATH*2];
                _snwprintf(cmdline, ARRAYSIZE(cmdline), L"\"%s\" \"%%1\"", exe);
                RegSetValueExW(cmd, NULL, 0, REG_SZ, (const BYTE*)cmdline,
                    (DWORD)((wcslen(cmdline)+1)*sizeof(wchar_t)));
                RegCloseKey(cmd);
            }
            RegCloseKey(key);
        }
    } else {
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\anything");
    }
}
#else
static bool taskbar_search_is_enabled(){ return false; }
static void set_taskbar_search(bool){ }
#endif

struct Date {
    int year = 2020;
    int month = 0;
    int day = 1;
};

struct Filters {
    std::string ext;
    float min_size_mb = 0.0f;
    float max_size_mb = -1.0f;
    Date min_date;
    Date max_date;
    bool use_min_date = false;
    bool use_max_date = false;
    std::string path_filter;
    bool regex_mode = false;
    bool whole_word = false;
};

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s;
}

time_t get_date_time(const Date& d) {
    std::tm t = {};
    t.tm_year = d.year - 1900;
    t.tm_mon = d.month;
    t.tm_mday = d.day;
    t.tm_hour = 0;
    t.tm_min = 0;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    return std::mktime(&t);
}

void draw_highlighted(const std::string& text, const std::string& q) {
    if (q.empty()) {
        ImGui::TextWrapped("%s", text.c_str());
        return;
    }
    std::string lower_text = to_lower(text);
    std::string lower_q = to_lower(q);
    size_t start = 0;
    while (true) {
        size_t found = lower_text.find(lower_q, start);
        if (found == std::string::npos) {
            ImGui::TextWrapped("%s", text.substr(start).c_str());
            break;
        }
        ImGui::TextWrapped("%s", text.substr(start, found - start).c_str());
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
        ImGui::TextWrapped("%s", text.substr(found, lower_q.size()).c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 0.0f);
        start = found + lower_q.size();
    }
}

enum ResultColumn { COL_NAME, COL_PATH, COL_SIZE, COL_MOD, COL_SCORE };

static void open_file_os(const std::string& p){
#ifdef _WIN32
    ShellExecuteA(NULL, "open", p.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    CFStringRef cfPath = CFStringCreateWithCString(nullptr, p.c_str(), kCFStringEncodingUTF8);
    if(!cfPath) return;
    CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, cfPath, kCFURLPOSIXPathStyle, false);
    CFRelease(cfPath);
    if(!url) return;
    // Use NSWorkspace for native file opening
    id ws = ((id(*)(id, SEL))objc_msgSend)((id)objc_getClass("NSWorkspace"), sel_registerName("sharedWorkspace"));
    ((void(*)(id, SEL, id))objc_msgSend)(ws, sel_registerName("openURL:"), url);
    CFRelease(url);
#elif defined(__ANDROID__)
    std::string cmd = std::string("am start -a android.intent.action.VIEW -d \"file://") + p + "\"";
    system(cmd.c_str());
#else
    std::string cmd = std::string("xdg-open \"") + p + "\"";
    system(cmd.c_str());
#endif
}

static void open_folder_os(const std::string& p){
#ifdef _WIN32
    ShellExecuteA(NULL, "open", p.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    CFStringRef cfPath = CFStringCreateWithCString(nullptr, p.c_str(), kCFStringEncodingUTF8);
    if(!cfPath) return;
    CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, cfPath, kCFURLPOSIXPathStyle, true);
    CFRelease(cfPath);
    if(!url) return;
    // Use NSWorkspace for native folder opening
    id ws = ((id(*)(id, SEL))objc_msgSend)((id)objc_getClass("NSWorkspace"), sel_registerName("sharedWorkspace"));
    ((void(*)(id, SEL, id))objc_msgSend)(ws, sel_registerName("openURL:"), url);
    CFRelease(url);
#elif defined(__ANDROID__)
    std::string cmd = std::string("am start -a android.intent.action.VIEW -d \"file://") + p + "\"";
    system(cmd.c_str());
#else
    std::string cmd = std::string("xdg-open \"") + p + "\"";
    system(cmd.c_str());
#endif
}

static void delete_path_os(const std::string& p){
#ifdef _WIN32
    DeleteFileA(p.c_str());
#else
    remove(p.c_str());
#endif
}

static void load_result_texture(Result& r) {
    std::string full = r.path + "\\" + r.filename;
    int channels;
    unsigned char* data = stbi_load(full.c_str(), &r.width, &r.height, &channels, 0);
    if (!data) return;
    glGenTextures(1, &r.texture);
    glBindTexture(GL_TEXTURE_2D, r.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLenum fmt = (channels == 4) ? GL_RGBA : (channels == 3) ? GL_RGB : GL_RED;
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, r.width, r.height, 0, fmt, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);
}

static void apply_sort(std::vector<Result>& items, const ImGuiTableSortSpecs* specs){
    if(!specs || specs->SpecsCount==0) return;
    const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
    int col = spec.ColumnUserID;
    bool asc = spec.SortDirection == ImGuiSortDirection_Ascending;
    auto cmp = [col,asc](const Result& a, const Result& b){
        switch(col){
        case COL_NAME: return asc ? a.filename < b.filename : a.filename > b.filename;
        case COL_PATH: return asc ? a.path < b.path : a.path > b.path;
        case COL_SIZE: return asc ? a.size < b.size : a.size > b.size;
        case COL_MOD:  return asc ? a.modified < b.modified : a.modified > b.modified;
        case COL_SCORE: default: return asc ? a.score < b.score : a.score > b.score;
        }
    };
    std::stable_sort(items.begin(), items.end(), cmp);
}

struct IdVec {
    uint64_t* ids;
    size_t n, cap;
};

void idvec_init(IdVec* v) { v->ids = NULL; v->n = v->cap = 0; }

void idvec_push(IdVec* v, uint64_t x) {
    if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 512; v->ids = (uint64_t*)realloc(v->ids, v->cap * sizeof(uint64_t)); }
    v->ids[v->n++] = x;
}

void idvec_free(IdVec* v) { free(v->ids); v->ids = NULL; v->n = v->cap = 0; }

int cmp_u64(const void* A, const void* B) {
    uint64_t a = *(const uint64_t*)A, b = *(const uint64_t*)B;
    return (a > b) - (a < b);
}

void sort_unique(IdVec* v) {
    qsort(v->ids, v->n, sizeof(uint64_t), cmp_u64);
    size_t w = 0; uint64_t prev = 0;
    for (size_t i = 0; i < v->n; i++) {
        if (i == 0 || v->ids[i] != prev) {
            v->ids[w++] = prev = v->ids[i];
        }
    }
    v->n = w;
}

void intersect_inplace(IdVec* a, const IdVec* b) {
    size_t i = 0, j = 0, w = 0;
    while (i < a->n && j < b->n) {
        uint64_t x = a->ids[i], y = b->ids[j];
        if (x == y) { a->ids[w++] = x; i++; j++; }
        else if (x < y) i++;
        else j++;
    }
    a->n = w;
}

void union_inplace(IdVec* a, const IdVec* b) {
    for (size_t i = 0; i < b->n; i++) { idvec_push(a, b->ids[i]); }
}

void difference_inplace(IdVec* a, const IdVec* b) {
    size_t i = 0, j = 0, w = 0;
    while (i < a->n && j < b->n) {
        uint64_t x = a->ids[i], y = b->ids[j];
        if (x == y) { i++; j++; }
        else if (x < y) { a->ids[w++] = x; i++; }
        else { j++; }
    }
    while (i < a->n) { a->ids[w++] = a->ids[i++]; }
    a->n = w;
}

enum TokType { TOK_TERM, TOK_AND, TOK_OR, TOK_NOT, TOK_LPAREN, TOK_RPAREN };

enum TermType { TERM_NAME, TERM_AUTHOR, TERM_EXT, TERM_CONTENT };

struct Token {
    TokType type;
    TermType ttype;
    std::string text;
};

struct TokenList {
    std::vector<Token> items;
};

void add_logic_token(TokenList* toks, const std::string& s) {
    if (s.empty()) return;
    std::string lower_s = to_lower(s);
    if (lower_s == "and") { toks->items.push_back({TOK_AND}); return; }
    if (lower_s == "or") { toks->items.push_back({TOK_OR}); return; }
    if (lower_s == "not") { toks->items.push_back({TOK_NOT}); return; }
    if (lower_s.find("author:") == 0) { toks->items.push_back({TOK_TERM, TERM_AUTHOR, s.substr(7)}); return; }
    if (lower_s.find("ext:") == 0) { toks->items.push_back({TOK_TERM, TERM_EXT, s.substr(4)}); return; }
    if (lower_s.find("content:") == 0) { toks->items.push_back({TOK_TERM, TERM_CONTENT, s.substr(8)}); return; }
    toks->items.push_back({TOK_TERM, TERM_NAME, s});
}

void parse_query(const std::string& query, TokenList* tokens) {
    tokens->items.clear();
    std::string buf;
    for (char c : query) {
        if (c == '(' || c == ')') {
            if (!buf.empty()) { add_logic_token(tokens, buf); buf.clear(); }
            tokens->items.push_back({(c == '(') ? TOK_LPAREN : TOK_RPAREN});
        } else if (std::isspace(c)) {
            if (!buf.empty()) { add_logic_token(tokens, buf); buf.clear(); }
        } else {
            buf += c;
        }
    }
    if (!buf.empty()) { add_logic_token(tokens, buf); }
}

struct Node {
    int type;
    TermType ttype;
    std::string text;
    Node* left = nullptr;
    Node* right = nullptr;
};

void free_node(Node* n) {
    if (!n) return;
    free_node(n->left);
    free_node(n->right);
    delete n;
}

int precedence(TokType t) {
    if (t == TOK_NOT) return 3;
    if (t == TOK_AND) return 2;
    if (t == TOK_OR) return 1;
    return 0;
}

Node* make_leaf(const Token& t) {
    Node* n = new Node();
    n->type = TOK_TERM;
    n->ttype = t.ttype;
    n->text = t.text;
    return n;
}

void apply_op(TokType op, std::vector<Node*>& stack) {
    Node* n = new Node();
    if (op == TOK_NOT) {
        if (stack.empty()) { delete n; return; }
        n->type = TOK_NOT;
        n->left = stack.back(); stack.pop_back();
    } else {
        if (stack.size() < 2) { delete n; return; }
        Node* r = stack.back(); stack.pop_back();
        Node* l = stack.back(); stack.pop_back();
        n->type = op;
        n->left = l;
        n->right = r;
    }
    stack.push_back(n);
}

Node* parse_tokens(const TokenList& toks) {
    std::vector<Token> opstack;
    std::vector<Node*> nodestack;
    for (const auto& tk : toks.items) {
        if (tk.type == TOK_TERM) {
            nodestack.push_back(make_leaf(tk));
        } else if (tk.type == TOK_AND || tk.type == TOK_OR || tk.type == TOK_NOT) {
            while (!opstack.empty() && opstack.back().type != TOK_LPAREN &&
                   precedence(opstack.back().type) >= precedence(tk.type)) {
                apply_op(opstack.back().type, nodestack);
                opstack.pop_back();
            }
            opstack.push_back(tk);
        } else if (tk.type == TOK_LPAREN) {
            opstack.push_back(tk);
        } else if (tk.type == TOK_RPAREN) {
            while (!opstack.empty() && opstack.back().type != TOK_LPAREN) {
                apply_op(opstack.back().type, nodestack);
                opstack.pop_back();
            }
            if (!opstack.empty() && opstack.back().type == TOK_LPAREN) opstack.pop_back();
        }
    }
    while (!opstack.empty()) {
        apply_op(opstack.back().type, nodestack);
        opstack.pop_back();
    }
    if (nodestack.size() != 1) return nullptr;
    return nodestack[0];
}

void collect_trigram_candidates(MDB_txn* txn, MDB_dbi dbi_trigram, const std::string& term, IdVec* out) {
    size_t len = term.length();
    if (len < 3) {
        out->n = 0;
        return;
    }
    std::string tmp = to_lower(term);
    IdVec candidates; idvec_init(&candidates);
    bool first = true;
    for (size_t i = 0; i + 3 <= len; i++) {
        uint32_t key = ((uint8_t)tmp[i] << 16) | ((uint8_t)tmp[i + 1] << 8) | ((uint8_t)tmp[i + 2]);
        MDB_val k = {.mv_data = &key, .mv_size = 3};
        MDB_cursor* c = nullptr;
        mdb_cursor_open(txn, dbi_trigram, &c);
        IdVec gram; idvec_init(&gram);
        MDB_val v;
        if (mdb_cursor_get(c, &k, &v, MDB_SET_KEY) == 0) {
            do {
                idvec_push(&gram, *(uint64_t*)v.mv_data);
            } while (mdb_cursor_get(c, &k, &v, MDB_NEXT_DUP) == 0);
        }
        mdb_cursor_close(c);
        sort_unique(&gram);
        if (first) {
            candidates = gram;
            first = false;
        } else {
            intersect_inplace(&candidates, &gram);
            idvec_free(&gram);
        }
        if (candidates.n == 0) break;
    }
    *out = candidates;
}

void records_for_name(MDB_txn* txn, MDB_dbi dbi_trigram, MDB_dbi dbi_fname, MDB_dbi dbi_strings, MDB_dbi dbi_smeta, const std::string& term, IdVec* out) {
    IdVec name_ids; idvec_init(&name_ids);
    collect_trigram_candidates(txn, dbi_trigram, term, &name_ids);
    sort_unique(&name_ids);
    if (name_ids.n > 0) {
        size_t keep = 0;
        size_t tlen = term.length();
        std::string tl = to_lower(term);
        uint32_t hbuf[4096]; size_t hn = 0;
        for (size_t i = 0; i + 3 <= tlen && hn < 4096; i++) {
            uint32_t h = 2166136261u ^ 0xA5A5A5A5u;
            for (int k = 0; k < 3; k++) { h ^= (uint8_t)tl[i + k]; h *= 16777619u; }
            hbuf[hn++] = h & 0xFFFFu;
            h = 2166136261u ^ 0x3C3C3C3Cu;
            for (int k = 0; k < 3; k++) { h ^= (uint8_t)tl[i + k]; h *= 16777619u; }
            hbuf[hn++] = h & 0xFFFFu;
            h = 2166136261u ^ 0x5A5A5A5Au;
            for (int k = 0; k < 3; k++) { h ^= (uint8_t)tl[i + k]; h *= 16777619u; }
            hbuf[hn++] = h & 0xFFFFu;
            h = 2166136261u ^ 0x1F1F1F1Fu;
            for (int k = 0; k < 3; k++) { h ^= (uint8_t)tl[i + k]; h *= 16777619u; }
            hbuf[hn++] = h & 0xFFFFu;
        }
        for (size_t i = 0; i < name_ids.n; i++) {
            MDB_val k = {.mv_data = &name_ids.ids[i], .mv_size = sizeof(uint64_t)};
            MDB_val v;
            if (mdb_get(txn, dbi_smeta, &k, &v) != 0 || v.mv_size < sizeof(StringMeta)) continue;
            const StringMeta* sm = (const StringMeta*)v.mv_data;
            if (sm->bloom_offset + 8192 > g_bloom_size) continue;
            const uint8_t* bloom = bloom_readonly_base + sm->bloom_offset;
            bool ok = true;
            for (size_t j = 0; j < hn; j++) {
                uint32_t bit = hbuf[j];
                if ((bloom[bit >> 3] & (1u << (bit & 7))) == 0) { ok = false; break; }
            }
            if (ok) { name_ids.ids[keep++] = name_ids.ids[i]; }
        }
        name_ids.n = keep;
    }
    MDB_cursor* cix = nullptr;
    mdb_cursor_open(txn, dbi_fname, &cix);
    if (name_ids.n > 0) {
        for (size_t i = 0; i < name_ids.n; i++) {
            MDB_val k = {.mv_data = &name_ids.ids[i], .mv_size = sizeof(uint64_t)};
            MDB_val v;
            if (mdb_cursor_get(cix, &k, &v, MDB_SET_KEY) == 0) {
                do {
                    idvec_push(out, *(uint64_t*)v.mv_data);
                } while (mdb_cursor_get(cix, &k, &v, MDB_NEXT_DUP) == 0);
            }
        }
    } else {
        MDB_cursor* cs = nullptr;
        mdb_cursor_open(txn, dbi_strings, &cs);
        MDB_val sk, sv;
        int rc = mdb_cursor_get(cs, &sk, &sv, MDB_FIRST);
        std::string npat = to_lower(term);
        for (size_t j = 0; j < npat.length(); ++j) { if (npat[j] == '_' || npat[j] == '-') npat[j] = ' '; }
        int maxd = (int)((npat.length() + 4) / 5);
        while (rc == 0) {
            uint64_t sid = *(uint64_t*)sk.mv_data;
            std::string name = std::string((char*)sv.mv_data, sv.mv_size);
            std::string norm = name;
            std::transform(norm.begin(), norm.end(), norm.begin(), [](unsigned char c){ return std::tolower(c); });
            for (char& c : norm) { if (c == '_' || c == '-' || c == '.') c = ' '; }
            if (fuzzy_match(norm.c_str(), npat.c_str(), maxd)) {
                MDB_val k = {.mv_data = &sid, .mv_size = sizeof(sid)};
                MDB_val v;
                if (mdb_cursor_get(cix, &k, &v, MDB_SET_KEY) == 0) {
                    do {
                        idvec_push(out, *(uint64_t*)v.mv_data);
                    } while (mdb_cursor_get(cix, &k, &v, MDB_NEXT_DUP) == 0);
                }
            }
            rc = mdb_cursor_get(cs, &sk, &sv, MDB_NEXT);
        }
        mdb_cursor_close(cs);
    }
    mdb_cursor_close(cix);
}

void records_for_content(MDB_txn* txn, MDB_dbi dbi_trigram, MDB_dbi dbi_content, const std::string& term, IdVec* out) {
    IdVec ids; idvec_init(&ids);
    collect_trigram_candidates(txn, dbi_trigram, term, &ids);
    sort_unique(&ids);
    MDB_cursor* cc = nullptr;
    mdb_cursor_open(txn, dbi_content, &cc);
    for (size_t i = 0; i < ids.n; i++) {
        MDB_val k = {.mv_data = &ids.ids[i], .mv_size = sizeof(uint64_t)};
        MDB_val v;
        if (mdb_cursor_get(cc, &k, &v, MDB_SET_KEY) == 0) {
            do {
                idvec_push(out, *(uint64_t*)v.mv_data);
            } while (mdb_cursor_get(cc, &k, &v, MDB_NEXT_DUP) == 0);
        }
    }
    mdb_cursor_close(cc);
    idvec_free(&ids);
}

void records_for_author(MDB_txn* txn, MDB_dbi dbi_author, MDB_dbi dbi_strrev, const std::string& author, IdVec* out) {
    MDB_val k = {.mv_data = (void*)author.c_str(), .mv_size = author.length()};
    MDB_val v;
    if (mdb_get(txn, dbi_strrev, &k, &v) == 0) {
        uint64_t sid = *(uint64_t*)v.mv_data;
        MDB_cursor* ca = nullptr;
        mdb_cursor_open(txn, dbi_author, &ca);
        MDB_val ak = {.mv_data = &sid, .mv_size = sizeof(sid)};
        MDB_val av;
        if (mdb_cursor_get(ca, &ak, &av, MDB_SET_KEY) == 0) {
            do {
                idvec_push(out, *(uint64_t*)av.mv_data);
            } while (mdb_cursor_get(ca, &ak, &av, MDB_NEXT_DUP) == 0);
        }
        mdb_cursor_close(ca);
    }
}

void records_for_ext(MDB_txn* txn, MDB_dbi dbi_ext, const std::string& ext, IdVec* out) {
    std::string buf = to_lower(ext);
    MDB_val k = {.mv_data = (void*)buf.c_str(), .mv_size = buf.length()};
    MDB_cursor* ce = nullptr;
    mdb_cursor_open(txn, dbi_ext, &ce);
    MDB_val v;
    if (mdb_cursor_get(ce, &k, &v, MDB_SET_KEY) == 0) {
        do {
            idvec_push(out, *(uint64_t*)v.mv_data);
        } while (mdb_cursor_get(ce, &k, &v, MDB_NEXT_DUP) == 0);
    }
    mdb_cursor_close(ce);
}

typedef void (*RecordCallback)(uint64_t id, void* ctx);

static void stream_all_records(MDB_txn* txn, MDB_dbi dbi_date,
                               RecordCallback cb, void* ctx) {
    MDB_cursor* cd = nullptr;
    if (mdb_cursor_open(txn, dbi_date, &cd) != 0) return;
    MDB_val k, v;
    int rc = mdb_cursor_get(cd, &k, &v, MDB_FIRST);
    while (rc == 0) {
        cb(*(uint64_t*)v.mv_data, ctx);
        rc = mdb_cursor_get(cd, &k, &v, MDB_NEXT);
    }
    mdb_cursor_close(cd);
}

struct DiffCtx { const IdVec* excl; IdVec* out; };
static int cmp_u64(const void* a, const void* b) {
    uint64_t ua = *(const uint64_t*)a, ub = *(const uint64_t*)b;
    if (ua < ub) return -1; if (ua > ub) return 1; return 0;
}
static void diff_collect(uint64_t id, void* p) {
    DiffCtx* c = (DiffCtx*)p;
    if (!bsearch(&id, c->excl->ids, c->excl->n, sizeof(uint64_t), cmp_u64))
        idvec_push(c->out, id);
}

static void collect_record(uint64_t id, void* ctx) {
    idvec_push((IdVec*)ctx, id);
}

void eval_node(Node* n, MDB_txn* txn, MDB_dbi dbi_strings, MDB_dbi dbi_fname, MDB_dbi dbi_trigram, MDB_dbi dbi_smeta, MDB_dbi dbi_content, MDB_dbi dbi_author, MDB_dbi dbi_ext, MDB_dbi dbi_strrev, MDB_dbi dbi_date, IdVec* out) {
    if (!n) return;
    if (n->type == TOK_TERM) {
        switch (n->ttype) {
            case TERM_NAME: records_for_name(txn, dbi_trigram, dbi_fname, dbi_strings, dbi_smeta, n->text, out); break;
            case TERM_CONTENT: records_for_content(txn, dbi_trigram, dbi_content, n->text, out); break;
            case TERM_AUTHOR: records_for_author(txn, dbi_author, dbi_strrev, n->text, out); break;
            case TERM_EXT: records_for_ext(txn, dbi_ext, n->text, out); break;
        }
        sort_unique(out);
        return;
    }
    if (n->type == TOK_AND || n->type == TOK_OR) {
        IdVec L; idvec_init(&L);
        IdVec R; idvec_init(&R);
        eval_node(n->left, txn, dbi_strings, dbi_fname, dbi_trigram, dbi_smeta, dbi_content, dbi_author, dbi_ext, dbi_strrev, dbi_date, &L);
        eval_node(n->right, txn, dbi_strings, dbi_fname, dbi_trigram, dbi_smeta, dbi_content, dbi_author, dbi_ext, dbi_strrev, dbi_date, &R);
        sort_unique(&L); sort_unique(&R);
        if (n->type == TOK_AND) { intersect_inplace(&L, &R); } else { union_inplace(&L, &R); sort_unique(&L); }
        idvec_free(&R);
        *out = L;
        return;
    }
    if (n->type == TOK_NOT) {
        IdVec B; idvec_init(&B);
        eval_node(n->left, txn, dbi_strings, dbi_fname, dbi_trigram, dbi_smeta, dbi_content, dbi_author, dbi_ext, dbi_strrev, dbi_date, &B);
        sort_unique(&B);
        IdVec All; idvec_init(&All);
        DiffCtx ctx{ &B, &All };
        stream_all_records(txn, dbi_date, diff_collect, &ctx);
        idvec_free(&B);
        *out = All;
        return;
    }
}

struct SearchThreadArgs {
    MDB_env* env;
    std::string query;
    Filters filters;
    std::vector<Result>* results;
    bool* done;
    std::mutex* mutex;
    char db_path[MAX_PATH * 3];
};

static void search_thread(SearchThreadArgs* sta) {
    MDB_txn* txn = nullptr;
    mdb_txn_begin(sta->env, nullptr, MDB_RDONLY, &txn);
    MDB_dbi dbi_strings, dbi_records, dbi_fname_index, dbi_trigram, dbi_size, dbi_date, dbi_ext, dbi_smeta, dbi_content, dbi_author, dbi_strrev;
    mdb_dbi_open(txn, "strings", 0, &dbi_strings);
    mdb_dbi_open(txn, "records", 0, &dbi_records);
    mdb_dbi_open(txn, "filename_index", 0, &dbi_fname_index);
    mdb_dbi_open(txn, "trigram_index", 0, &dbi_trigram);
    mdb_dbi_open(txn, "size_index", 0, &dbi_size);
    mdb_dbi_open(txn, "date_index", 0, &dbi_date);
    mdb_dbi_open(txn, "extension_index", 0, &dbi_ext);
    mdb_dbi_open(txn, "string_meta", 0, &dbi_smeta);
    mdb_dbi_open(txn, "content_index", 0, &dbi_content);
    mdb_dbi_open(txn, "author_index", 0, &dbi_author);
    mdb_dbi_open(txn, "strrev", 0, &dbi_strrev);
    TokenList tokens;
    parse_query(sta->query, &tokens);
    Node* root = parse_tokens(tokens);
    IdVec rec_ids; idvec_init(&rec_ids);
    if (root) {
        eval_node(root, txn, dbi_strings, dbi_fname_index, dbi_trigram, dbi_smeta, dbi_content, dbi_author, dbi_ext, dbi_strrev, dbi_date, &rec_ids);
        free_node(root);
    } else {
        stream_all_records(txn, dbi_date, collect_record, &rec_ids);
    }
    sort_unique(&rec_ids);
    MDB_stat st; mdb_stat(txn, dbi_records, &st);
    size_t total_docs = st.ms_entries;
    size_t docs_with_term = rec_ids.n;
    std::vector<RankedResult> ranked(rec_ids.n);
    size_t rankedn = 0;
    SearchQuery sq;
    sq.name_pattern = (char*)sta->query.c_str();
    sq.path_filter = (char*)sta->filters.path_filter.c_str();
    sq.ext_pattern = (char*)sta->filters.ext.c_str();
    sq.size_min = (uint64_t)(sta->filters.min_size_mb * 1024 * 1024);
    sq.size_max = (sta->filters.max_size_mb >= 0) ? (uint64_t)(sta->filters.max_size_mb * 1024 * 1024) : ~0ULL;
    sq.date_min_day = sta->filters.use_min_date ? get_date_time(sta->filters.min_date) / 86400 : 0;
    sq.date_max_day = sta->filters.use_max_date ? get_date_time(sta->filters.max_date) / 86400 : ~0ULL;
    sq.regex_mode = sta->filters.regex_mode;
    sq.whole_word = sta->filters.whole_word;
    for (size_t i = 0; i < rec_ids.n; i++) {
        uint64_t rid = rec_ids.ids[i];
        MDB_val rk = {.mv_data = &rid, .mv_size = sizeof(rid)};
        MDB_val rv;
        if (mdb_get(txn, dbi_records, &rk, &rv) != 0 || rv.mv_size < sizeof(DbRecord)) continue;
        DbRecord* r = (DbRecord*)rv.mv_data;
        if (r->file_size < sq.size_min || r->file_size > sq.size_max) continue;
        uint64_t day = filetime_days(r->modified_time);
        if (day < sq.date_min_day || day > sq.date_max_day) continue;
        MDB_val pk = {.mv_data = &r->parent_str_id, .mv_size = sizeof(r->parent_str_id)};
        MDB_val pv;
        MDB_val nk = {.mv_data = &r->name_str_id, .mv_size = sizeof(r->name_str_id)};
        MDB_val nv;
        if (mdb_get(txn, dbi_strings, &pk, &pv) != 0) continue;
        if (mdb_get(txn, dbi_strings, &nk, &nv) != 0) continue;
        std::string parent = std::string((char*)pv.mv_data, pv.mv_size);
        std::string name = std::string((char*)nv.mv_data, nv.mv_size);
        std::string lower_parent = to_lower(parent);
        if (!sq.path_filter.empty() && lower_parent.find(to_lower(sq.path_filter)) == std::string::npos) continue;
        std::string ext = "";
        size_t dot = name.rfind('.');
        if (dot != std::string::npos) ext = to_lower(name.substr(dot + 1));
        if (!sq.ext_pattern.empty() && ext != to_lower(sq.ext_pattern)) continue;
        float score = calculate_relevance(txn, dbi_strings, r, parent.c_str(), name.c_str(), &sq, total_docs, docs_with_term);
        ranked[rankedn].rec_id = rid;
        ranked[rankedn].score = score;
        rankedn++;
    }
    qsort(ranked.data(), rankedn, sizeof(RankedResult), cmp_rank);
    {
        std::lock_guard<std::mutex> guard(*sta->mutex);
        sta->results->clear();
        for (size_t i = 0; i < rankedn && i < 1000; i++) {
            uint64_t rid = ranked[i].rec_id;
            MDB_val rk = {.mv_data = &rid, .mv_size = sizeof(rid)};
            MDB_val rv;
            mdb_get(txn, dbi_records, &rk, &rv);
            DbRecord* r = (DbRecord*)rv.mv_data;
            MDB_val pv, nv;
            MDB_val pk = {.mv_data = &r->parent_str_id, .mv_size = sizeof(r->parent_str_id)};
            MDB_val nk = {.mv_data = &r->name_str_id, .mv_size = sizeof(r->name_str_id)};
            mdb_get(txn, dbi_strings, &pk, &pv);
            mdb_get(txn, dbi_strings, &nk, &nv);
            std::string path = std::string((char*)pv.mv_data, pv.mv_size);
            std::string filename = std::string((char*)nv.mv_data, nv.mv_size);
            std::string snippet = "";
            if (r->content_str_id) {
                MDB_val ck = {.mv_data = &r->content_str_id, .mv_size = sizeof(r->content_str_id)};
                MDB_val cv;
                mdb_get(txn, dbi_strings, &ck, &cv);
                snippet = std::string((char*)cv.mv_data, cv.mv_size).substr(0, 500);
            }
            std::string type = "text";
            size_t dot = filename.rfind('.');
            if (dot != std::string::npos) {
                std::string e = to_lower(filename.substr(dot + 1));
                if (e == "jpg" || e == "png" || e == "gif" || e == "bmp") type = "image";
                else if (e == "pdf") type = "pdf";
            }
            sta->results->push_back({filename, path, snippet, (int64_t)r->file_size, (time_t)(r->modified_time / 10000000 - 11644473600LL), type, ranked[i].score});
        }
        *sta->done = true;
    }
    mdb_txn_abort(txn);
}

#endif

// run_ui - launch the GUI if ImGui is available
int run_ui(void){
#ifdef HAS_IMGUI
    if(!glfwInit()){
        fprintf(stderr, "Failed to init GLFW\n");
        return -1;
    }
    const char* glsl_version = "#version 130";
    GLFWwindow* window = glfwCreateWindow(1280, 800, "Anything", NULL, NULL);
    if(!window){
        fprintf(stderr, "Failed to create window\n");
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    std::string query_str;
#ifdef _WIN32
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if(argv && argc > 1 && wcsncmp(argv[1], L"anything:", 9) == 0){
            char tmp[512];
            to_utf8(argv[1] + 9, tmp, sizeof(tmp));
            query_str = tmp;
        }
        if(argv) LocalFree(argv);
    }
#endif
    std::vector<Result> filtered;
    int selected = -1;
    bool show_advanced = false;
    Filters filters;
    bool taskbar_integration = taskbar_search_is_enabled();
    int theme_idx = 0; // 0 = Dark, 1 = Light
    bool need_update = true;
    bool need_sort = true;
    Db* db = nullptr;
    const DbHeader* header;
#ifdef _WIN32
    header = db_open_readonly(L"anything.mdb", &db);
    char u8db[MAX_PATH*3];
    to_utf8(L"anything.mdb", u8db, sizeof(u8db));
    open_bloom(L"anything.mdb");
#else
    wchar_t wdb[MAX_PATH];
    mbstowcs(wdb, "anything.mdb", MAX_PATH);
    header = db_open_readonly(wdb, &db);
    char u8db[MAX_PATH*3];
    strncpy(u8db, "anything.mdb", sizeof(u8db));
    open_bloom(wdb);
#endif
    if (!header) {
        fprintf(stderr, "Failed to open DB\n");
    }
    MDB_env* env = nullptr;
    mdb_env_create(&env);
    mdb_env_open(env, u8db, MDB_RDONLY, 0664);
    std::thread search_th;
    SearchThreadArgs sta;
    bool search_done = true;
    std::mutex search_lock;
    sta.env = env;
    sta.results = &filtered;
    sta.done = &search_done;
    sta.mutex = &search_lock;
    strcpy(sta.db_path, u8db);

    std::vector<Result> all_items; // Dummy if needed

    live_updates_init();

    auto update_results = [&]() {
        if (search_th.joinable()) {
            search_th.join();
        }
        search_done = false;
        sta.query = query_str;
        sta.filters = filters;
        search_th = std::thread(search_thread, &sta);
        need_sort = true;
    };

    static const char* months[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };

    while(!glfwWindowShouldClose(window)){
        LiveUpdate lu;
        while(live_updates_poll(&lu)) { need_update = true; }
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("Anything Search", nullptr, ImGuiWindowFlags_None);
        if (ImGui::BeginTabBar("MainTabs")) {
            if (ImGui::BeginTabItem("Search")) {
                ImGui::InputText("Query", &query_str);
                bool query_edited = ImGui::IsItemEdited();
                ImGui::SameLine();
                if (ImGui::Button("Advanced")) show_advanced = !show_advanced;
                if (query_edited) need_update = true;

                ImGui::BeginChild("ResultsAndPreview", ImVec2(0, 0), false);
                if (ImGui::BeginTable("split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings)) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("Results:");
                    ImGui::Separator();
                    bool results_focused = false;
                    ImGui::BeginChild("ResultsList", ImVec2(0, 0), false, ImGuiWindowFlags_None);
                    results_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
                    ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoSavedSettings;
                    if (ImGui::BeginTable("ResultsTable", 6, tflags)) {
                        ImGui::TableSetupScrollFreeze(0,1);
                        ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, 60.0f);
                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort, 0.0f, COL_NAME);
                        ImGui::TableSetupColumn("Path", 0, 0.0f, COL_PATH);
                        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, COL_SIZE);
                        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, COL_MOD);
                        ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, COL_SCORE);
                        ImGui::TableHeadersRow();
                        ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs();
                        if (sort_specs && (sort_specs->SpecsDirty || need_sort)) {
                            apply_sort(filtered, sort_specs);
                            sort_specs->SpecsDirty = false;
                            need_sort = false;
                        }
                        for (size_t i = 0; i < filtered.size(); ++i) {
                            Result& r = filtered[i];
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(1);
                            ImGui::PushID((int)i);
                            bool is_selected = (selected == (int)i);
                            if (ImGui::Selectable(r.filename.c_str(), is_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                                selected = (int)i;
                            }
                            bool hovered = ImGui::IsItemHovered();
                            if ((hovered || is_selected) && r.type == "image" && r.texture == 0) {
                                load_result_texture(r);
                            }
                            if (ImGui::BeginPopupContextItem()) {
                                std::string full = r.path + "\\" + r.filename;
                                if (ImGui::MenuItem("Open")) open_file_os(full);
                                if (ImGui::MenuItem("Open Folder")) open_folder_os(r.path);
                                if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(full.c_str());
                                if (ImGui::MenuItem("Delete")) { delete_path_os(full); need_update = true; }
                                ImGui::EndPopup();
                            }
                            ImGui::TableSetColumnIndex(0);
                            if (r.type == "image" && r.texture != 0) {
                                ImGui::Image((ImTextureID)(intptr_t)r.texture, ImVec2(48, 48));
                            }
                            ImGui::TableSetColumnIndex(2);
                            ImGui::TextUnformatted(r.path.c_str());
                            ImGui::TableSetColumnIndex(3);
                            ImGui::Text("%lld", (long long)r.size);
                            ImGui::TableSetColumnIndex(4);
                            char dstr[64];
                            std::strftime(dstr, sizeof(dstr), "%Y-%m-%d %H:%M:%S", std::localtime(&r.modified));
                            ImGui::Text("%s", dstr);
                            ImGui::TableSetColumnIndex(5);
                            ImGui::Text("%.1f", r.score);
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndChild();

                    if (results_focused) {
                        ImGuiIO& io = ImGui::GetIO();
                        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && selected > 0) selected--;
                        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && selected + 1 < (int)filtered.size()) selected++;
                        if (selected >= 0 && selected < (int)filtered.size()) {
                            const Result& r = filtered[selected];
                            std::string full = r.path + "\\" + r.filename;
                            if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                                open_file_os(full);
                            }
                            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) {
                                if (io.KeyShift) open_folder_os(r.path);
                                else open_file_os(full);
                            }
                            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
                                ImGui::SetClipboardText(full.c_str());
                            }
                            if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                                delete_path_os(full);
                                need_update = true;
                            }
                        }
                    }

                    ImGui::TableNextColumn();
                    ImGui::BeginChild("QuickViewPane", ImVec2(0, 0), false, ImGuiWindowFlags_None);
                    if (selected >= 0 && selected < static_cast<int>(filtered.size())) {
                        const Result& r = filtered[selected];
                        if (ImGui::BeginTabBar("QuickViewTabs")) {
                            if (ImGui::BeginTabItem("Info")) {
                                ImGui::Text("Path: %s", r.path.c_str());
                                ImGui::Text("Size: %lld bytes", r.size);
                                char date_str[64];
                                std::strftime(date_str, sizeof(date_str), "%Y-%m-%d %H:%M:%S", std::localtime(&r.modified));
                                ImGui::Text("Modified: %s", date_str);
                                ImGui::EndTabItem();
                            }
                            if (ImGui::BeginTabItem("Preview")) {
                                if (r.type == "text" || r.type == "pdf") {
                                    draw_highlighted(r.snippet, query_str);
                                } else if (r.type == "image") {
                                    if (r.texture != 0) {
                                        float aspect = static_cast<float>(r.height) / static_cast<float>(r.width);
                                        float preview_width = ImGui::GetContentRegionAvail().x;
                                        ImGui::Image((ImTextureID)(intptr_t)r.texture, ImVec2(preview_width, preview_width * aspect));
                                    } else {
                                        ImGui::Text("No thumbnail loaded.");
                                    }
                                }
                                ImGui::EndTabItem();
                            }
                            ImGui::EndTabBar();
                        }
                    }
                    ImGui::EndChild();
                    ImGui::EndTable();
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Duplicates")) {
                ImGuiTableFlags dflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings;
                if (ImGui::BeginTable("DupTable", 4, dflags)) {
                    ImGui::TableSetupColumn("Path");
                    ImGui::TableSetupColumn("Size");
                    ImGui::TableSetupColumn("Hash");
                    ImGui::TableSetupColumn("Diff Preview");
                    ImGui::TableHeadersRow();
                    for (auto& d : g_duplicates) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Selectable(d.path.c_str(), &d.selected, ImGuiSelectableFlags_SpanAllColumns);
                        ImGui::TableNextColumn();
                        ImGui::Text("%llu", (unsigned long long)d.size);
                        ImGui::TableNextColumn();
                        ImGui::Text("%016llX", (unsigned long long)d.hash);
                        ImGui::TableNextColumn();
                        ImGui::Text("(preview)");
                    }
                    ImGui::EndTable();
                }
                if (ImGui::Button("Delete Selected")) {
                    g_duplicates.erase(
                        std::remove_if(g_duplicates.begin(), g_duplicates.end(),
                            [](const DuplicateItem& d){
                                if(d.selected){
                                    delete_path_os(d.path);
                                    return true;
                                }
                                return false;
                            }),
                        g_duplicates.end());
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings")) {
                const char* themes[] = { "Dark", "Light" };
                if (ImGui::Combo("Theme", &theme_idx, themes, IM_ARRAYSIZE(themes))) {
                    if (theme_idx == 0) ImGui::StyleColorsDark();
                    else ImGui::StyleColorsLight();
                }
#ifdef _WIN32
                if (ImGui::Checkbox("Enable Taskbar Search integration", &taskbar_integration)) {
                    set_taskbar_search(taskbar_integration);
                }
#endif
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();

        if (show_advanced) {
            bool advanced_changed = false;
            ImGui::Begin("Advanced Search Builder", &show_advanced);
            advanced_changed |= ImGui::InputText("Extension (without .)", &filters.ext);
            advanced_changed |= ImGui::SliderFloat("Min Size (MB)", &filters.min_size_mb, 0.0f, 1000.0f, "%.1f");
            advanced_changed |= ImGui::SliderFloat("Max Size (MB, -1 for no max)", &filters.max_size_mb, -1.0f, 1000.0f, "%.1f");
            advanced_changed |= ImGui::Checkbox("Use Min Date", &filters.use_min_date);
            if (filters.use_min_date) {
                advanced_changed |= ImGui::Combo("Min Month", &filters.min_date.month, months, IM_ARRAYSIZE(months));
                advanced_changed |= ImGui::InputInt("Min Day", &filters.min_date.day);
                if (filters.min_date.day < 1) filters.min_date.day = 1;
                if (filters.min_date.day > 31) filters.min_date.day = 31;
                advanced_changed |= ImGui::InputInt("Min Year", &filters.min_date.year);
            }
            advanced_changed |= ImGui::Checkbox("Use Max Date", &filters.use_max_date);
            if (filters.use_max_date) {
                advanced_changed |= ImGui::Combo("Max Month", &filters.max_date.month, months, IM_ARRAYSIZE(months));
                advanced_changed |= ImGui::InputInt("Max Day", &filters.max_date.day);
                if (filters.max_date.day < 1) filters.max_date.day = 1;
                if (filters.max_date.day > 31) filters.max_date.day = 31;
                advanced_changed |= ImGui::InputInt("Max Year", &filters.max_date.year);
            }
            advanced_changed |= ImGui::InputText("Path Filter", &filters.path_filter);
            advanced_changed |= ImGui::Checkbox("Regex Mode", &filters.regex_mode);
            advanced_changed |= ImGui::Checkbox("Whole Word", &filters.whole_word);
            ImGui::End();
            if (advanced_changed) need_update = true;
        }

        if (need_update) {
            update_results();
            need_update = false;
        } else if (search_th.joinable() && search_done) {
            search_th.join();
            need_sort = true;
        }

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    if (search_th.joinable()) {
        search_th.join();
    }

    for (auto& r : all_items) {
        if (r.texture != 0) {
            glDeleteTextures(1, &r.texture);
            r.texture = 0;
        }
    }
    for (auto& r : filtered) {
        if (r.texture != 0) {
            glDeleteTextures(1, &r.texture);
            r.texture = 0;
        }
    }

    db_close(db);
    mdb_env_close(env);
    close_bloom();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
#else
    fprintf(stderr, "ImGui not available - GUI disabled\n");
    return -1;
#endif
}

