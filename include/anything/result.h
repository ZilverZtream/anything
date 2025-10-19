#pragma once

#ifndef RESULT_H
#define RESULT_H
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RESULT_SUCCESS = 0,
    RESULT_ERROR_MEMORY,
    RESULT_ERROR_IO,
    RESULT_ERROR_INVALID_PARAM,
    RESULT_ERROR_TIMEOUT
} ResultCode;

typedef struct {
    ResultCode code;
    char message[256];
    void* data;
} Result;

static inline Result result_success(void* data){
    Result r; r.code = RESULT_SUCCESS; r.message[0] = '\0'; r.data = data; return r;
}

static inline Result result_error(ResultCode code, const char* msg){
    Result r; r.code = code; r.data = NULL;
    if(msg){
#if defined(_MSC_VER)
        strncpy_s(r.message, sizeof(r.message), msg, _TRUNCATE);
#else
        strncpy(r.message, msg, sizeof(r.message)-1);
        r.message[sizeof(r.message)-1] = '\0';
#endif
    } else {
        r.message[0] = '\0';
    }
    return r;
}

#ifdef __cplusplus
}
#endif
#endif
