#include "plugin.h"
#include <wchar.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static PluginHost g_host;

static void append_token(char** buf, size_t* len, size_t* cap, const char* type, const char* name){
    if(!name || !*name) return;
    size_t need = strlen(type) + strlen(name) + 2; // type:name\n
    if(*len + need >= *cap){
        size_t new_cap = (*cap + need) * 2;
        char* new_buf = (char*)realloc(*buf, new_cap);
        if(!new_buf) return;
        *buf = new_buf; *cap = new_cap;
    }
    int written = sprintf(*buf + *len, "%s:%s\n", type, name);
    if(written > 0) *len += written;
}

static void process_line(char* line, char** buf, size_t* len, size_t* cap){
    char* p = line;
    while(*p && isspace((unsigned char)*p)) p++;
    if(!*p) return;
    if(*p=='#' || (*p=='/' && p[1]=='/') || (*p=='/' && p[1]=='*') || *p=='*') return;

    // --- Direct keyword-based detections ---
    if(strncmp(p, "def ", 4)==0){
        char* name = p+4; while(*name && isspace((unsigned char)*name)) name++;
        char* end = name; while(*end && (isalnum((unsigned char)*end) || *end=='_')) end++;
        char tmp = *end; *end=0;
        append_token(buf,len,cap,"function",name);
        *end = tmp;
        return;
    }
    if(strncmp(p, "class ", 6)==0){
        char* name = p+6; while(*name && isspace((unsigned char)*name)) name++;
        char* end = name; while(*end && (isalnum((unsigned char)*end) || *end=='_')) end++;
        char tmp = *end; *end=0;
        append_token(buf,len,cap,"class",name);
        *end = tmp;
        return;
    }
    if(strncmp(p, "struct ", 7)==0 || strncmp(p, "enum ", 5)==0 || strncmp(p, "interface ",10)==0 || strncmp(p,"trait ",6)==0){
        char* name = strchr(p,' ');
        if(name){
            while(*name && isspace((unsigned char)*name)) name++;
            char* end = name; while(*end && (isalnum((unsigned char)*end) || *end=='_')) end++;
            char tmp = *end; *end=0;
            append_token(buf,len,cap,"class",name);
            *end = tmp; return;
        }
    }
    if(strncmp(p, "type ",5)==0){
        char* name = p+5; while(*name && isspace((unsigned char)*name)) name++;
        char* end = name; while(*end && (isalnum((unsigned char)*end) || *end=='_')) end++;
        char tmp = *end; *end=0;
        char* after = end; while(*after && isspace((unsigned char)*after)) after++;
        if(!strncmp(after,"struct",6) || !strncmp(after,"interface",9)){
            append_token(buf,len,cap,"class",name);
            *end = tmp; return;
        }
        *end = tmp;
    }
    if(strncmp(p, "fn ",3)==0){
        char* name = p+3; while(*name && isspace((unsigned char)*name)) name++;
        char* end = name; while(*end && (isalnum((unsigned char)*end) || *end=='_')) end++;
        char tmp = *end; *end=0; append_token(buf,len,cap,"function",name); *end=tmp; return;
    }
    if(strncmp(p, "func ",5)==0){
        char* name = p+5;
        if(*name=='('){ // skip receiver
            name = strchr(name,')');
            if(!name) return; name++;
        }
        while(*name && isspace((unsigned char)*name)) name++;
        char* end = name; while(*end && (isalnum((unsigned char)*end) || *end=='_')) end++;
        char tmp = *end; *end=0; append_token(buf,len,cap,"function",name); *end=tmp; return;
    }
    if(!strncmp(p,"Function ",9) || !strncmp(p,"Sub ",4)){
        char* name = strchr(p,' ');
        if(name){
            while(*name && isspace((unsigned char)*name)) name++;
            char* end = name; while(*end && (isalnum((unsigned char)*end) || *end=='_')) end++;
            char tmp = *end; *end=0; append_token(buf,len,cap,"function",name); *end=tmp; return;
        }
    }

    // variable definitions with keywords
    if(!strncmp(p,"let ",4) || !strncmp(p,"var ",4) || !strncmp(p,"const ",6) || !strncmp(p,"Dim ",4)){
        char* name = strchr(p,' ');
        if(name){
            while(*name && isspace((unsigned char)*name)) name++;
            char* end = name; while(*end && (isalnum((unsigned char)*end) || *end=='_')) end++;
            char tmp = *end; *end=0; append_token(buf,len,cap,"var",name); *end=tmp; return;
        }
    }

    // generic function detection for C-like languages
    char* open = strchr(p,'(');
    char* brace = strchr(p,'{');
    if(open && brace && brace>open){
        // find name before '('
        char* q = open-1;
        while(q>p && isspace((unsigned char)*q)) q--;
        while(q>p && (isalnum((unsigned char)*q) || *q=='_')) q--;
        if(!isalnum((unsigned char)*q) && *q!='_') q++;
        char fname[128]; size_t n=0;
        while(q<open && n<sizeof(fname)-1 && (isalnum((unsigned char)*q) || *q=='_')) fname[n++]=*q++;
        fname[n]=0;
        if(fname[0] && strcmp(fname,"if") && strcmp(fname,"for") && strcmp(fname,"while") && strcmp(fname,"switch") && strcmp(fname,"catch")){
            append_token(buf,len,cap,"function",fname);
            return;
        }
    }

    // variable from assignment ( = or := )
    char* colon = strstr(p,":=");
    if(colon){
        char* q = colon-1; while(q>p && isspace((unsigned char)*q)) q--;
        while(q>p && (isalnum((unsigned char)*q) || *q=='_')) q--;
        if(!isalnum((unsigned char)*q) && *q!='_') q++;
        char vname[128]; size_t n=0;
        while(q<colon && n<sizeof(vname)-1 && (isalnum((unsigned char)*q) || *q=='_')) vname[n++]=*q++;
        vname[n]=0; append_token(buf,len,cap,"var",vname); return;
    }
    char* eq = strchr(p,'=');
    if(eq && !(eq[1]=='=' || eq[-1]=='=' || eq[-1]=='!' || eq[-1]=='<' || eq[-1]=='>')){
        char* q = eq-1; while(q>p && isspace((unsigned char)*q)) q--;
        while(q>p && (isalnum((unsigned char)*q) || *q=='_')) q--;
        if(!isalnum((unsigned char)*q) && *q!='_') q++;
        char vname[128]; size_t n=0; while(q<eq && n<sizeof(vname)-1 && (isalnum((unsigned char)*q) || *q=='_')) vname[n++]=*q++;
        vname[n]=0; append_token(buf,len,cap,"var",vname); return;
    }
}

