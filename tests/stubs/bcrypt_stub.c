#include "bcrypt.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
    const unsigned char* data;
    unsigned long len;
} SHA1_STUB_CTX;

static uint32_t rol32(uint32_t x, uint32_t n){ return (x<<n) | (x>>(32-n)); }

static void simple_sha1(const unsigned char* msg, size_t len, unsigned char out[20]){
    uint32_t h0=0x67452301, h1=0xEFCDAB89, h2=0x98BADCFE, h3=0x10325476, h4=0xC3D2E1F0;
    size_t ml = len * 8;
    size_t padded = ((len + 8) / 64 + 1) * 64;
    unsigned char* buf = (unsigned char*)malloc(padded);
    if(!buf){ memset(out,0,20); return; }
    memcpy(buf, msg, len);
    buf[len]=0x80;
    memset(buf+len+1,0,padded-len-1-8);
    for(int i=0;i<8;i++) buf[padded-8+i]=(unsigned char)(ml>>(56-8*i));
    for(size_t off=0; off<padded; off+=64){
        uint32_t w[80];
        for(int i=0;i<16;i++){
            w[i]=((uint32_t)buf[off+4*i]<<24)|((uint32_t)buf[off+4*i+1]<<16)|((uint32_t)buf[off+4*i+2]<<8)|((uint32_t)buf[off+4*i+3]);
        }
        for(int i=16;i<80;i++) w[i]=rol32(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
        uint32_t a=h0,b=h1,c=h2,d=h3,e=h4;
        for(int i=0;i<80;i++){
            uint32_t f,k;
            if(i<20){ f=(b&c)|((~b)&d); k=0x5A827999; }
            else if(i<40){ f=b^c^d; k=0x6ED9EBA1; }
            else if(i<60){ f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
            else { f=b^c^d; k=0xCA62C1D6; }
            uint32_t temp=rol32(a,5)+f+e+k+w[i];
            e=d; d=c; c=rol32(b,30); b=a; a=temp;
        }
        h0+=a; h1+=b; h2+=c; h3+=d; h4+=e;
    }
    free(buf);
    uint32_t hs[5]={h0,h1,h2,h3,h4};
    for(int i=0;i<5;i++){
        out[4*i]= (unsigned char)(hs[i]>>24);
        out[4*i+1]=(unsigned char)(hs[i]>>16);
        out[4*i+2]=(unsigned char)(hs[i]>>8);
        out[4*i+3]=(unsigned char)(hs[i]);
    }
}

int BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE* phAlg, const wchar_t* algId, const wchar_t* implementation, unsigned long flags){ *phAlg=(BCRYPT_ALG_HANDLE)1; return 0; }
int BCryptGetProperty(BCRYPT_ALG_HANDLE hAlg, const wchar_t* propId, unsigned char* pbOutput, unsigned long cbOutput, unsigned long* pcbResult, unsigned long flags){ unsigned long size=sizeof(SHA1_STUB_CTX); if(pbOutput && cbOutput>=sizeof(unsigned long)) memcpy(pbOutput,&size,sizeof(unsigned long)); if(pcbResult) *pcbResult=sizeof(unsigned long); return 0; }
int BCryptCreateHash(BCRYPT_ALG_HANDLE hAlg, BCRYPT_HASH_HANDLE* phHash, unsigned char* pbHashObject, unsigned long cbHashObject, unsigned char* pbSecret, unsigned long cbSecret, unsigned long flags){ SHA1_STUB_CTX* ctx=(SHA1_STUB_CTX*)pbHashObject; ctx->data=NULL; ctx->len=0; *phHash=ctx; return 0; }
int BCryptHashData(BCRYPT_HASH_HANDLE hHash, unsigned char* pbInput, unsigned long cbInput, unsigned long flags){ SHA1_STUB_CTX* ctx=(SHA1_STUB_CTX*)hHash; ctx->data=pbInput; ctx->len=cbInput; return 0; }
int BCryptFinishHash(BCRYPT_HASH_HANDLE hHash, unsigned char* pbOutput, unsigned long cbOutput, unsigned long flags){ SHA1_STUB_CTX* ctx=(SHA1_STUB_CTX*)hHash; simple_sha1(ctx->data, ctx->len, pbOutput); return 0; }
int BCryptDestroyHash(BCRYPT_HASH_HANDLE hHash){ return 0; }
int BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE hAlg, unsigned long flags){ return 0; }
