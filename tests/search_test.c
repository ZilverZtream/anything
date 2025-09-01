#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <wchar.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strdup strdup

#include "../util.h"

// --- Begin code copied from search.c for tokenization and query parsing ---

typedef struct {
    char* name_pattern;
    char* content_pattern;
    char* author_pattern;
    char* camera_pattern;
    char* lens_pattern;
    char* artist_pattern;
    char* album_pattern;
    char* title_pattern;
    char* ext_pattern;
    uint64_t size_min, size_max;
    uint64_t date_min_day, date_max_day;
    char* path_filter;   // utf8
    bool regex_mode;
    bool whole_word;
} SearchQuery;

typedef enum { TOK_TERM, TOK_AND, TOK_OR, TOK_NOT, TOK_LPAREN, TOK_RPAREN } TokType;
typedef enum { TERM_NAME, TERM_AUTHOR, TERM_CAMERA, TERM_LENS, TERM_ARTIST, TERM_ALBUM, TERM_TITLE, TERM_EXT, TERM_CONTENT } TermType;

typedef struct { TokType type; TermType ttype; char* text; } Token;
typedef struct { Token* items; int n, cap; } TokenList;

static void tokenlist_init(TokenList* t){ t->items=NULL; t->n=t->cap=0; }
static void tokenlist_push(TokenList* t, Token tk){ if(t->n==t->cap){ t->cap=t->cap?t->cap*2:64; t->items=(Token*)realloc(t->items,t->cap*sizeof(Token)); } t->items[t->n++]=tk; }
static void tokenlist_free(TokenList* t){ for(int i=0;i<t->n;i++){ if(t->items[i].type==TOK_TERM && t->items[i].text) free(t->items[i].text); } free(t->items); t->items=NULL; t->n=t->cap=0; }

static void add_logic_token(TokenList* toks, const char* s){
    if(!*s) return;
    if(_stricmp(s,"AND")==0){ tokenlist_push(toks,(Token){.type=TOK_AND}); return; }
    if(_stricmp(s,"OR")==0){ tokenlist_push(toks,(Token){.type=TOK_OR}); return; }
    if(_stricmp(s,"NOT")==0){ tokenlist_push(toks,(Token){.type=TOK_NOT}); return; }
    if(_strnicmp(s,"author:",7)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_AUTHOR,.text=_strdup(s+7)}); return; }
    if(_strnicmp(s,"camera:",7)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_CAMERA,.text=_strdup(s+7)}); return; }
    if(_strnicmp(s,"lens:",5)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_LENS,.text=_strdup(s+5)}); return; }
    if(_strnicmp(s,"artist:",7)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_ARTIST,.text=_strdup(s+7)}); return; }
    if(_strnicmp(s,"album:",6)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_ALBUM,.text=_strdup(s+6)}); return; }
    if(_strnicmp(s,"title:",6)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_TITLE,.text=_strdup(s+6)}); return; }
    if(_strnicmp(s,"ext:",4)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_EXT,.text=_strdup(s+4)}); return; }
    if(_strnicmp(s,"content:",8)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_CONTENT,.text=_strdup(s+8)}); return; }
    tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_NAME,.text=_strdup(s)});
}

static uint64_t parse_size(const char* s){
    char* end=NULL; double v=strtod(s,&end); uint64_t mult=1;
    if(end&&*end){ if(*end=='k'||*end=='K') mult=1024ULL; else if(*end=='m'||*end=='M') mult=1024ULL*1024ULL; else if(*end=='g'||*end=='G') mult=1024ULL*1024ULL*1024ULL; }
    return (uint64_t)(v*mult);
}

static uint64_t today_day(void){ return (uint64_t)(time(NULL)/86400); }