static wchar_t* parse_file(const wchar_t* path){
    FILE* f = _wfopen(path, L"rb");
    if(!f) return NULL;
    fseek(f,0,SEEK_END); long len = ftell(f); fseek(f,0,SEEK_SET);
    char* data = (char*)malloc(len+1);
    if(!data){ fclose(f); return NULL; }
    fread(data,1,len,f); data[len]=0; fclose(f);

    size_t cap = len*2 + 1; size_t l=0; char* out = (char*)malloc(cap);
    if(!out){ free(data); return NULL; }
    out[0]=0;

    char* ctx=NULL; char* line = strtok_s(data, "\n\r", &ctx);
    while(line){
        process_line(line,&out,&l,&cap);
        line = strtok_s(NULL, "\n\r", &ctx);
    }

    int wlen = MultiByteToWideChar(CP_UTF8,0,out,-1,NULL,0);
    wchar_t* wout = (wchar_t*)malloc(sizeof(wchar_t)*wlen);
    if(wout) MultiByteToWideChar(CP_UTF8,0,out,-1,wout,wlen);
    free(out); free(data);
    return wout;
}

static BOOL init(const PluginHost* host){
    g_host = *host; return TRUE;
}

static BOOL has_ext(const wchar_t* ext){
    const wchar_t* exts[] = {L"c",L"h",L"cpp",L"cc",L"hpp",L"rs",L"go",L"cs",L"vb",L"java",L"py"};
    for(size_t i=0;i<sizeof(exts)/sizeof(exts[0]);i++) if(_wcsicmp(ext,exts[i])==0) return TRUE;
    return FALSE;
}

static void scan(void){
    const wchar_t* root = L"code"; // folder containing source files
    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%s\\*.*", root);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pattern,&fd);
    if(h==INVALID_HANDLE_VALUE) return;
    do{
        if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const wchar_t* ext = wcsrchr(fd.cFileName, L'.');
        if(!ext) continue; ext++;
        if(!has_ext(ext)) continue;
        wchar_t full[MAX_PATH]; _snwprintf(full, MAX_PATH, L"%s\\%s", root, fd.cFileName);
        wchar_t* content = parse_file(full);
        if(!content) continue;
        DbWorkItem* wi = (DbWorkItem*)_aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
        if(!wi){ free(content); continue; }
        wi->content = content;
        wi->preview = NULL;
        wcscpy_s(wi->parent_path, MAX_LONG_PATH, root);
        wcscpy_s(wi->name, MAX_PATH, fd.cFileName);
        ULARGE_INTEGER s; s.LowPart = fd.nFileSizeLow; s.HighPart = fd.nFileSizeHigh; wi->file_size = s.QuadPart;
        wi->creation_time = ((ULARGE_INTEGER){fd.ftCreationTime.dwLowDateTime, fd.ftCreationTime.dwHighDateTime}).QuadPart;
        wi->modified_time = ((ULARGE_INTEGER){fd.ftLastWriteTime.dwLowDateTime, fd.ftLastWriteTime.dwHighDateTime}).QuadPart;
        wi->access_time   = ((ULARGE_INTEGER){fd.ftLastAccessTime.dwLowDateTime, fd.ftLastAccessTime.dwHighDateTime}).QuadPart;
        wi->attributes = fd.dwFileAttributes;
        wi->op = WI_ADD;
        while(!MPMC_Push(g_host.queue, wi)) Sleep(0);
    }while(FindNextFileW(h,&fd));
    FindClose(h);
}

static void shutdown(void){ }

static AnythingPlugin g_plugin = {
    ANYTHING_PLUGIN_API_VERSION,
    L"Code Parser Plugin",
    init,
    scan,
    shutdown
};

__declspec(dllexport) AnythingPlugin* Anything_GetPlugin(void){ return &g_plugin; }

