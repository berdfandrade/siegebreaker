#include "siege.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"failed line %d\n",__LINE__); return 1; } } while (0)
int main(void) {
    CHECK(curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK);
    char *args[]={"siege","barrage","--duration","3m","--rps","999"};
    Config c;
    CHECK(parse_config(6,args,&c) && c.seconds==180 && c.rps==999);
    args[3]="3min";
    CHECK(!parse_config(6,args,&c));
    for (int r=1;r<20000;r+=71) {
        for (int i=1;i<100000;i+=719) {
            int64_t due=due_ns(i,r);
            CHECK(latest_slot(due,r)==i && latest_slot(due-1,r)==i-1);
        }
    }
    Metrics a={0},b={0};
    CHECK(metrics_init(&a) && metrics_init(&b));
    hdr_record_value(a.service,100);
    hdr_record_value(b.service,200);
    b.http[500]=3;
    b.transport=2;
    metrics_merge(&a,&b);
    CHECK(a.http[500]==3 && a.transport==2 && a.service->total_count==2);
    CHECK(hdr_value_at_percentile(a.service,99)==200);
    metrics_free(&a); metrics_free(&b); curl_global_cleanup();
    return 0;
}
