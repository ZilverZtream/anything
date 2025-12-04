// office_parser.c - native parsing of modern Office files (.docx, .xlsx, .pptx)
// Modern Office files are ZIP containers with XML content.
// This parser extracts text directly from the XML, avoiding slow IFilter COM calls.

#include "core/pch.h"
#include <ctype.h>

// Maximum content size to prevent memory exhaustion
#ifndef MAX_INDEXED_CONTENT
#define MAX_INDEXED_CONTENT (128 * 1024 * 1024)
#endif

// Helper: convert UTF-8 string to wide string
static wchar_t* utf8_to_wide(const char* utf8){
    if(!utf8) return NULL;
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if(wlen <= 0) return NULL;
    wchar_t* wbuf = (wchar_t*)malloc(sizeof(wchar_t) * wlen);
    if(!wbuf) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, wlen);
    return wbuf;
#else
    size_t len = strlen(utf8);
    wchar_t* wbuf = (wchar_t*)malloc(sizeof(wchar_t) * (len + 1));
    if(!wbuf) return NULL;
    mbstowcs(wbuf, utf8, len + 1);
    return wbuf;
#endif
}

// Helper: convert wide string to UTF-8
static void to_utf8(const wchar_t* wide, char* out, size_t outcch){
    if(!wide || !out || outcch == 0) return;
#ifdef _WIN32
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)outcch, NULL, NULL);
#else
    wcstombs(out, wide, outcch);
#endif
}

// Helper: extract text content from XML tags
// This is a simple parser that extracts text between > and < characters
// while respecting XML structure and avoiding tag content
static void extract_text_from_xml(const char* xml, char** buf, size_t* len, size_t* cap){
    if(!xml || !buf || !len || !cap) return;

    const char* p = xml;
    bool in_tag = false;
    bool in_text = false;

    while(*p){
        if(*p == '<'){
            in_tag = true;
            in_text = false;
            p++;
            continue;
        }

        if(*p == '>'){
            in_tag = false;
            p++;
            // Skip if we're entering a tag that doesn't contain text
            if(*p && !isspace((unsigned char)*p)){
                in_text = true;
            }
            continue;
        }

        if(in_tag){
            p++;
            continue;
        }

        // Extract text content
        if(!in_tag && *p){
            const char* text_start = p;
            while(*p && *p != '<'){
                p++;
            }

            size_t text_len = p - text_start;
            if(text_len > 0){
                // Check if we need to expand buffer
                if(*len + text_len + 2 > MAX_INDEXED_CONTENT){
                    return; // Hit size limit
                }

                if(*len + text_len + 2 > *cap){
                    size_t new_cap = (*cap + text_len + 2) * 2;
                    if(new_cap > MAX_INDEXED_CONTENT) new_cap = MAX_INDEXED_CONTENT;
                    char* tmp = (char*)realloc(*buf, new_cap);
                    if(!tmp) return;
                    *buf = tmp;
                    *cap = new_cap;
                }

                // Copy text and add space separator
                memcpy(*buf + *len, text_start, text_len);
                *len += text_len;
                (*buf)[(*len)++] = ' ';
                (*buf)[*len] = 0;
            }
        }
    }
}

// Helper: extract value from XML element
static char* extract_xml_element(const char* xml, const char* element_name){
    if(!xml || !element_name) return NULL;

    char open_tag[256];
    char close_tag[256];
    snprintf(open_tag, sizeof(open_tag), "<%s", element_name);
    snprintf(close_tag, sizeof(close_tag), "</%s>", element_name);

    const char* start = strstr(xml, open_tag);
    if(!start) return NULL;

    // Find the end of opening tag
    start = strchr(start, '>');
    if(!start) return NULL;
    start++; // Move past '>'

    const char* end = strstr(start, close_tag);
    if(!end) return NULL;

    size_t len = end - start;
    if(len == 0) return NULL;

    char* result = (char*)malloc(len + 1);
    if(!result) return NULL;

    memcpy(result, start, len);
    result[len] = 0;
    return result;
}

