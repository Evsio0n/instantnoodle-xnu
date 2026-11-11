/* Run through the regular QMI transaction and private projection. */
#define main original_radio_tests
#include "test_radio.c"
#undef main
static uint8_t card_field[3500];
static size_t field_bytes;
static uint8_t validity[8];
static size_t validity_bytes;
static bool validity_first, validity_duplicate, omit_card;

static int send_card(struct hk_qrtr_port *port,uint32_t node,uint32_t to,const void *raw,size_t length)
{
 const uint8_t *request=raw;uint8_t reply[3600],result[4]={0};size_t size=QMI_HEADER_SIZE;
 check(node==SERVER_NODE && to==SERVER_PORT && length==QMI_HEADER_SIZE &&
       request[0]==QMI_REQUEST && request[3]==0x2f && request[4]==0,"actual UIM Get Card Status request");
 memcpy(reply,request,QMI_HEADER_SIZE);reply[0]=QMI_RESPONSE;
 append_tlv(reply,&size,2,result,4);
 if (validity_bytes && validity_first) append_tlv(reply,&size,0x12,validity,validity_bytes);
 if (!omit_card) append_tlv(reply,&size,0x10,card_field,(uint16_t)field_bytes);
 if (validity_bytes && (!validity_first || validity_duplicate)) append_tlv(reply,&size,0x12,validity,validity_bytes);
 reply[5]=(uint8_t)(size-7);reply[6]=(uint8_t)((size-7)>>8);
 port->receive(port->context,SERVER_NODE,SERVER_PORT,reply,size);return 0;
}
static void header(uint16_t primary,uint16_t secondary,unsigned count)
{
 memset(card_field,0xff,8);card_field[0]=primary;card_field[1]=primary>>8;
 card_field[4]=secondary;card_field[5]=secondary>>8;card_field[8]=count;field_bytes=9;
}
static void slot(unsigned state,unsigned error,unsigned apps,uint8_t opaque)
{
 uint8_t *v=card_field+field_bytes;memset(v,opaque,6);v[0]=state;v[4]=error;v[5]=apps;field_bytes+=6;
}
static void application(unsigned t,unsigned state,unsigned len,uint8_t opaque)
{
 uint8_t *v=card_field+field_bytes;memset(v,opaque,14+len);v[0]=t;v[1]=state;v[6]=len;field_bytes+=14+len;
}
static void paired(uint8_t opaque)
{
 header(0x0101,0x0000,2);
 slot(1,199,2,opaque);application(1,2,7,opaque);application(5,7,12,opaque);
 slot(1,211,3,opaque);application(2,7,8,opaque);application(2,7,3,opaque);application(1,3,0,opaque);
}
int main(void)
{
 struct qmi_handle qmi;struct hk_qmi_uim_status s,other;const struct hk_qrtr_ops ops={open_port,send_card,lookup_service,close_port};
 memset(&router,0,sizeof(router));checks=failures=0;
 check(qmi_handle_init_transport(&qmi,4096,&qmi_ops,NULL,&ops)==0,"fixture init");
 check(qmi_add_lookup(&qmi,HK_QMI_UIM_SERVICE,1,0)==0,"fixture UIM lookup");
 router.server(router.context,true,HK_QMI_UIM_SERVICE,1,0,SERVER_NODE,SERVER_PORT);
 paired(0xa5);check(hk_qmi_uim_get_card_status(&qmi,&s,100)>0 && s.valid,"complete paired response");
 check(s.slots==2 && s.logical_count==2 && s.card_state[1]==2 && s.application_state[7]==3,"existing card/ready histograms include ISIM");
 check(s.logical[0].card_state==1 && !s.logical[0].error_valid && !s.logical[0].error && s.logical[0].app_count==2 &&
       s.logical[0].gw_apps==1 && !s.logical[0].ready_gw_apps && s.logical[0].sim_states==4,"primary logical slot SIM locked; ISIM not counted as ready GW");
 check(s.logical[1].card_state==1 && s.logical[1].gw_apps==3 && s.logical[1].ready_gw_apps==2 &&
       s.logical[1].usim_states==128 && s.logical[1].sim_states==8,"per-type readiness and state masks in second slot");
 check(s.gw[0].status==HK_UIM_GW_MAPPED && s.gw[0].index==0x0101 && s.gw[0].app_type==2 &&
       s.gw[0].app_state==7 && s.gw[1].status==HK_UIM_GW_MAPPED && s.gw[1].index==0 &&
       s.gw[1].app_type==1 && s.gw[1].app_state==2,"primary and secondary use native high-byte slot, low-byte app");
 paired(0x4c);check(hk_qmi_uim_get_card_status(&qmi,&other,100)>0 &&
       !memcmp(s.logical,other.logical,sizeof(s.logical)) && !memcmp(s.gw,other.gw,sizeof(s.gw)),
       "changing all AID bytes, PIN states/retry counts and personalization has no observable effect");
 header(0xffff,0x0101,2);slot(2,2,0,0x66);slot(0,65,0,0x66);
 check(hk_qmi_uim_get_card_status(&qmi,&s,100)>0 && s.error_mask==4 && s.logical[0].error_valid &&
       s.logical[0].error==2 && !s.logical[1].error_valid && s.gw[0].status==HK_UIM_GW_UNASSIGNED &&
       s.gw[0].index==0xffff && s.gw[1].status==HK_UIM_GW_INVALID && s.gw[1].index==0xffff,
       "absent vs card-error vs unset/invalid reference; opaque error suppressed when card absent");
 header(0x0000,0x0200,3);slot(1,7,1,0xff);application(4,7,255,0xff);slot(2,128,1,0xff);
 application(2,242,0,0xff);slot(1,0,1,0xff);application(2,7,0,0xff);
 check(hk_qmi_uim_get_card_status(&qmi,&s,100)>0 && s.slots==3 && s.logical_count==2 &&
       s.gw[0].status==HK_UIM_GW_INVALID && s.gw[1].status==HK_UIM_GW_OUTSIDE && s.gw[1].index==0xffff &&
       s.logical[1].error_valid && s.logical[1].error==HK_UIM_ENUM_OTHER && s.logical[1].other_gw_states==1 &&
       !s.logical[1].ready_gw_apps && s.application_state[7]==2,"other family, future enum and outside projection kept separate");
 field_bytes--;check(hk_qmi_uim_get_card_status(&qmi,&s,100)==-EBADMSG && !s.valid && !s.slots && !s.logical_count,
       "malformed out-of-scope final slot cannot leak partial good records");
 header(0xffff,0xffff,0);check(hk_qmi_uim_get_card_status(&qmi,&s,100)>0 && s.valid && s.logical_count==0 &&
       s.gw[0].status==HK_UIM_GW_UNASSIGNED && s.gw[1].status==HK_UIM_GW_UNASSIGNED,"zero-slot valid bounded result");
 header(0x0000,0xffff,2);
 slot(1,0,1,0); application(2,7,0,0);
 slot(2,3,1,0); application(2,7,0,0);
 check(hk_qmi_uim_get_card_status(&qmi,&s,100)>0 && !s.validity_present && s.application_state[7]==2 &&
       s.present_ready_apps==1 && !s.confirmed_ready_apps && s.logical[0].validity==HK_UIM_VALIDITY_UNREPORTED,
       "absent validity preserves eligible observations and leaves confirmations unknown");
 validity[0]=2; validity[1]=0; validity[2]=1; validity_bytes=3;
 check(hk_qmi_uim_get_card_status(&qmi,&s,100)>0 && s.valid && s.validity_present && s.validity_count==2 &&
       s.logical[0].validity==HK_UIM_VALIDITY_INVALID && s.logical[1].validity==HK_UIM_VALIDITY_VALID &&
       s.application_state[7]==2 && s.logical[0].app_count==1 && !s.logical[0].ready_gw_apps &&
       !s.present_ready_apps && !s.confirmed_ready_apps,
       "invalid slot and errored slot have no usable ready apps, even with READY bytes");
 validity_first=true;
 check(hk_qmi_uim_get_card_status(&qmi,&other,100)>0 && !memcmp(&s,&other,sizeof(s)), "validity TLV order is immaterial");
 validity[0]=1;validity[1]=1;validity_bytes=2;
 check(hk_qmi_uim_get_card_status(&qmi,&other,100)>0 && other.present_ready_apps==1 && other.confirmed_ready_apps==1 &&
       other.logical[1].validity==HK_UIM_VALIDITY_UNREPORTED,"validity array can cover a prefix of the reported slots");
 validity[1]=99;
 check(hk_qmi_uim_get_card_status(&qmi,&other,100)>0 && other.logical[0].validity==HK_UIM_VALIDITY_OTHER &&
       !other.present_ready_apps && !other.confirmed_ready_apps,"unknown future validity is not accepted as ready");
 validity_duplicate=true;
 check(hk_qmi_uim_get_card_status(&qmi,&other,100)==-EBADMSG && !other.valid && !other.present_ready_apps,
       "duplicate validity is rejected without prior ready state");
 validity_duplicate=false;validity[0]=2;
 check(hk_qmi_uim_get_card_status(&qmi,&other,100)==-EBADMSG && !other.valid && !other.logical_count,
       "truncated validity vector is rejected transactionally");
 omit_card=true;validity_bytes=0;
 check(hk_qmi_uim_get_card_status(&qmi,&other,100)>0 && !other.valid && !other.present_ready_apps,
       "no card TLV has no reportable card/ready count");
 omit_card=false;header(0xffff,0xffff,2);slot(3,1,0,0);slot(77,1,0,0);
 check(hk_qmi_uim_get_card_status(&qmi,&other,100)>0 && other.other_cards==2 &&
       other.logical[0].card_state==HK_UIM_CARD_UNKNOWN && other.logical[1].card_state==HK_UIM_ENUM_OTHER &&
       !other.logical[0].error_valid,"documented UNKNOWN is distinguished from a future card-state value");
 qmi_handle_release(&qmi);printf("%s UIM logical fixtures (%u checks)\n",failures?"FAIL":"ok",checks);return failures!=0;
}
