#ifndef ARCHIVE_H
#define ARCHIVE_H
#include "anything.h"
#include "database.h"
#ifdef __cplusplus
extern "C" {
#endif

BOOL index_archive(Db* db, const wchar_t* archive_path);

#ifdef __cplusplus
}
#endif
#endif
