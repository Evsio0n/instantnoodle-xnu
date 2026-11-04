/* Read-only commandDB planner. Refuse unless384 is the highest supported
 * LCX/LMX level: then its index cannot lower another legal AP rail vote.
 * XO on3 is the native maximum enable vote. Never issue a release/zero vote. */
#ifndef SLPI_PROXY_PLAN_H
#define SLPI_PROXY_PLAN_H
struct slpi_proxy_command { uint32_t address,value; };
static int slpi_proxy_resource(const uint8_t *db,uint32_t len,const char *name,int rail,struct slpi_proxy_command *cmd){
 if(len<144 || db[4]!=0xdb||db[5]!=0x30||db[6]!=3||db[7]!=12)return 0;
 unsigned matches=0;
 for(unsigned s=0;s<8;s++){
  const uint8_t *h=db+8+s*16;uint32_t ho=slpi_u16(h+2),d=slpi_u16(h+4),n=slpi_u16(h+6);
  if(!slpi_u16(h))continue;
  if(n>512||ho>len-144||n>(len-144-ho)/24)return 0;
  for(unsigned i=0;i<n;i++){
   const uint8_t *e=db+144+ho+i*24;unsigned k=0;
   for(;k<8 && name[k] && e[k]==(uint8_t)name[k];k++);
   if(k==8 || name[k] || e[k])continue;
   for(unsigned j=k;j<8;j++)if(e[j])return 0;
   if(++matches>1)return 0;
   uint32_t a=slpi_u32(e+16),bytes=slpi_u16(e+20),off=slpi_u16(e+22);
   if(!a || (a&3))return 0;cmd->address=a;
   if(!rail){cmd->value=3;continue;}
   if(bytes<2||bytes>32||(bytes&1)||d>len-144||off>len-144-d||bytes>len-144-d-off)return 0;
   unsigned last=0,ended=0,maximum=0;
   for(unsigned j=0;j<bytes/2;j++){
    unsigned v=slpi_u16(db+144+d+off+j*2);
    if(j==0){if(v!=0)return 0;continue;}
    if(!v){ended=1;continue;}
    if(ended||v<=maximum)return 0;maximum=v;last=j;
   }
   if(maximum!=384)return 0;cmd->value=last;
  }
 }
 return matches==1;
}
static int slpi_proxy_plan(const uint8_t *db,uint32_t n,struct slpi_proxy_command c[3]){
 return slpi_proxy_resource(db,n,"lcx.lvl",1,c)&&slpi_proxy_resource(db,n,"lmx.lvl",1,c+1)&&slpi_proxy_resource(db,n,"xo.lvl",0,c+2)&&c[0].address!=c[1].address&&c[0].address!=c[2].address&&c[1].address!=c[2].address;
}
#endif
