#ifndef UI_SKETCH_JOB_H
#define UI_SKETCH_JOB_H
#include "ui/sketch_model.h"
typedef struct SketchExportJob {
    _Atomic i32 refs;
    _Atomic bool cancelled, done;
    SketchDocument snapshot;
    u8 *png;
    usize len;
    char error[128];
} SketchExportJob;
SketchExportJob *sketch_job_start(const SketchDocument *document);
void sketch_job_release(SketchExportJob *job);
void sketch_job_cancel(SketchExportJob *job);
bool sketch_job_ready(const SketchExportJob *job);
#endif
