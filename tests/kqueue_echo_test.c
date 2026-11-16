#include <arpa/inet.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "zg-frame.h"

static int fails;
static void chk(int ok, const char *m){ printf("%s: %s\n", ok?"PASS":"FAIL", m); fflush(stdout); if(!ok) fails++; }

static int send_frame(int fd, const char *text){
    guint32 l=htonl((guint32)strlen(text));
    return (write(fd,&l,4)==4 && write(fd,text,strlen(text))==(ssize_t)strlen(text))?0:-1;
}
static char *recv_frame(int fd){
    guint32 l; char *t;
    if(read(fd,&l,4)!=4) return NULL;
    l=ntohl(l); t=calloc(1,l+1);
    if(!t) return NULL;
    if(read(fd,t,l)!=(ssize_t)l){ free(t); return NULL; }
    return t;
}
static int relay_frame(int kq, ZgFrameBuffer *b, int from, int to){
    for(;;){
        struct kevent ev; struct timespec ts={2,0};
        int n=kevent(kq,NULL,0,&ev,1,&ts);
        if(n!=1){ printf("kevent n=%d errno=%d\n",n,errno); fflush(stdout); return -1; }
        if((int)ev.ident!=from){ printf("unexpected ident %ld\n",(long)ev.ident); return -1; }
        if(ev.flags & EV_EOF) return -1;
        ZgFrameReadResult rr=zg_frame_read(b,from,NULL);
        if(rr==ZG_FRAME_EOF) return -1;
        gchar *f=NULL; gint p=zg_frame_pop(b,&f,NULL);
        if(p<0) return -1;
        if(p==0) continue;   /* need another kevent for the rest */
        int rc=send_frame(to,f); g_free(f);
        return rc;
    }
}
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    int hp[2],cp[2]; struct kevent ch; int kq;
    chk(socketpair(AF_UNIX,SOCK_STREAM,0,hp)==0,"helper socketpair");
    chk(socketpair(AF_UNIX,SOCK_STREAM,0,cp)==0,"client socketpair");
    fcntl(hp[0],F_SETFL,O_NONBLOCK); fcntl(cp[0],F_SETFL,O_NONBLOCK);
    struct timeval tv={2,0};
    setsockopt(cp[0],SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    setsockopt(hp[1],SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    kq=kqueue();
    EV_SET(&ch,hp[0],EVFILT_READ,EV_ADD|EV_ENABLE,0,0,(void*)1); chk(kevent(kq,&ch,1,NULL,0,NULL)==0,"register helper");
    EV_SET(&ch,cp[0],EVFILT_READ,EV_ADD|EV_ENABLE,0,0,(void*)2); chk(kevent(kq,&ch,1,NULL,0,NULL)==0,"register client");
    ZgFrameBuffer *hb=zg_frame_buffer_new(1<<20);
    chk(send_frame(hp[1],"{\"type\":\"started\"}")==0,"helper write started");
    chk(relay_frame(kq,hb,hp[0],cp[0])==0,"relay started");
    char *f=recv_frame(cp[1]); chk(f&&strstr(f,"started"),"client got started"); free(f);
    chk(send_frame(hp[1],"{\"type\":\"progress\",\"bytes\":4096}")==0,"helper write progress");
    chk(relay_frame(kq,hb,hp[0],cp[0])==0,"relay progress");
    f=recv_frame(cp[1]); chk(f&&strstr(f,"progress"),"client got progress"); free(f);
    ZgFrameBuffer *cb=zg_frame_buffer_new(1<<16);
    chk(send_frame(cp[1],"{\"op\":\"cancel\"}")==0,"client write cancel");
    chk(relay_frame(kq,cb,cp[0],hp[0])==0,"relay cancel");
    f=recv_frame(hp[1]); chk(f&&strstr(f,"cancel"),"helper got cancel"); free(f);
    chk(send_frame(hp[1],"{\"type\":\"completed\",\"ok\":true}")==0,"helper write final");
    chk(relay_frame(kq,hb,hp[0],cp[0])==0,"relay final");
    f=recv_frame(cp[1]); chk(f&&strstr(f,"completed"),"client got final"); free(f);
    printf(fails?"KQUEUE_ECHO: FAIL %d\n":"KQUEUE_ECHO: ALL PASS\n",fails);
    return fails?1:0;
}
