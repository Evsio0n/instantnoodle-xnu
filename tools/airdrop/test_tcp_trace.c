#include <assert.h>
#include <string.h>
#include "TcpTracePacket.h"
int main(void){
 unsigned char p[128]={0},peer[16]={0xfe,0x80};peer[15]=7;
 p[12]=0x86;p[13]=0xdd;p[14]=0x60;p[19]=74;p[20]=6;memcpy(p+22,peer,16);
 p[54]=8770>>8;p[55]=8770&255;p[56]=55000>>8;p[57]=55000&255;
 p[58]=0x12;p[59]=0x34;p[60]=0x56;p[61]=0x78;p[62]=0xaa;p[63]=0xbb;p[64]=0xcc;p[65]=0xdd;p[66]=8<<4;p[67]=0x18;
 struct hk_tcp_trace t;assert(hk_trace_packet(p,128,peer,&t));assert(t.src==8770&&t.dst==55000&&t.seq==0x12345678&&t.ack==0xaabbccdd&&t.header==32&&t.payload==42&&t.data_offset==86);
 for(unsigned n=0;n<86;n++)assert(!hk_trace_packet(p,n,peer,&t));
 p[24]=9;assert(hk_trace_packet(p,128,peer,&t));p[24]=0;
 p[37]=8;assert(!hk_trace_packet(p,128,peer,&t));assert(hk_trace_packet(p,128,NULL,&t));p[37]=7;
 memcpy(p+38,peer,16);memset(p+22,0,16);p[54]=55000>>8;p[55]=55000&255;p[56]=8770>>8;p[57]=8770&255;assert(hk_trace_packet(p,128,peer,&t)&&t.dst==8770);
 p[66]=4<<4;assert(!hk_trace_packet(p,128,peer,&t));p[66]=8<<4;p[19]=31;assert(!hk_trace_packet(p,128,peer,&t));
 return 0;
}