static BOOL parse_date(const char* s, uint64_t* out_day){
    if(_stricmp(s,"today")==0){ *out_day=today_day(); return TRUE; }
    if(_stricmp(s,"yesterday")==0){ *out_day=today_day()-1; return TRUE; }
    size_t n=strlen(s);
    if(n>1 && (s[n-1]=='d'||s[n-1]=='D')){ int days=atoi(s); *out_day=today_day()-(uint64_t)days; return TRUE; }
    int y=0,m=0,d=1;
    if(sscanf(s,"%d-%d-%d",&y,&m,&d)>=2){ struct tm tm={0}; tm.tm_year=y-1900; tm.tm_mon=m-1; tm.tm_mday=d; time_t t=timegm(&tm); if(t==(time_t)-1) return FALSE; *out_day=(uint64_t)(t/86400); return TRUE; }
    return FALSE;
}

static void parse_query(int argc, wchar_t** argv, wchar_t* dbPath, SearchQuery* q, TokenList* tokens){
    dbPath[0]=0; ZeroMemory(q,sizeof(*q));
    q->size_min=0; q->size_max=~0ULL;
    q->date_min_day=0; q->date_max_day=~0ULL;
    tokenlist_init(tokens);
    for(int i=1;i<argc;i++){
        if(wcscmp(argv[i], L"--db")==0 && i+1<argc){ wcscpy_s(dbPath, MAX_PATH, argv[++i]); continue; }
        if(wcscmp(argv[i], L"--start-indexer")==0 || wcscmp(argv[i], L"--pause-indexer")==0) continue;
        char u8[1024]; to_utf8(argv[i], u8, sizeof(u8));
        if(_strnicmp(u8,"size:",5)==0){
            const char* s=u8+5;
            if(s[0]=='>'||s[0]=='<'){
                BOOL gt=(s[0]=='>'); s++; uint64_t val=parse_size(s);
                if(gt){ q->size_min=val+1; } else { q->size_max=val-1; }
            } else {
                const char* dots=strstr(s,"..");
                if(dots){ char a[64]={0},b[64]={0}; strncpy(a,s,(size_t)(dots-s)); strcpy(b,dots+2); q->size_min=parse_size(a); q->size_max=parse_size(b); }
                else { q->size_min=q->size_max=parse_size(s); }
            }
        } else if(_strnicmp(u8,"dm:",3)==0){
            const char* s=u8+3;
            if(s[0]=='>'||s[0]=='<'){
                BOOL gt=(s[0]=='>'); s++; uint64_t day; if(parse_date(s,&day)){ if(gt){ q->date_min_day=day+1; } else { q->date_max_day=day-1; } }
            } else {
                const char* dots=strstr(s,"..");
                if(dots){ char a[64]={0},b[64]={0}; strncpy(a,s,(size_t)(dots-s)); strcpy(b,dots+2); uint64_t da,db; if(parse_date(a,&da)&&parse_date(b,&db)){ q->date_min_day=da; q->date_max_day=db; } }
                else { uint64_t d; if(parse_date(s,&d)){ q->date_min_day=d; q->date_max_day=d; } }
            }
        } else if(_strnicmp(u8,"path:",5)==0){
            q->path_filter=_strdup(u8+5); lowercase_ascii(q->path_filter,strlen(q->path_filter));
        } else if(_strnicmp(u8,"regex:",6)==0){
            q->regex_mode=true; q->name_pattern=_strdup(u8+6);
        } else if(_strnicmp(u8,"whole:",6)==0){
            q->whole_word=(_stricmp(u8+6,"yes")==0||_stricmp(u8+6,"true")==0);
        } else {
            char buf[1024]; int bi=0; for(size_t j=0; u8[j]; ++j){
                if(u8[j]=='(' || u8[j]==')'){
                    if(bi>0){ buf[bi]=0; add_logic_token(tokens, buf); bi=0; }
                    Token t={.type=(u8[j]=='(')?TOK_LPAREN:TOK_RPAREN}; tokenlist_push(tokens,t);
                } else { buf[bi++]=u8[j]; }
            }
            if(bi>0){ buf[bi]=0; add_logic_token(tokens, buf); }
        }
    }
    for(int i=0;i<tokens->n;i++){
        if(tokens->items[i].type!=TOK_TERM || !tokens->items[i].text) continue;
        if(!q->name_pattern && tokens->items[i].ttype==TERM_NAME) q->name_pattern=_strdup(tokens->items[i].text);
        if(!q->content_pattern && tokens->items[i].ttype==TERM_CONTENT) q->content_pattern=_strdup(tokens->items[i].text);
        if(!q->author_pattern && tokens->items[i].ttype==TERM_AUTHOR) q->author_pattern=_strdup(tokens->items[i].text);
        if(!q->camera_pattern && tokens->items[i].ttype==TERM_CAMERA) q->camera_pattern=_strdup(tokens->items[i].text);
        if(!q->lens_pattern && tokens->items[i].ttype==TERM_LENS) q->lens_pattern=_strdup(tokens->items[i].text);
        if(!q->artist_pattern && tokens->items[i].ttype==TERM_ARTIST) q->artist_pattern=_strdup(tokens->items[i].text);
        if(!q->album_pattern && tokens->items[i].ttype==TERM_ALBUM) q->album_pattern=_strdup(tokens->items[i].text);
        if(!q->title_pattern && tokens->items[i].ttype==TERM_TITLE) q->title_pattern=_strdup(tokens->items[i].text);
        if(!q->ext_pattern && tokens->items[i].ttype==TERM_EXT) q->ext_pattern=_strdup(tokens->items[i].text);
    }
}

