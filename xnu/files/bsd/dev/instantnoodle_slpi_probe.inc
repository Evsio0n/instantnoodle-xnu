/* Default-off stage1 only: read SMEM metadata; never starts/stops/acks a DSP. */
#include <vm/memory_types.h>
#include <sys/instantnoodle.h>
#include "slpi_oem130_ro.h"
extern vm_offset_t ml_io_map_unmappable(vm_offset_t,vm_size_t,unsigned int);
extern void ml_io_unmap(vm_offset_t,vm_size_t);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern void loader_log_putc(char);
static void slpi_ro_log(const char *line){printf("%s",line);for(const char *p=line;*p;p++)loader_log_putc(*p);}
static int slpi_ro_read(void *ctx,uint32_t off,void *dst,uint32_t n){
 const volatile uint8_t *src=(const volatile uint8_t *)ctx;uint8_t *out=dst;
 if(off>SLPI_SMEM_BYTES||n>SLPI_SMEM_BYTES-off)return 0;
 for(uint32_t i=0;i<n;i++)out[i]=src[off+i];return 1;
}
static void instantnoodle_slpi_probe_init(void){
 static int attempted;uint32_t step=0,allow=0,adsp=0,adsp_step=0;char line[192];
 PE_parse_boot_argn("slpi_step",&step,sizeof(step));if(!step)return;
 PE_parse_boot_argn("slpi_smem_ro",&allow,sizeof(allow));
 if(attempted++)return;
 if(step!=1 || allow!=1 || !instantnoodle_board() || pe_arm_get_soc_base_phys()!=0x00400000){slpi_ro_log("slpi-ro: refused; stage1 + slpi_smem_ro=1 + instantnoodle required\n");return;}
 PE_parse_boot_argn("adsp",&adsp,sizeof(adsp));PE_parse_boot_argn("adsp_step",&adsp_step,sizeof(adsp_step));
 if(adsp || adsp_step){slpi_ro_log("slpi-ro: refused concurrent legacy ADSP path (wrong live reservation)\n");return;}
 slpi_ro_log("slpi-ro: stage1 READ ONLY SMEM80900000+200000; PID3 PAS12; no DSP/mailbox writes\n");
 vm_offset_t mapped=ml_io_map_unmappable(0x80900000,SLPI_SMEM_BYTES,VM_WIMG_WCOMB);
 if(!mapped){slpi_ro_log("slpi-ro: map failed\n");return;}
 struct slpi_report report;const char *why=slpi_observe(slpi_ro_read,(void *)mapped,&report);
 if(why){snprintf(line,sizeof(line),"slpi-ro: observation rejected: %s\n",why);slpi_ro_log(line);}
 else{
  struct so130_observation oem={0};
  const char *status=so130_observe(slpi_ro_read,(void *)mapped,&report,&oem);
  snprintf(line,sizeof(line),"slpi-oem130: status=%s global_partitions=%u hits=%u bytes=%u\n",
           status,report.global_partitions,report.global130_count,report.global130.bytes);
  slpi_ro_log(line);
  if(oem.checked){
   for(unsigned i=0;i<SO130_TYPES;i++){
    snprintf(line,sizeof(line),"slpi-oem130: type=%u id=%u slots(name,bus,dir,irq)=%02x%02x%02x%02x,%02x%02x%02x%02x,%02x%02x%02x%02x\n",
     so130_types[i],oem.type[i].id,oem.type[i].slot[0][0],oem.type[i].slot[0][1],oem.type[i].slot[0][2],oem.type[i].slot[0][3],
     oem.type[i].slot[1][0],oem.type[i].slot[1][1],oem.type[i].slot[1][2],oem.type[i].slot[1][3],
     oem.type[i].slot[2][0],oem.type[i].slot[2][1],oem.type[i].slot[2][2],oem.type[i].slot[2][3]);
    slpi_ro_log(line);
   }
  }
  snprintf(line,sizeof(line),"slpi-ro: PID0/3 partition offset=%x size=%x selected-items=%u\n",report.remote_partition,report.remote_bytes,report.found);slpi_ro_log(line);
  for(unsigned i=0;i<6;i++)if(report.items[i].bytes){
   struct slpi_item *it=&report.items[i];snprintf(line,sizeof(line),"slpi-ro: item%u hosts%u/%u offset=%x bytes=%u\n",it->id,it->host0,it->host1,it->offset,it->bytes);slpi_ro_log(line);
   /* Counters only. Do not print FIFO payloads or DSP crash-text contents. */
   if(it->id==478 && it->bytes>=16){uint8_t d[16];if(slpi_read(slpi_ro_read,(void *)mapped,it->offset,d,16)){snprintf(line,sizeof(line),"slpi-ro: GLINK indices %u %u %u %u\n",slpi_u32(d),slpi_u32(d+4),slpi_u32(d+8),slpi_u32(d+12));slpi_ro_log(line);}}
   if((it->id==430||it->id==481) && it->bytes>=20){uint8_t d[20];if(slpi_read(slpi_ro_read,(void *)mapped,it->offset,d,20)){snprintf(line,sizeof(line),"slpi-ro: SMP2P magic=%x ver/features=%x hosts=%u/%u flags=%x entries=%u/%u\n",slpi_u32(d),slpi_u32(d+4),slpi_u16(d+8),slpi_u16(d+10),slpi_u32(d+16),slpi_u16(d+14),slpi_u16(d+12));slpi_ro_log(line);}}
  }
 }
 ml_io_unmap(mapped,SLPI_SMEM_BYTES);slpi_ro_log("slpi-ro: stage1 ended; mapping released; no DSP ownership acquired\n");
}
