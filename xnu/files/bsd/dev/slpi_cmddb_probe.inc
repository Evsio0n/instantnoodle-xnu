/* Included after the existing read-only SMEM probe definitions. */
#include "slpi_cmddb_ro.h"
static void instantnoodle_slpi_cmddb_probe_init(void){
 static int attempted;uint32_t enabled=0;char line[256];
 PE_parse_boot_argn("slpi_cmddb_ro",&enabled,sizeof(enabled));if(enabled!=1||attempted++)return;
 if(!instantnoodle_board()||pe_arm_get_soc_base_phys()!=0x00400000){slpi_ro_log("slpi-cmdb: board mismatch; no mapping\n");return;}
 slpi_ro_log("slpi-cmdb: READ ONLY 80860000+20000; no RSC/AOP/PAS/IPCC access\n");
 vm_offset_t map=ml_io_map_unmappable(0x80860000,0x20000,VM_WIMG_WCOMB);
 if(!map){slpi_ro_log("slpi-cmdb: map failed\n");return;}
 static const char *names[]={"lcx.lvl","lmx.lvl","xo.lvl"};
 for(unsigned i=0;i<3;i++){
  struct slpi_cmddb_record r;
  if(!slpi_cmddb_record((const uint8_t *)map,0x20000,names[i],&r)){snprintf(line,sizeof(line),"slpi-cmdb: %s missing/ambiguous/invalid bounded record\n",names[i]);slpi_ro_log(line);continue;}
  unsigned maximum=0;for(unsigned j=0;j<r.count;j++)if(r.levels[j]>maximum)maximum=r.levels[j];
  snprintf(line,sizeof(line),"slpi-cmdb: %s slave=%u address=%x aux_bytes=%u raw_u16_count=%u raw_max=%u\n",names[i],r.slave,r.address,r.aux_bytes,r.count,maximum);slpi_ro_log(line);
  for(unsigned j=0;j<r.count;j++){snprintf(line,sizeof(line),"slpi-cmdb: %s aux[%u]=%u\n",names[i],j,r.levels[j]);slpi_ro_log(line);}
 }
 ml_io_unmap(map,0x20000);slpi_ro_log("slpi-cmdb: mapping released; no votes changed (XO raw aux not assumed rail levels)\n");
}
