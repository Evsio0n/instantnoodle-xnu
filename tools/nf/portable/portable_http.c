/* A foreground Safari control on this phone only. No Internet resources,
 * cookies, file server, configurable targets, or coordinate API. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#define ORIGIN "http://127.0.0.1:8765"
static const char host[] = "127.0.0.1:8765";
static char token[33];

static void rejection(const char *kind,int action,int code)
{
#ifdef PORTABLE_HTTP_TEST
    (void)kind;(void)action;(void)code;
#else
    int fd=open("/dev/console",O_WRONLY|O_NOCTTY);
    if(fd>=0){dprintf(fd,"portable-http-v2 action=%d outcome=%s code=%d end\n",action,kind,code);close(fd);}
#endif
}
static void reply(int fd, int code, const char *kind, const char *body)
{
    char head[768];
    snprintf(head,sizeof(head),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: close\r\nCache-Control: no-store\r\nX-Frame-Options: DENY\r\n"
        "Referrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\n"
        "Content-Security-Policy: default-src 'none'; connect-src 'self'; script-src 'nonce-%s'; "
        "style-src 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n\r\n",
        code,code==200?"OK":"Rejected",kind,strlen(body),token);
    const char *parts[]={head,body};
    for(unsigned i=0;i<2;i++){
        const char *p=parts[i];size_t left=strlen(p);
        while(left){ssize_t n=write(fd,p,left);if(n<=0)return;p+=n;left-=(size_t)n;}
    }
}

static int get_summary(unsigned value[7])
{
    size_t size=7*sizeof(unsigned);
    return !sysctlbyname("kern.hackios.portable_result",value,&size,NULL,0) && size==7*sizeof(unsigned)
        && value[0]==1 && value[1]<=1 && value[2]<=1 && value[3]<=1 && value[4]<=1 && value[6]<=1;
}

static const char page[] =
"<!doctype html><html lang=zh-Hans><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>%s</title><style>body{font:18px system-ui;margin:2rem auto;padding:0 1.2rem;max-width:28rem}button{font:inherit;padding:.7rem;margin:0 .4rem .5rem 0}</style>"
"<h1>%s</h1><p>%s</p>"
"<p id=s role=status>正在读取状态…</p><button id=b disabled>开始测量</button><button id=c disabled>提前结束</button>"
"<script nonce='%s'>const btn=[b,c],labels=['未启用','等待准备','已准备，等待拔线','正在启动','正在测量','已结束，请重新连接','已确认拔线，可以点击开始','暂时无法读取 USB 状态，请稍候','本次操作时间已结束，请重新连接'];"
"async function sync(){try{const r=await fetch('/state',{cache:'no-store'});if(!r.ok)throw 0;const n=Number(await r.text());"
"if(n===1&&document.visibilityState==='visible'){await new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)));"
"if(document.visibilityState==='visible')await fetch('/ready',{method:'POST',headers:{'X-Portable':'%s'}})}s.textContent=labels[n];if(n===5){const out=await fetch('/result',{cache:'no-store'});if(out.ok){const v=await out.json();"
"if(v.rehearsal)s.textContent='拔线演练已完成，未启动调制解调器。请重新连接。';else if(v.complete)s.textContent='测量已完成，位置报告 '+v.reports+' 条，'+(v.fix?'已取得有效定位':'未取得有效定位')+'。请重新连接。';"
"else s.textContent='测量未完整结束，请重新连接。'}}btn.forEach((e,i)=>e.disabled=!(i===0?n===6:n===3||n===4))}catch{btn.forEach(e=>e.disabled=true);s.textContent='状态暂不可用'}}"
"btn.forEach((e,i)=>e.onclick=async ev=>{if(!ev.isTrusted||document.visibilityState!=='visible')return;e.disabled=true;"
"const r=await fetch('/'+['start','stop'][i],{method:'POST',headers:{'X-Portable':'%s'}});await sync();"
"if(!r.ok){const m=await r.text();s.textContent=({cable:'手机仍检测到 USB，请等开始按钮重新点亮',late:'本次操作时间已结束，请重新连接',sensor:'暂时无法读取 USB 状态',state:'当前状态不接受本次操作',origin:'页面来源不符，请重新打开本机页面',token:'本次页面已更新，请刷新',request:'页面请求不完整，请刷新'})[m]||'页面请求未完成，请重新连接'};});sync();setInterval(sync,2000)</script></html>";

static void serve(int fd)
{
    char buf[8192], html[4096], *next, *line;
    const char *reason="request";
    size_t n=0;int hosts=0,origins=0,keys=0, lengths=0, action=0;
    while(n+1<sizeof(buf)){
        ssize_t got=read(fd,buf+n,sizeof(buf)-1-n);
        if(got<=0)return;
        if(memchr(buf+n,0,(size_t)got))goto bad;
        n+=(size_t)got;buf[n]=0;
        if(strstr(buf,"\r\n\r\n"))break;
    }
    if(!strstr(buf,"\r\n\r\n"))goto bad;
    line=buf;next=strstr(line,"\r\n");*next=0;next+=2;
    int home=!strcmp(line,"GET / HTTP/1.1"), state=!strcmp(line,"GET /state HTTP/1.1");
    if(!strcmp(line,"POST /ready HTTP/1.1"))action=1;
    if(!strcmp(line,"POST /start HTTP/1.1"))action=2;
    if(!strcmp(line,"POST /stop HTTP/1.1"))action=3;
    int result=!strcmp(line,"GET /result HTTP/1.1");
    if(!home&&!state&&!result&&!action)goto bad;
    while(*next!='\r'){
        char *end=strstr(next,"\r\n"), *sep=strchr(next,':');
        if(!end||!sep||sep>=end||sep==next||next[0]==' '||next[0]=='\t')goto bad;
        *end=0;*sep++=0;
        while(*sep==' '||*sep=='\t')sep++;
        if(!strcasecmp(next,"Host")){if(strcmp(sep,host))goto bad;hosts++;}
        if(!strcasecmp(next,"Origin")){if(strcmp(sep,ORIGIN)){reason="origin";goto bad;}origins++;}
        if(!strcasecmp(next,"X-Portable")){if(strcmp(sep,token)){reason="token";goto bad;}keys++;}
        if(!strcasecmp(next,"Transfer-Encoding"))goto bad;
        if(!strcasecmp(next,"Content-Length")){if(strcmp(sep,"0"))goto bad;lengths++;}
        next=end+2;
    }
    if(hosts!=1||origins>1||keys>1||lengths>1||strcmp(next,"\r\n"))goto bad;
    if(action&&origins!=1){reason="origin";goto bad;}
    if(action&&keys!=1){reason="token";goto bad;}
    if(home){
        unsigned v[7]={0};
        if(!get_summary(v))goto bad;
        const char *title=v[1]?"USB 拔线演练":"户外 GNSS 测量";
        const char *hint=v[1]?"等电脑提示后拔线，留在电脑旁，等页面显示「已确认拔线」再点击开始。本次演练不会启动定位。完成后接回原 USB 接口。":
            "等电脑提示后拔线，到室外视野开阔处、页面显示「已确认拔线」后再点开始。完成后请接回原来的 USB 接口。";
        int written=snprintf(html,sizeof(html),page,title,title,hint,token,token,token);
        if(written<0||(size_t)written>=sizeof(html))goto bad;
        reply(fd,200,"text/html; charset=UTF-8",html);return;
    }
    if(result){
        unsigned v[7]={0};char body[128];
        if(!get_summary(v)||!v[2])goto bad;
        snprintf(body,sizeof(body),"{\"rehearsal\":%u,\"started\":%u,\"complete\":%u,\"reports\":%u,\"fix\":%u}",
                 v[1],v[3],v[4],v[5],v[6]);
        reply(fd,200,"application/json",body);return;
    }
    unsigned value=0;size_t size=sizeof(value);
    if(sysctlbyname("kern.hackios.portable_gnss",&value,&size,action?&action:NULL,action?sizeof(action):0)||
       size!=sizeof(value)||value>8){
        int code=errno;
        const char *why=code==ETIMEDOUT?"late":code==EAGAIN?"cable":code==ENXIO?"sensor":"state";
        if(action)rejection("kernel",action,code);
        reply(fd,409,"text/plain",why);return;
    }
    char text[8];snprintf(text,sizeof(text),"%u",value);reply(fd,200,"text/plain",text);return;
bad:
    if(action)rejection(reason,action,403);
    reply(fd,403,"text/plain",reason);
}

int main(void)
{
    unsigned char bytes[16];arc4random_buf(bytes,sizeof(bytes));
    for(unsigned i=0;i<sizeof(bytes);i++)snprintf(token+2*i,3,"%02x",bytes[i]);
    signal(SIGPIPE,SIG_IGN);
    int sock=socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(8765),.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
    if(sock<0||bind(sock,(struct sockaddr *)&addr,sizeof(addr))||listen(sock,4))return 1;
    pid_t child=fork();if(child<0)return 1;if(child)return 0;
    for(;;){
        int fd=accept(sock,NULL,NULL);if(fd<0){if(errno==EINTR)continue;return 1;}
        struct timeval limit={3,0};
        setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&limit,sizeof(limit));
        setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&limit,sizeof(limit));
        serve(fd);close(fd);
    }
}
