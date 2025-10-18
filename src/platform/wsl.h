#ifndef WSL_H
#define WSL_H
#include "anything/anything.h"
#include "anything/database.h"
#ifdef __cplusplus
extern "C" {
#endif

BOOL WSLScanner_Start(const wchar_t* root_path, Db* db);

#ifdef __cplusplus
}
#endif
#endif
