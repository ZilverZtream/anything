#pragma once

#ifndef METADATA_H
#define METADATA_H
#include "anything/anything.h"
#include "anything/database.h"
void extract_exif_metadata(Db* db, const wchar_t* path, DbRecord* r);
void extract_id3_metadata(Db* db, const wchar_t* path, DbRecord* r);
#endif
