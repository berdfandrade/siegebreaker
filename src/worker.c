#include "siege.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

typedef struct Slot {
    CURL *easy;
    int64_t due, start;
    bool active, wrote;
    struct Slot *next;
    Metrics *metrics;
} Slot;

typedef struct {
    Worker *worker;
    int pollfd, timer, active;
    CURLM *multi;
    int64_t curl_deadline;
    Slot *slots, *available;
} Loop;

static curl_socket_t local_socket(void *unused, curlsocktype purpose,
                                  struct curl_sockaddr *address) {
    (void)unused;
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKET_BAD;
    if (address->family == AF_INET) {
        const struct sockaddr_in *v4 = (const void *)&address->addr;
        if ((ntohl(v4->sin_addr.s_addr) >> 24) != 127) return CURL_SOCKET_BAD;
    } else if (address->family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const void *)&address->addr;
        if (!IN6_IS_ADDR_LOOPBACK(&v6->sin6_addr)) return CURL_SOCKET_BAD;
    } else return CURL_SOCKET_BAD;
    return socket(address->family,address->socktype | SOCK_CLOEXEC,address->protocol);
}

static size_t discard(char *data,size_t size,size_t count,void *unused) {
    (void)data; (void)unused;
    return size*count;
}
static int debug_event(CURL *easy,curl_infotype type,char *data,size_t len,void *arg) {
    (void)easy; (void)data; (void)len;
    Slot *slot=arg;
    if (type==CURLINFO_HEADER_OUT && !slot->wrote) {
        slot->metrics->emitted++;
        slot->wrote=true;
    }
    return 0;
}
static bool prepare(Slot *s,const Config *c) {
    s->easy=curl_easy_init();
    if (!s->easy) return false;
#define SET(option,value) if (curl_easy_setopt(s->easy,option,value)!=CURLE_OK) return false
    SET(CURLOPT_URL,c->url);
    SET(CURLOPT_PROTOCOLS_STR,"http");
    SET(CURLOPT_PROXY,"");
    SET(CURLOPT_HTTP_VERSION,(long)CURL_HTTP_VERSION_1_1);
    SET(CURLOPT_NOSIGNAL,1L);
    SET(CURLOPT_TIMEOUT_MS,(long)c->timeout_ms);
    SET(CURLOPT_OPENSOCKETFUNCTION,local_socket);
    SET(CURLOPT_WRITEFUNCTION,discard);
    SET(CURLOPT_PRIVATE,s);
    SET(CURLOPT_DEBUGFUNCTION,debug_event);
    SET(CURLOPT_DEBUGDATA,s);
    SET(CURLOPT_VERBOSE,1L);
#undef SET
    return true;
}