static void free_search_query(SearchQuery* q){
    if(q->name_pattern) free(q->name_pattern);
    if(q->content_pattern) free(q->content_pattern);
    if(q->author_pattern) free(q->author_pattern);
    if(q->camera_pattern) free(q->camera_pattern);
    if(q->lens_pattern) free(q->lens_pattern);
    if(q->artist_pattern) free(q->artist_pattern);
    if(q->album_pattern) free(q->album_pattern);
    if(q->title_pattern) free(q->title_pattern);
    if(q->ext_pattern) free(q->ext_pattern);
    if(q->path_filter) free(q->path_filter);
}

typedef struct Node{ int type; TermType ttype; char* text; struct Node* left; struct Node* right; } Node;
static void free_node(Node* n){ if(!n)return; free_node(n->left); free_node(n->right); if(n->type==TOK_TERM && n->text) free(n->text); free(n); }

static int precedence(Token t){ if(t.type==TOK_NOT) return 3; if(t.type==TOK_AND) return 2; if(t.type==TOK_OR) return 1; return 0; }
static Node* make_leaf(Token t){ Node* n=(Node*)calloc(1,sizeof(Node)); n->type=TOK_TERM; n->ttype=t.ttype; n->text=t.text?_strdup(t.text):NULL; n->left=n->right=NULL; return n; }
static void apply_op(Token op, Node** stack, int* sp){ Node* n=(Node*)calloc(1,sizeof(Node)); if(op.type==TOK_NOT){ if(*sp<1){ free(n); return; } n->type=TOK_NOT; n->left=stack[--(*sp)]; n->right=NULL; } else { if(*sp<2){ free(n); return; } Node* r=stack[--(*sp)]; Node* l=stack[--(*sp)]; n->type=op.type; n->left=l; n->right=r; } stack[(*sp)++]=n; }

static Node* parse_tokens(const TokenList* toks){ Token opstack[256]; int os=0; Node* nodestack[256]; int ns=0; for(int i=0;i<toks->n;i++){ Token tk=toks->items[i]; if(tk.type==TOK_TERM){ nodestack[ns++]=make_leaf(tk); } else if(tk.type==TOK_AND || tk.type==TOK_OR || tk.type==TOK_NOT){ while(os>0 && opstack[os-1].type!=TOK_LPAREN && precedence(opstack[os-1])>=precedence(tk)){ apply_op(opstack[--os], nodestack, &ns); } opstack[os++]=tk; } else if(tk.type==TOK_LPAREN){ opstack[os++]=tk; } else if(tk.type==TOK_RPAREN){ while(os>0 && opstack[os-1].type!=TOK_LPAREN){ apply_op(opstack[--os], nodestack, &ns); } if(os>0 && opstack[os-1].type==TOK_LPAREN) os--; } }
    while(os>0){ apply_op(opstack[--os], nodestack, &ns); }
    if(ns!=1) return NULL; return nodestack[0]; }

