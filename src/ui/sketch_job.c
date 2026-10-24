#include "ui/sketch_job.h"
#include "core/mem_budget.h"
#include "platform/platform.h"
#include <pthread.h>
#include <stdlib.h>
#define SKETCH_JOB_LIMIT 2
/* Charge a conservative peak, including the bounded RGB/filter/PNG buffers. */
#define SKETCH_JOB_CHARGE ((usize)sizeof(SketchExportJob)+(usize)SKETCH_MAX_DIM*SKETCH_MAX_DIM*10+65536u)
static _Atomic i32 g_jobs;
void sketch_job_release(SketchExportJob *j) {
    if(!j||atomic_fetch_sub_explicit(&j->refs,1,memory_order_acq_rel)!=1)return;
    free(j->png);mem_budget_adjust(MEM_IMAGE_CPU,-(isize)SKETCH_JOB_CHARGE);
    atomic_fetch_sub_explicit(&g_jobs,1,memory_order_relaxed);free(j);
}
void sketch_job_cancel(SketchExportJob *j) { if(j)atomic_store_explicit(&j->cancelled,true,memory_order_relaxed); }
bool sketch_job_ready(const SketchExportJob *j) {return j&&atomic_load_explicit(&j->done,memory_order_acquire);}
static void *export_worker(void *arg) {
    SketchExportJob *j=arg;
    sketch_export_png(&j->snapshot,&j->cancelled,&j->png,&j->len,j->error,sizeof j->error);
    atomic_store_explicit(&j->done,true,memory_order_release);
    if(!atomic_load_explicit(&j->cancelled,memory_order_relaxed))platform_wake_event_loop();
    sketch_job_release(j);return NULL;
}
SketchExportJob *sketch_job_start(const SketchDocument *d) {
    if(!sketch_document_valid(d))return NULL;
    i32 count=atomic_load_explicit(&g_jobs,memory_order_relaxed);
    do {if(count>=SKETCH_JOB_LIMIT)return NULL;}
    while(!atomic_compare_exchange_weak_explicit(&g_jobs,&count,count+1,memory_order_relaxed,memory_order_relaxed));
    SketchExportJob *j=calloc(1,sizeof *j);
    if(!j){atomic_fetch_sub(&g_jobs,1);return NULL;}
    atomic_init(&j->refs,2);atomic_init(&j->cancelled,false);atomic_init(&j->done,false);
    j->snapshot=*d;mem_budget_adjust(MEM_IMAGE_CPU,(isize)SKETCH_JOB_CHARGE);
    pthread_attr_t attr;
    if(pthread_attr_init(&attr)!=0){atomic_store(&j->refs,1);sketch_job_release(j);return NULL;}
    if(pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED)!=0){pthread_attr_destroy(&attr);atomic_store(&j->refs,1);sketch_job_release(j);return NULL;}
    pthread_t thread;int result=pthread_create(&thread,&attr,export_worker,j);pthread_attr_destroy(&attr);
    if(result){atomic_store(&j->refs,1);sketch_job_release(j);return NULL;}
    return j;
}