static int watch(CURL *easy,curl_socket_t fd,int action,void *arg,void *unused) {
    (void)easy; (void)unused;
    Loop *l=arg;
    if (action==CURL_POLL_REMOVE) {
        epoll_ctl(l->pollfd,EPOLL_CTL_DEL,fd,NULL);
        return 0;
    }
    struct epoll_event e={.data.fd=fd};
    if (action==CURL_POLL_IN || action==CURL_POLL_INOUT) e.events|=EPOLLIN;
    if (action==CURL_POLL_OUT || action==CURL_POLL_INOUT) e.events|=EPOLLOUT;
    if (epoll_ctl(l->pollfd,EPOLL_CTL_MOD,fd,&e)==0) return 0;
    return epoll_ctl(l->pollfd,EPOLL_CTL_ADD,fd,&e);
}
static int set_deadline(CURLM *multi,long ms,void *arg) {
    (void)multi;
    Loop *l=arg;
    l->curl_deadline=ms<0 ? INT64_MAX : now_ns()+ms*INT64_C(1000000);
    return 0;
}
static bool drive(Loop *l,int fd,int events) {
    int running,remaining;
    if (curl_multi_socket_action(l->multi,fd,events,&running)!=CURLM_OK) return false;
    CURLMsg *msg;
    Metrics *m=&l->worker->metrics;
    while ((msg=curl_multi_info_read(l->multi,&remaining))) {
        if (msg->msg!=CURLMSG_DONE) continue;
        Slot *s=NULL;
        long code=0, connects=0;
        curl_off_t bytes=0;
        curl_easy_getinfo(msg->easy_handle,CURLINFO_PRIVATE,&s);
        curl_easy_getinfo(s->easy,CURLINFO_RESPONSE_CODE,&code);
        curl_easy_getinfo(s->easy,CURLINFO_NUM_CONNECTS,&connects);
        curl_easy_getinfo(s->easy,CURLINFO_SIZE_DOWNLOAD_T,&bytes);
        m->new_connections+=connects;
        m->bytes+=bytes;
        if (msg->data.result==CURLE_OK && code>=100 && code<600) {
            int64_t end=now_ns();
            m->http[code]++;
            if (!hdr_record_value(m->service,(end-s->start)/1000) ||
                !hdr_record_value(m->scheduled,(end-s->due)/1000)) return false;
        } else {
            m->transport++;
            if (msg->data.result==CURLE_OPERATION_TIMEDOUT) m->timeouts++;
        }
        if (curl_multi_remove_handle(l->multi,s->easy)!=CURLM_OK) return false;
        s->active=false;
        s->next=l->available;
        l->available=s;
        l->active--;
    }
    return true;
}
// Each worker owns one residue class of the global slot sequence.
static bool schedule(Loop *l,int64_t now,int64_t *next,int64_t total) {
    Worker *w=l->worker;
    const Config *c=w->config;
    int64_t elapsed=now-w->start;
    if (elapsed<0 || *next>=total) return true;
    int64_t latest=latest_slot(elapsed,c->rps);
    if (latest>=total) latest=total-1;
    if (*next>latest) return true;
    int64_t missed=(latest-*next)/c->workers;
    w->metrics.skipped+=missed;
    *next+=missed*c->workers;
    int64_t due=w->start+due_ns(*next,c->rps);
    *next+=c->workers;
    if (elapsed>=c->seconds*SECOND) {
        w->metrics.skipped++;
        return true;
    }
    if (!l->available) {
        w->metrics.busy++;
        return true;
    }
    Slot *s=l->available;
    l->available=s->next;
    s->start=now;
    s->due=due;
    s->wrote=false;
    if (!hdr_record_value(w->metrics.lag,(now-due)/1000) ||
        curl_multi_add_handle(l->multi,s->easy)!=CURLM_OK) return false;
    s->active=true;
    l->active++;
    w->metrics.admitted++;
    return true;
}
static bool run(Loop *l) {
    Worker *w=l->worker;
    const Config *c=w->config;
    int64_t next=w->id, total=(int64_t)c->seconds*c->rps;
    for (;;) {
        int64_t now=now_ns();
        if (!atomic_load(&stopped) && !schedule(l,now,&next,total)) return false;
        if ((atomic_load(&stopped) || next>=total) && !l->active) return true;
        if (l->curl_deadline<=now_ns()) {
            l->curl_deadline=INT64_MAX;
            if (!drive(l,CURL_SOCKET_TIMEOUT,0)) return false;
            continue;
        }
        int64_t wake=now+SECOND/10; // Bound signal response while idle.
        if (!atomic_load(&stopped) && next<total) {
            int64_t due=w->start+due_ns(next,c->rps);
            if (due<wake) wake=due;
        }
        if (l->curl_deadline<wake) wake=l->curl_deadline;
        struct itimerspec t={.it_value={wake/SECOND,wake%SECOND}};
        if (timerfd_settime(l->timer,TFD_TIMER_ABSTIME,&t,NULL)) return false;
        struct epoll_event ev[64];
        int n=epoll_wait(l->pollfd,ev,64,-1);
        if (n<0 && errno!=EINTR) return false;
        for (int i=0;i<n;i++) {
            if (ev[i].data.fd==l->timer) {
                uint64_t ticks;
                (void)read(l->timer,&ticks,sizeof(ticks));
                continue;
            }
            int flags=0;
            if (ev[i].events & (EPOLLIN|EPOLLHUP)) flags|=CURL_CSELECT_IN;
            if (ev[i].events & EPOLLOUT) flags|=CURL_CSELECT_OUT;
            if (ev[i].events & EPOLLERR) flags|=CURL_CSELECT_ERR;
            if (!drive(l,ev[i].data.fd,flags)) return false;
        }
    }
}
void *worker_run(void *arg) {
    Worker *w=arg;
    Loop l={.worker=w,.pollfd=epoll_create1(EPOLL_CLOEXEC),
            .timer=timerfd_create(CLOCK_MONOTONIC,TFD_CLOEXEC|TFD_NONBLOCK),
            .multi=curl_multi_init(),.curl_deadline=INT64_MAX};
    struct epoll_event e={.events=EPOLLIN,.data.fd=l.timer};
    l.slots=calloc((size_t)w->capacity,sizeof(Slot));
    bool ok=l.slots && l.multi && l.timer>=0 && l.pollfd>=0 &&
        epoll_ctl(l.pollfd,EPOLL_CTL_ADD,l.timer,&e)==0 &&
        curl_multi_setopt(l.multi,CURLMOPT_SOCKETFUNCTION,watch)==CURLM_OK &&
        curl_multi_setopt(l.multi,CURLMOPT_SOCKETDATA,&l)==CURLM_OK &&
        curl_multi_setopt(l.multi,CURLMOPT_TIMERFUNCTION,set_deadline)==CURLM_OK &&
        curl_multi_setopt(l.multi,CURLMOPT_TIMERDATA,&l)==CURLM_OK &&
        curl_multi_setopt(l.multi,CURLMOPT_MAX_TOTAL_CONNECTIONS,(long)w->capacity)==CURLM_OK;
    for (int i=0;ok && i<w->capacity;i++) {
        Slot *s=&l.slots[i];
        ok=prepare(s,w->config);
        s->metrics=&w->metrics;
        s->next=l.available;
        l.available=s;
    }
    if (ok) ok=run(&l);
    w->finish=now_ns();
    if (!ok) {
        w->failed=true;
        atomic_store(&stopped,1);
    }
    for (int i=0;l.slots && i<w->capacity;i++) {
        if (l.slots[i].active) {
            curl_multi_remove_handle(l.multi,l.slots[i].easy);
            w->metrics.cancelled++;
        }
        curl_easy_cleanup(l.slots[i].easy);
    }
    if (l.multi) curl_multi_cleanup(l.multi);
    free(l.slots);
    if (l.timer>=0) close(l.timer);
    if (l.pollfd>=0) close(l.pollfd);
    return NULL;
}