// --- End copied code ---

static void test_parse_tokens_complex(){
    wchar_t* argv[] = { L"prog", L"termA", L"AND", L"(termB", L"OR", L"NOT", L"termC)" };
    int argc = sizeof(argv)/sizeof(argv[0]);
    wchar_t dbPath[MAX_PATH]; SearchQuery q; TokenList tokens;
    parse_query(argc, argv, dbPath, &q, &tokens);
    Node* root = parse_tokens(&tokens);
    assert(root && root->type==TOK_AND);
    assert(root->left && root->left->type==TOK_TERM && strcmp(root->left->text,"termA")==0);
    assert(root->right && root->right->type==TOK_OR);
    Node* orr = root->right;
    assert(orr->left && orr->left->type==TOK_TERM && strcmp(orr->left->text,"termB")==0);
    assert(orr->right && orr->right->type==TOK_NOT);
    assert(orr->right->left && orr->right->left->type==TOK_TERM && strcmp(orr->right->left->text,"termC")==0);
    free_node(root); tokenlist_free(&tokens); free_search_query(&q);
}

static void test_parse_tokens_precedence(){
    wchar_t* argv[] = { L"prog", L"termA", L"OR", L"termB", L"AND", L"termC" };
    int argc = sizeof(argv)/sizeof(argv[0]);
    wchar_t dbPath[MAX_PATH]; SearchQuery q; TokenList tokens;
    parse_query(argc, argv, dbPath, &q, &tokens);
    Node* root = parse_tokens(&tokens);
    assert(root && root->type==TOK_OR);
    assert(root->left && root->left->type==TOK_TERM && strcmp(root->left->text,"termA")==0);
    assert(root->right && root->right->type==TOK_AND);
    assert(root->right->left && root->right->left->type==TOK_TERM && strcmp(root->right->left->text,"termB")==0);
    assert(root->right->right && root->right->right->type==TOK_TERM && strcmp(root->right->right->text,"termC")==0);
    free_node(root); tokenlist_free(&tokens); free_search_query(&q);
}

static void test_filter_size(){
    wchar_t* argv[] = { L"prog", L"size:>1mb" };
    int argc = 2; wchar_t dbPath[MAX_PATH]; SearchQuery q; TokenList tokens;
    parse_query(argc, argv, dbPath, &q, &tokens);
    assert(q.size_min == parse_size("1mb") + 1);
    assert(q.size_max == ~0ULL);
    tokenlist_free(&tokens); free_search_query(&q);
}

static void test_filter_dm(){
    wchar_t* argv[] = { L"prog", L"dm:yesterday" };
    int argc = 2; wchar_t dbPath[MAX_PATH]; SearchQuery q; TokenList tokens;
    uint64_t today = today_day();
    parse_query(argc, argv, dbPath, &q, &tokens);
    assert(q.date_min_day == today-1);
    assert(q.date_max_day == today-1);
    tokenlist_free(&tokens); free_search_query(&q);
}

static void test_parse_tokens_syntax_error(){
    wchar_t* argv[] = { L"prog", L"termA", L")", L"termB" };
    int argc = sizeof(argv)/sizeof(argv[0]);
    wchar_t dbPath[MAX_PATH]; SearchQuery q; TokenList tokens;
    parse_query(argc, argv, dbPath, &q, &tokens);
    Node* root = parse_tokens(&tokens);
    assert(root == NULL);
    tokenlist_free(&tokens); free_search_query(&q);
}

int main(){
    test_parse_tokens_complex();
    test_parse_tokens_precedence();
    test_filter_size();
    test_filter_dm();
    test_parse_tokens_syntax_error();
    printf("All search tests passed\n");
    return 0;
}

