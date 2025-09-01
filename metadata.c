#include "metadata.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

static uint16_t rd16(const uint8_t* p, int be){
    return be ? (uint16_t)(p[0]<<8 | p[1]) : (uint16_t)(p[1]<<8 | p[0]);
}
static uint32_t rd32(const uint8_t* p, int be){
    return be ? (uint32_t)(p[0]<<24 | p[1]<<16 | p[2]<<8 | p[3])
               : (uint32_t)(p[3]<<24 | p[2]<<16 | p[1]<<8 | p[0]);
}

static void parse_ifd(const uint8_t* base, size_t len, int be, uint32_t off, Db* db, DbRecord* r){
    if(off>=len) return;
    const uint8_t* p = base + off;
    if(p+2>base+len) return;
    uint16_t count = rd16(p,be); p+=2;
    for(uint16_t i=0;i<count;i++){
        if(p+12>base+len) return;
        uint16_t tag = rd16(p,be);
        uint16_t type = rd16(p+2,be);
        uint32_t num = rd32(p+4,be);
        uint32_t valoff = rd32(p+8,be);
        const uint8_t* val;
        if(type==2){
            if(num<=4) val = p+8;
            else if(valoff + num <= len) val = base + valoff;
            else { p+=12; continue; }
            size_t slen = num < 255 ? num : 255;
            char tmp[256]; memcpy(tmp,val,slen); tmp[slen]=0;
            wchar_t wbuf[256]; to_wide(tmp,wbuf,256);
            if(tag==0x0110) r->camera_str_id = db_intern_wstring(db,wbuf);
            else if(tag==0xA434) r->lens_str_id = db_intern_wstring(db,wbuf);
        } else if(tag==0x8769){
            parse_ifd(base,len,be,valoff,db,r);
        }
        p+=12;
    }
}

void extract_exif_metadata(Db* db, const wchar_t* path, DbRecord* r){
    char u8[MAX_LONG_PATH];
    to_utf8(path,u8,sizeof(u8));
    FILE* f = fopen(u8,"rb");
    if(!f) return;
    uint8_t buf[64*1024];
    size_t n = fread(buf,1,sizeof(buf),f);
    fclose(f);
    const uint8_t* exif=NULL;
    size_t len=n;
    for(size_t i=2;i+4<n;i++){
        if(buf[i]==0xFF){
            uint8_t marker=buf[i+1];
            if(marker==0xE1){
                uint16_t seglen=(buf[i+2]<<8)|buf[i+3];
                if(i+4+6<n && memcmp(buf+i+4,"Exif\0\0",6)==0){
                    exif=buf+i+10; len=seglen-8; break;
                }
                i+=1+seglen;
            } else {
                uint16_t seglen=(buf[i+2]<<8)|buf[i+3];
                i+=1+seglen;
            }
        }
    }
    if(!exif) return;
    int be;
    if(exif[0]=='M' && exif[1]=='M') be=1;
    else if(exif[0]=='I' && exif[1]=='I') be=0;
    else return;
    uint32_t ifd0 = rd32(exif+4,be);
    parse_ifd(exif,len,be,ifd0,db,r);
}

void extract_id3_metadata(Db* db, const wchar_t* path, DbRecord* r){
    char u8[MAX_LONG_PATH];
    to_utf8(path,u8,sizeof(u8));
    FILE* f = fopen(u8,"rb");
    if(!f) return;
    uint8_t hdr[10];
    if(fread(hdr,1,10,f)==10 && memcmp(hdr,"ID3",3)==0){
        uint32_t size = ((hdr[6]&0x7F)<<21)|((hdr[7]&0x7F)<<14)|((hdr[8]&0x7F)<<7)|(hdr[9]&0x7F);
        uint8_t* buf=(uint8_t*)malloc(size);
        if(buf){
            size_t got=fread(buf,1,size,f);
            size_t pos=0;
            while(pos+10<=got){
                char id[5]; memcpy(id,buf+pos,4); id[4]=0;
                uint32_t fsize=(buf[pos+4]<<24)|(buf[pos+5]<<16)|(buf[pos+6]<<8)|buf[pos+7];
                if(fsize==0||pos+10+fsize>got) break;
                uint8_t enc=buf[pos+10];
                const uint8_t* data=buf+pos+11; size_t dlen=fsize-1;
                char tmp[256]={0};
                if(enc==0||enc==3){
                    size_t len=dlen<255?dlen:255; memcpy(tmp,data,len); tmp[len]=0;
                } else if(enc==1 && dlen>=2){
                    const uint8_t* p=data;
                    if(p[0]==0xFF && p[1]==0xFE){
                        p+=2; dlen-=2;
                        wchar_t wtmp[256]; size_t wlen=0;
                        while(dlen>=2 && wlen<255){
                            wchar_t ch=p[0]|(p[1]<<8); if(ch==0) break;
                            wtmp[wlen++]=ch; p+=2; dlen-=2;
                        }
                        wtmp[wlen]=0; to_utf8(wtmp,tmp,sizeof(tmp));
                    } else if(p[0]==0xFE && p[1]==0xFF){
                        p+=2; dlen-=2;
                        wchar_t wtmp[256]; size_t wlen=0;
                        while(dlen>=2 && wlen<255){
                            wchar_t ch=(p[0]<<8)|p[1]; if(ch==0) break;
                            wtmp[wlen++]=ch; p+=2; dlen-=2;
                        }
                        wtmp[wlen]=0; to_utf8(wtmp,tmp,sizeof(tmp));
                    }
                }
                if(tmp[0]){
                    wchar_t wbuf[256]; to_wide(tmp,wbuf,256);
                    if(strcmp(id,"TPE1")==0) r->artist_str_id=db_intern_wstring(db,wbuf);
                    else if(strcmp(id,"TALB")==0) r->album_str_id=db_intern_wstring(db,wbuf);
                }
                pos+=10+fsize;
            }
            free(buf);
        }
        fclose(f);
        return;
    }
    // ID3v1
    fseek(f,-128,SEEK_END);
    uint8_t v1[128];
    if(fread(v1,1,128,f)==128 && memcmp(v1,"TAG",3)==0){
        char tmp[31]; memcpy(tmp,v1+33,30); tmp[30]=0;
        if(tmp[0]){ wchar_t wbuf[256]; to_wide(tmp,wbuf,256); r->artist_str_id=db_intern_wstring(db,wbuf); }
        char tmp2[31]; memcpy(tmp2,v1+63,30); tmp2[30]=0;
        if(tmp2[0]){ wchar_t wbuf[256]; to_wide(tmp2,wbuf,256); r->album_str_id=db_intern_wstring(db,wbuf); }
    }
    fclose(f);
}

