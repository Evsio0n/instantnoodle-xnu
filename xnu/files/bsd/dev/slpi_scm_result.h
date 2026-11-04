#ifndef SLPI_SCM_RESULT_H
#define SLPI_SCM_RESULT_H
/* Qualcomm primary SCM constants: INTERRUPTED1, WAITQ_SLEEP2.
 * This adapter does not implement WAITQ context management/resumption. */
#define SLPI_SCM_UNCERTAIN (-2147483647-1)
enum slpi_scm_action {SLPI_SCM_TERMINAL,SLPI_SCM_RESUME,SLPI_SCM_RETAIN};
static enum slpi_scm_action slpi_scm_classify(int32_t raw,unsigned attempt){
 if(raw==1)return attempt<100?SLPI_SCM_RESUME:SLPI_SCM_RETAIN;
 if(raw>0)return SLPI_SCM_RETAIN; /* WAITQ_SLEEP2 and unknown positive control returns */
 return SLPI_SCM_TERMINAL;
}
#endif
