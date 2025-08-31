#ifndef MACVM_H
#define MACVM_H
#include "anything.h"
#include "database.h"
#ifdef __cplusplus
extern "C" {
#endif

BOOL MacVMScanner_Start(const wchar_t* image_path, Db* db);

#ifdef __cplusplus
}
#endif
#endif
