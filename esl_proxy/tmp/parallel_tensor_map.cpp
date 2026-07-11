typedef struct {
    uint64_t base;
    int32_t next;
    uint64_t offset;
    uint32_t producer_id;
    int32_t version;
    uint32_t ndims;
    uint8_t dtype;
    uint8_t manual_dep;
    uint8_t is_contiguous;
    uint32_t shapes[5];
    uint32_t strides[5];
    uint64_t extent_elem_cache;
} TmEntry;

while (true) {
    long current_desc = atomic_load(&desc_taskid);
    bool is_done = atomic_load(&all_task_desc_done);
    long current_detector = atomic_load(&detector_taskid);
    if (current_detector >= current_desc && is_done)
        return;

    long prev_detector_taskid = atomic_fetch_add(&detector_taskid, DECTER_BATCH_SIZE);
    long max_detector_taskid = prev_detector_taskid + DECTER_BATCH_SIZE;

    while (max_detector_taskid > current_desc) {
        if (atomic_load(&all_task_desc_done)) {
            current_desc = atomic_load(&desc_taskid);
            if (max_detector_taskid > current_desc)
                max_detector_taskid = current_desc;
            if (prev_detector_taskid >= max_detector_taskid)
                return;
            break;
        }
        wait(); 
    }

    update_tensor_map(min_uncompleted, local_tensormap);
    for (long i = prev_detector_taskid; i < max_detector_taskid; i++) {
        for (const auto& in_tensor : get_in_tensors(i)) {
            check_overlap(local_tensormap, in_tensor, predecessor[i]);
        }
    }
}