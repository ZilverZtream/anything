#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Extract text content from modern Office files (.docx, .xlsx, .pptx)
// by directly parsing the XML from their ZIP container.
// Returns a wide-character string with the extracted text, or NULL on failure.
// The caller is responsible for freeing the returned string.
wchar_t* extract_office_content(const wchar_t* path, wchar_t** author_out, wchar_t** title_out);

#ifdef __cplusplus
}
#endif
