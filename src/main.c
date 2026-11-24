#include "siege.h"
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

_Static_assert(ATOMIC_INT_LOCK_FREE==2,"signal handler requires lock-free int");
static void stop(int sig) { atomic_store(&stopped,sig); }
static void histogram(const char *key,struct hdr_histogram *h) {
    printf(",\n  \"%s_us\": ",key);
    if (!h->total_count) { fputs("null",stdout); return; }
    printf("{\"n\":%" PRId64 ",\"min\":%" PRId64 ",\"mean\":%.2f,\"max\":%" PRId64,
           h->total_count,hdr_min(h),hdr_mean(h),hdr_max(h));
    printf(",\"stddev\":%.2f",hdr_stddev(h));
    const double p[]={50,90,95,99,99.9};
    for (int i=0;i<5;i++) printf(",\"p%g\":%" PRId64,p[i],hdr_value_at_percentile(h,p[i]));
    putchar('}');
}
static void report(const Config *c,Metrics *m,double elapsed,bool failed) {
    long completed=0,errors=0;
    for (int i=100;i<600;i++) {
        completed+=m->http[i];
        if (i>=400) errors+=m->http[i];
    }
    printf("{\n  \"mode\":\"barrage\",\"status\":\"%s\",",failed?"failed":atomic_load(&stopped)?"interrupted":"finished");
    printf("\n  \"target_rps\":%d,\"duration_s\":%d,\"workers\":%d,\"connections\":%d,\"timeout_ms\":%d,",
           c->rps,c->seconds,c->workers,c->connections,c->timeout_ms);
    printf("\n  \"elapsed_s\":%.6f,\"planned\":%ld,\"admitted\":%ld,\"emitted\":%ld,\"completed\":%ld,",
           elapsed,(long)c->seconds*c->rps,m->admitted,m->emitted,completed);
    printf("\n  \"missed_schedule\":%ld,\"pool_full\":%ld,\"cancelled\":%ld,",
           m->skipped,m->busy,m->cancelled);
    printf("\n  \"emitted_rps\":%.3f,\"completed_rps\":%.3f,",
           elapsed>0?m->emitted/elapsed:0,elapsed>0?completed/elapsed:0);
    printf("\n  \"http_errors\":%ld,\"transport_errors\":%ld,\"timeouts\":%ld,",
           errors,m->transport,m->timeouts);
    printf("\n  \"new_connections\":%ld,\"body_bytes\":%" PRId64 ",\"http\":{",m->new_connections,m->bytes);
    bool first=true;
    for (int i=100;i<600;i++) if (m->http[i]) {
        printf("%s\"%d\":%ld",first?"":",",i,m->http[i]);
        first=false;
    }
    putchar('}');
    histogram("service",m->service);
    histogram("scheduled",m->scheduled);
    histogram("scheduler_lag",m->lag);
    puts("\n}");
}
int main(int argc,char **argv) {
    const char *usage="siegebreaker barrage [--url http://localhost:8080/] [--rps 100] "
        "[--duration 10s] [--workers 2] [--connections 16] [--timeout-ms 2000]";
    if (argc==2 && !strcmp(argv[1],"--help")) { puts(usage); return 0; }
    if (curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) return 1;
    Config c;
    if (!parse_config(argc,argv,&c)) {
        fprintf(stderr,"Configuração inválida. %s\n",usage);
        curl_global_cleanup();
        return 2;
    }
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE,&limit) || limit.rlim_cur<(rlim_t)(c.connections+6*c.workers+16)) {
        fputs("Limite de descritores insuficiente para esta configuração.\n",stderr);
        curl_global_cleanup();
        return 2;
    }
    struct sigaction action={.sa_handler=stop};
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM,&action,NULL);
    sigaction(SIGINT,&action,NULL);
    signal(SIGPIPE,SIG_IGN);
    Worker workers[MAX_WORKERS]={0};
    pthread_t threads[MAX_WORKERS];
    Metrics merged={0};
    bool ok=metrics_init(&merged);
    for (int i=0;ok && i<c.workers;i++) ok=metrics_init(&workers[i].metrics);
    int64_t start=now_ns()+SECOND/10, finish=start;
    int n=0;
    while (ok && n<c.workers) {
        Worker *w=&workers[n];
        w->id=n;
        w->start=start;
        w->config=&c;
        w->capacity=c.connections/c.workers+(n<c.connections%c.workers);
        ok=pthread_create(&threads[n],NULL,worker_run,w)==0;
        if (ok) n++;
    }
    if (!ok) atomic_store(&stopped,1);
    for (int i=0;i<n;i++) {
        pthread_join(threads[i],NULL);
        if (workers[i].failed) ok=false;
        if (workers[i].finish>finish) finish=workers[i].finish;
        metrics_merge(&merged,&workers[i].metrics);
    }
    int sig=atomic_load(&stopped);
    if (!sig && finish<start+c.seconds*SECOND) finish=start+c.seconds*SECOND;
    if (merged.lag) report(&c,&merged,(double)(finish-start)/SECOND,!ok);
    if (!ok) fputs("Falha interna do gerador.\n",stderr);
    metrics_free(&merged);
    for (int i=0;i<c.workers;i++) metrics_free(&workers[i].metrics);
    curl_global_cleanup();
    return !ok?1:sig?128+sig:0;
}
