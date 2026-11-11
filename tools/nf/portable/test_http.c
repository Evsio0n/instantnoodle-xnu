/* socketpair only, with the real HTTP parser and a fake on-device sysctl. */
#include <assert.h>
#include <sys/sysctl.h>
static int mock_sysctl(const char *,void *,size_t *,void *,size_t);
#define sysctlbyname mock_sysctl
#define main portable_server_main
#define PORTABLE_HTTP_TEST 1
#include "portable_http.c"
#undef main
static int calls, last;
static int mock_sysctl(const char *name,void *value,size_t *length,void *in,size_t n)
{
    if(!strcmp(name,"kern.hackios.portable_result")){
        assert(!in && *length==sizeof(unsigned)*7);
        unsigned *v=value;v[0]=v[1]=v[2]=1;return 0;
    }
    assert(!strcmp(name,"kern.hackios.portable_gnss") && *length==sizeof(unsigned));
    if(in){assert(n==sizeof(int));calls++;last=*(int *)in;}
    *(unsigned *)value=2;
    return 0;
}
static void query(const char *request,const char *expected,int writes)
{
    int sockets[2];char result[6000];calls=0;
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    assert(write(sockets[0],request,strlen(request))==(ssize_t)strlen(request));
    shutdown(sockets[0],SHUT_WR);serve(sockets[1]);close(sockets[1]);
    ssize_t n=read(sockets[0],result,sizeof(result)-1);assert(n>0);result[n]=0;
    assert(strstr(result,expected));assert(calls==writes);close(sockets[0]);
}
#define HOST "Host: 127.0.0.1:8765\r\n"
#define ORG "Origin: http://127.0.0.1:8765\r\n"
#define KEY "X-Portable: abc\r\n"
int main(void)
{
    strcpy(token,"abc");
    query("GET / HTTP/1.1\r\n" HOST "\r\n","ev.isTrusted",0);
    query("GET /result HTTP/1.1\r\n" HOST "\r\n","\"rehearsal\":1",0);
    query("GET /state HTTP/1.1\r\n" HOST "\r\n","200 OK",0);
    query("GET /start HTTP/1.1\r\n" HOST "\r\n","403 Rejected",0);
    query("POST /start HTTP/1.1\r\n" HOST KEY "\r\n","403 Rejected",0);
    query("POST /start HTTP/1.1\r\n" HOST ORG "\r\n","403 Rejected",0);
    query("POST /start HTTP/1.1\r\n" HOST HOST ORG KEY "\r\n","403 Rejected",0);
    query("POST /start HTTP/1.1\r\n" HOST "Origin: https://example.invalid\r\n" KEY "\r\n","403 Rejected",0);
    query("POST /start HTTP/1.1\r\n" HOST ORG KEY "Content-Length: 1\r\n\r\nx","403 Rejected",0);
    query("POST /ready HTTP/1.1\r\n" HOST ORG KEY "\r\n","200 OK",1);assert(last==1);
    query("POST /start HTTP/1.1\r\n" HOST ORG KEY "\r\n","200 OK",1);assert(last==2);
    query("POST /stop HTTP/1.1\r\n" HOST ORG KEY "\r\n","200 OK",1);assert(last==3);
    return 0;
}
