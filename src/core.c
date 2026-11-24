#include "siege.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
_Atomic int stopped;
int64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * SECOND + t.tv_nsec;
}
int64_t due_ns(int64_t slot, int rps) {
    return slot / rps * SECOND + slot % rps * SECOND / rps;
}
int64_t latest_slot(int64_t elapsed, int rps) {
    return elapsed / SECOND * rps + ((elapsed % SECOND + 1) * rps - 1) / SECOND;
}
static bool local_url(const char *url) {
    CURLU *u = curl_url();
    char *host = NULL, *scheme = NULL;
    bool ok = u && strncmp(url, "http://", 7) == 0 &&
        curl_url_set(u, CURLUPART_URL, url, CURLU_DISALLOW_USER) == CURLUE_OK &&
        curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
        strcmp(scheme, "http") == 0 &&
        curl_url_get(u, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
        (!strcmp(host,"localhost") || !strcmp(host,"127.0.0.1") || !strcmp(host,"[::1]"));
    curl_free(host);
    curl_free(scheme);
    curl_url_cleanup(u);
    return ok;
}
static int number(const char *s, const char *suffix, int max) {
    if (*s < '0' || *s > '9') return 0;
    char *end;
    long n = strtol(s, &end, 10);
    return n > 0 && n <= max && strcmp(end,suffix) == 0 ? (int)n : 0;
}
bool parse_config(int argc, char **argv, Config *c) {
    *c = (Config){"http://localhost:8080/",100,10,2,16,2000};
    if (argc < 2 || strcmp(argv[1],"barrage") || argc % 2) return false;
    for (int i=2; i<argc; i+=2) {
        const char *k=argv[i], *v=argv[i+1];
        if (!strcmp(k,"--url")) c->url=v;
        else if (!strcmp(k,"--rps")) c->rps=number(v,"",20000);
        else if (!strcmp(k,"--duration")) {
            c->seconds=number(v,"s",3600);
            if (!c->seconds) c->seconds=60*number(v,"m",60);
        }
        else if (!strcmp(k,"--workers")) c->workers=number(v,"",MAX_WORKERS);
        else if (!strcmp(k,"--connections")) c->connections=number(v,"",512);
        else if (!strcmp(k,"--timeout-ms")) c->timeout_ms=number(v,"",30000);
        else return false;
    }
    return c->seconds && c->rps && c->workers && c->connections >= c->workers &&
           c->timeout_ms && local_url(c->url);
}
bool metrics_init(Metrics *m) {
    return !hdr_init(1,INT64_C(86400000000),3,&m->service) &&
           !hdr_init(1,INT64_C(86400000000),3,&m->scheduled) &&
           !hdr_init(1,INT64_C(86400000000),3,&m->lag);
}
void metrics_free(Metrics *m) {
    if (m->service) hdr_close(m->service);
    if (m->scheduled) hdr_close(m->scheduled);
    if (m->lag) hdr_close(m->lag);
}
void metrics_merge(Metrics *d, const Metrics *s) {
    d->admitted+=s->admitted;
    d->emitted+=s->emitted;
    d->transport+=s->transport;
    d->timeouts+=s->timeouts;
    d->cancelled+=s->cancelled;
    d->skipped+=s->skipped;
    d->busy+=s->busy;
    d->new_connections+=s->new_connections;
    d->bytes+=s->bytes;
    for (int i=0;i<600;i++) d->http[i]+=s->http[i];
    hdr_add(d->service,s->service);
    hdr_add(d->scheduled,s->scheduled);
    hdr_add(d->lag,s->lag);
}
