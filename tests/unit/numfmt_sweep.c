// Opt-in exhaustive check of src/os/lua_numfmt.c against glibc: every 61st
// float32 bit pattern (~70M values) at %.7g and %.9g, the formats Lua uses
// for tostring.  About 35 s; not part of ctest.
//   make test-numfmt-sweep
#include "lua_numfmt.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
int main(void){ long n=0,f=0; char a[128],b[128];
 for (uint64_t u=0; u<=0xFFFFFFFFull; u+=61){ uint32_t x=(uint32_t)u; float v; memcpy(&v,&x,4); if(isnan(v)) continue;
  picos_numfmt_float(a,sizeof a,"%.7g",v); snprintf(b,sizeof b,"%.7g",(double)v); n++;
  if(strcmp(a,b)){ if(f++<10) printf("MISMATCH %08x [%s] [%s]\n",x,a,b);} 
  picos_numfmt_float(a,sizeof a,"%.9g",v); snprintf(b,sizeof b,"%.9g",(double)v);
  if(strcmp(a,b)){ if(f++<10) printf("MISMATCH9 %08x [%s] [%s]\n",x,a,b);} }
 printf("%ld values, %ld mismatches\n",n,f); return f!=0; }
