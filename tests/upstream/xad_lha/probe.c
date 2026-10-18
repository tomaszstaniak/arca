#include <stdio.h>
#include <proto/xadmaster.h>
#include "xad_static.h"
struct xadMasterBase *xadMasterBase;
int main(int argc,char **argv) {
 int i,rc=0; xadMasterBase=xad_static_open(); if(!xadMasterBase)return 20;
 for(i=1;i<argc;i++) {struct xadArchiveInfo *a=xadAllocObjectA(XADOBJ_ARCHIVEINFO,NULL); struct xadFileInfo *f; LONG e;
 e=xadGetInfo(a,XAD_INFILENAME,(IPTR)argv[i],TAG_DONE); printf("ARCHIVE %s error=%ld\n",argv[i],(long)e);
 if(e)rc=20;else for(f=a->xai_FileInfo;f;f=f->xfi_Next)printf("ENTRY %s type=%s prot=%08lx comment=%s\n",f->xfi_FileName,(f->xfi_Flags&XADFIF_DIRECTORY)?"dir":"file",(unsigned long)f->xfi_Protection,f->xfi_Comment?f->xfi_Comment:"");
 xadFreeInfo(a); xadFreeObjectA(a,NULL);
 } xad_static_close(); return rc;
}
