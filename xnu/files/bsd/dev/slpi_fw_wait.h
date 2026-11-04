#ifndef SLPI_FW_WAIT_H
#define SLPI_FW_WAIT_H
/* Host-testable mount/file-readiness barrier. No hardware callback exists.
 * poll:1 exactmetadatafile ready,0 notyet,-1 fatal/cancelled. */
static int slpi_wait_firmware(void *ctx,int(*poll)(void *),uint64_t(*now_ms)(void *),void(*pause_ms)(void *,unsigned)){
 uint64_t began=now_ms(ctx);
 for(;;){
  uint64_t now=now_ms(ctx);if(now<began||now-began>=60000)return 0;
  int result=poll(ctx);if(result)return result>0?1:0;
  now=now_ms(ctx);if(now<began||now-began>=60000)return 0;
  uint64_t left=60000-(now-began);pause_ms(ctx,(unsigned)(left<200?left:200));
 }
}
#endif
