/* Pure bounded immutable commandDB inventory; no MMIO/write interface. */
#ifndef SLPI_CMDDB_RO_H
#define SLPI_CMDDB_RO_H
struct slpi_cmddb_record { uint32_t address;uint16_t slave,aux_bytes,levels[16];unsigned count; };
static int slpi_cmddb_record(const uint8_t *db,uint32_t len,const char *name,struct slpi_cmddb_record *out){
 *out=(struct slpi_cmddb_record){0};
 if(len<144||db[4]!=0xdb||db[5]!=0x30||db[6]!=3||db[7]!=12)return 0;
 unsigned matches=0;
 for(unsigned s=0;s<8;s++){
  const uint8_t*h=db+8+s*16;unsigned id=slpi_u16(h),ho=slpi_u16(h+2),off=slpi_u16(h+4),n=slpi_u16(h+6);
  if(!id)continue;
  if(n>512||ho>len-144||n>(len-144-ho)/24)return 0;
  for(unsigned i=0;i<n;i++){
   const uint8_t*e=db+144+ho+i*24;unsigned k=0;
   for(;k<8&&name[k]&&e[k]==(uint8_t)name[k];k++);
   if(k==8||name[k]||e[k])continue;
   for(unsigned j=k;j<8;j++)if(e[j])return 0;
   if(++matches!=1)return 0;
   unsigned bytes=slpi_u16(e+20),at=slpi_u16(e+22);
   if(bytes>32||(bytes&1)||off>len-144||at>len-144-off||bytes>len-144-off-at)return 0;
   out->address=slpi_u32(e+16);out->slave=id;out->aux_bytes=bytes;out->count=bytes/2;
   for(unsigned j=0;j<out->count;j++)out->levels[j]=slpi_u16(db+144+off+at+2*j);
  }
 }
 return matches==1;
}
#endif
