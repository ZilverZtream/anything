#ifndef BCRYPT_H
#define BCRYPT_H
#include "windows_stub.h"

typedef void* BCRYPT_ALG_HANDLE;
typedef void* BCRYPT_HASH_HANDLE;

#define BCRYPT_SHA1_ALGORITHM L"SHA1"
#define BCRYPT_OBJECT_LENGTH L"ObjectLength"

int BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE* phAlg, const wchar_t* algId, const wchar_t* implementation, unsigned long flags);
int BCryptGetProperty(BCRYPT_ALG_HANDLE hAlg, const wchar_t* propId, unsigned char* pbOutput, unsigned long cbOutput, unsigned long* pcbResult, unsigned long flags);
int BCryptCreateHash(BCRYPT_ALG_HANDLE hAlg, BCRYPT_HASH_HANDLE* phHash, unsigned char* pbHashObject, unsigned long cbHashObject, unsigned char* pbSecret, unsigned long cbSecret, unsigned long flags);
int BCryptHashData(BCRYPT_HASH_HANDLE hHash, unsigned char* pbInput, unsigned long cbInput, unsigned long flags);
int BCryptFinishHash(BCRYPT_HASH_HANDLE hHash, unsigned char* pbOutput, unsigned long cbOutput, unsigned long flags);
int BCryptDestroyHash(BCRYPT_HASH_HANDLE hHash);
int BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE hAlg, unsigned long flags);

#endif
