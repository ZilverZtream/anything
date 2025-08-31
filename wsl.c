// wsl.c - stub for indexing WSL file systems
#include "wsl.h"

BOOL WSLScanner_Start(const wchar_t* root_path, Db* db){
    (void)root_path; (void)db;
    // Placeholder: traverse WSL (ext4) file systems and index metadata
    // similar to native scanners.
    return FALSE;
}
