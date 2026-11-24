#ifndef SIEGE_H
#define SIEGE_H
#include <curl/curl.h>
#include <hdr/hdr_histogram.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#define SECOND INT64_C(1000000000)
#define MAX_WORKERS 16
typedef struct {
    const char *url;
    int rps, seconds, workers, connections, timeout_ms;
} Config;
typedef struct {
    long admitted, emitted, http[600], transport, timeouts, cancelled;
    long skipped, busy, new_connections;
    int64_t bytes;
    struct hdr_histogram *service, *scheduled, *lag;
} Metrics;
typedef struct {
    const Config *config;
    int id, capacity;
    int64_t start, finish;
    Metrics metrics;
    bool failed;
} Worker;
extern _Atomic int stopped;
int64_t now_ns(void);
int64_t due_ns(int64_t slot, int rps);
int64_t latest_slot(int64_t elapsed, int rps);
bool parse_config(int argc, char **argv, Config *config);
bool metrics_init(Metrics *m);
void metrics_free(Metrics *m);
void metrics_merge(Metrics *dst, const Metrics *src);
void *worker_run(void *arg);
#endif