// Extract text from .docx (Word document)
static wchar_t* extract_docx(zip_t* z, wchar_t** author_out, wchar_t** title_out){
    // Extract metadata from docProps/core.xml
    if(author_out || title_out){
        zip_stat_t st;
        if(zip_stat(z, "docProps/core.xml", 0, &st) == 0 && st.size > 0 && st.size < 1024*1024){
            char* meta_buf = (char*)malloc(st.size + 1);
            if(meta_buf){
                zip_file_t* f = zip_fopen(z, "docProps/core.xml", 0);
                if(f){
                    zip_fread(f, meta_buf, st.size);
                    zip_fclose(f);
                    meta_buf[st.size] = 0;

                    if(author_out){
                        char* author = extract_xml_element(meta_buf, "dc:creator");
                        if(author){
                            *author_out = utf8_to_wide(author);
                            free(author);
                        }
                    }

                    if(title_out){
                        char* title = extract_xml_element(meta_buf, "dc:title");
                        if(title){
                            *title_out = utf8_to_wide(title);
                            free(title);
                        }
                    }
                }
                free(meta_buf);
            }
        }
    }

    // Extract main document text from word/document.xml
    zip_stat_t st;
    if(zip_stat(z, "word/document.xml", 0, &st) != 0){
        return NULL;
    }

    if(st.size == 0 || st.size > MAX_INDEXED_CONTENT){
        return NULL;
    }

    char* xml_buf = (char*)malloc(st.size + 1);
    if(!xml_buf) return NULL;

    zip_file_t* f = zip_fopen(z, "word/document.xml", 0);
    if(!f){
        free(xml_buf);
        return NULL;
    }

    zip_fread(f, xml_buf, st.size);
    zip_fclose(f);
    xml_buf[st.size] = 0;

    // Extract text from XML
    size_t cap = 4096;
    size_t len = 0;
    char* text_buf = (char*)malloc(cap);
    if(!text_buf){
        free(xml_buf);
        return NULL;
    }
    text_buf[0] = 0;

    extract_text_from_xml(xml_buf, &text_buf, &len, &cap);
    free(xml_buf);

    wchar_t* result = utf8_to_wide(text_buf);
    free(text_buf);
    return result;
}

// Extract text from .xlsx (Excel spreadsheet)
static wchar_t* extract_xlsx(zip_t* z, wchar_t** author_out, wchar_t** title_out){
    // Extract metadata from docProps/core.xml
    if(author_out || title_out){
        zip_stat_t st;
        if(zip_stat(z, "docProps/core.xml", 0, &st) == 0 && st.size > 0 && st.size < 1024*1024){
            char* meta_buf = (char*)malloc(st.size + 1);
            if(meta_buf){
                zip_file_t* f = zip_fopen(z, "docProps/core.xml", 0);
                if(f){
                    zip_fread(f, meta_buf, st.size);
                    zip_fclose(f);
                    meta_buf[st.size] = 0;

                    if(author_out){
                        char* author = extract_xml_element(meta_buf, "dc:creator");
                        if(author){
                            *author_out = utf8_to_wide(author);
                            free(author);
                        }
                    }

                    if(title_out){
                        char* title = extract_xml_element(meta_buf, "dc:title");
                        if(title){
                            *title_out = utf8_to_wide(title);
                            free(title);
                        }
                    }
                }
                free(meta_buf);
            }
        }
    }

    size_t cap = 4096;
    size_t len = 0;
    char* text_buf = (char*)malloc(cap);
    if(!text_buf) return NULL;
    text_buf[0] = 0;

    // Extract shared strings from xl/sharedStrings.xml
    zip_stat_t st;
    if(zip_stat(z, "xl/sharedStrings.xml", 0, &st) == 0 && st.size > 0 && st.size < MAX_INDEXED_CONTENT){
        char* xml_buf = (char*)malloc(st.size + 1);
        if(xml_buf){
            zip_file_t* f = zip_fopen(z, "xl/sharedStrings.xml", 0);
            if(f){
                zip_fread(f, xml_buf, st.size);
                zip_fclose(f);
                xml_buf[st.size] = 0;
                extract_text_from_xml(xml_buf, &text_buf, &len, &cap);
            }
            free(xml_buf);
        }
    }

    // Extract text from all worksheets (xl/worksheets/sheet*.xml)
    zip_int64_t num_entries = zip_get_num_entries(z, 0);
    for(zip_int64_t i = 0; i < num_entries && len < MAX_INDEXED_CONTENT; i++){
        const char* name = zip_get_name(z, i, 0);
        if(!name) continue;

        // Check if this is a worksheet file
        if(strncmp(name, "xl/worksheets/sheet", 19) == 0 && strstr(name, ".xml")){
            if(zip_stat_index(z, i, 0, &st) == 0 && st.size > 0 && st.size < MAX_INDEXED_CONTENT){
                if(len + st.size >= MAX_INDEXED_CONTENT) break;

                char* xml_buf = (char*)malloc(st.size + 1);
                if(xml_buf){
                    zip_file_t* f = zip_fopen_index(z, i, 0);
                    if(f){
                        zip_fread(f, xml_buf, st.size);
                        zip_fclose(f);
                        xml_buf[st.size] = 0;
                        extract_text_from_xml(xml_buf, &text_buf, &len, &cap);
                    }
                    free(xml_buf);
                }
            }
        }
    }

    wchar_t* result = utf8_to_wide(text_buf);
    free(text_buf);
    return result;
}

// Extract text from .pptx (PowerPoint presentation)
static wchar_t* extract_pptx(zip_t* z, wchar_t** author_out, wchar_t** title_out){
    // Extract metadata from docProps/core.xml
    if(author_out || title_out){
        zip_stat_t st;
        if(zip_stat(z, "docProps/core.xml", 0, &st) == 0 && st.size > 0 && st.size < 1024*1024){
            char* meta_buf = (char*)malloc(st.size + 1);
            if(meta_buf){
                zip_file_t* f = zip_fopen(z, "docProps/core.xml", 0);
                if(f){
                    zip_fread(f, meta_buf, st.size);
                    zip_fclose(f);
                    meta_buf[st.size] = 0;

                    if(author_out){
                        char* author = extract_xml_element(meta_buf, "dc:creator");
                        if(author){
                            *author_out = utf8_to_wide(author);
                            free(author);
                        }
                    }

                    if(title_out){
                        char* title = extract_xml_element(meta_buf, "dc:title");
                        if(title){
                            *title_out = utf8_to_wide(title);
                            free(title);
                        }
                    }
                }
                free(meta_buf);
            }
        }
    }

    size_t cap = 4096;
    size_t len = 0;
    char* text_buf = (char*)malloc(cap);
    if(!text_buf) return NULL;
    text_buf[0] = 0;

    // Extract text from all slides (ppt/slides/slide*.xml)
    zip_int64_t num_entries = zip_get_num_entries(z, 0);
    for(zip_int64_t i = 0; i < num_entries && len < MAX_INDEXED_CONTENT; i++){
        const char* name = zip_get_name(z, i, 0);
        if(!name) continue;

        // Check if this is a slide file
        if(strncmp(name, "ppt/slides/slide", 16) == 0 && strstr(name, ".xml")){
            zip_stat_t st;
            if(zip_stat_index(z, i, 0, &st) == 0 && st.size > 0 && st.size < MAX_INDEXED_CONTENT){
                if(len + st.size >= MAX_INDEXED_CONTENT) break;

                char* xml_buf = (char*)malloc(st.size + 1);
                if(xml_buf){
                    zip_file_t* f = zip_fopen_index(z, i, 0);
                    if(f){
                        zip_fread(f, xml_buf, st.size);
                        zip_fclose(f);
                        xml_buf[st.size] = 0;
                        extract_text_from_xml(xml_buf, &text_buf, &len, &cap);
                    }
                    free(xml_buf);
                }
            }
        }
    }

    // Also extract notes from ppt/notesSlides/notesSlide*.xml
    for(zip_int64_t i = 0; i < num_entries && len < MAX_INDEXED_CONTENT; i++){
        const char* name = zip_get_name(z, i, 0);
        if(!name) continue;

        if(strncmp(name, "ppt/notesSlides/notesSlide", 26) == 0 && strstr(name, ".xml")){
            zip_stat_t st;
            if(zip_stat_index(z, i, 0, &st) == 0 && st.size > 0 && st.size < MAX_INDEXED_CONTENT){
                if(len + st.size >= MAX_INDEXED_CONTENT) break;

                char* xml_buf = (char*)malloc(st.size + 1);
                if(xml_buf){
                    zip_file_t* f = zip_fopen_index(z, i, 0);
                    if(f){
                        zip_fread(f, xml_buf, st.size);
                        zip_fclose(f);
                        xml_buf[st.size] = 0;
                        extract_text_from_xml(xml_buf, &text_buf, &len, &cap);
                    }
                    free(xml_buf);
                }
            }
        }
    }

    wchar_t* result = utf8_to_wide(text_buf);
    free(text_buf);
    return result;
}

// Main entry point: extract content from Office files
wchar_t* extract_office_content(const wchar_t* path, wchar_t** author_out, wchar_t** title_out){
    if(!path) return NULL;

    if(author_out) *author_out = NULL;
    if(title_out) *title_out = NULL;

    // Convert path to UTF-8
    char u8[MAX_LONG_PATH * 3];
    to_utf8(path, u8, sizeof(u8));

    // Open ZIP archive
    int err = 0;
    zip_t* z = zip_open(u8, ZIP_RDONLY, &err);
    if(!z) return NULL;

    // Detect file type by checking for signature files
    zip_stat_t st;
    wchar_t* result = NULL;

    if(zip_stat(z, "word/document.xml", 0, &st) == 0){
        // .docx file
        result = extract_docx(z, author_out, title_out);
    } else if(zip_stat(z, "xl/workbook.xml", 0, &st) == 0){
        // .xlsx file
        result = extract_xlsx(z, author_out, title_out);
    } else if(zip_stat(z, "ppt/presentation.xml", 0, &st) == 0){
        // .pptx file
        result = extract_pptx(z, author_out, title_out);
    }

    zip_close(z);
    return result;
}
