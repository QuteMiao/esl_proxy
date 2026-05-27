#include "dag/mem_pool.h"
#include <stdio.h>
#include <assert.h>

void test_basic_alloc_free(void) {
    printf("Test: basic_alloc_free\n");

    mem_pool_config_t config = {.pool_size = 64 * 1024};
    int ret = mem_pool_init(&config);
    assert(ret == 0);

    buffer_handle_t *buf = mem_pool_alloc(1024);
    assert(buf != NULL);
    assert(buf->size == 1024);

    ret = mem_pool_free(buf->addr);
    assert(ret == 0);

    printf("  PASSED\n");
}

void test_multiple_alloc(void) {
    printf("Test: multiple_alloc\n");

    mem_pool_config_t config = {.pool_size = 64 * 1024};
    int ret = mem_pool_init(&config);
    assert(ret == 0);

    buffer_handle_t *buf1 = mem_pool_alloc(1024);
    buffer_handle_t *buf2 = mem_pool_alloc(2048);
    buffer_handle_t *buf3 = mem_pool_alloc(512);

    assert(buf1 != NULL);
    assert(buf2 != NULL);
    assert(buf3 != NULL);

    printf("  buf1 addr=%p size=%zu\n", buf1->addr, buf1->size);
    printf("  buf2 addr=%p size=%zu\n", buf2->addr, buf2->size);
    printf("  buf3 addr=%p size=%zu\n", buf3->addr, buf3->size);

    printf("  PASSED\n");
}

void test_pool_exhaustion(void) {
    printf("Test: pool_exhaustion\n");

    mem_pool_config_t config = {.pool_size = 1024};
    int ret = mem_pool_init(&config);
    assert(ret == 0);

    printf("  total_size=%zu, allocated=%zu, available=%zu\n",
           mem_pool_total_size(), mem_pool_allocated(), mem_pool_available());

    buffer_handle_t *buf1 = mem_pool_alloc(1024);
    printf("  buf1=%p\n", (void *)buf1);
    assert(buf1 != NULL);

    printf("  after buf1: total=%zu, allocated=%zu, available=%zu\n",
           mem_pool_total_size(), mem_pool_allocated(), mem_pool_available());

    buffer_handle_t *buf2 = mem_pool_alloc(1024);
    printf("  buf2=%p\n", (void *)buf2);
    assert(buf2 == NULL);

    printf("  PASSED\n");
}

void test_metadata(void) {
    printf("Test: metadata\n");

    mem_pool_config_t config = {.pool_size = 64 * 1024};
    int ret = mem_pool_init(&config);
    assert(ret == 0);

    assert(mem_pool_total_size() == 64 * 1024);
    assert(mem_pool_available() == 64 * 1024);

    buffer_handle_t *buf = mem_pool_alloc(1024);
    assert(buf != NULL);

    assert(mem_pool_allocated() == 1024);
    assert(mem_pool_available() == 64 * 1024 - 1024);

    printf("  PASSED\n");
}

void test_when2free(void) {
    printf("Test: when2free\n");

    mem_pool_config_t config = {.pool_size = 64 * 1024};
    int ret = mem_pool_init(&config);
    assert(ret == 0);

    buffer_handle_t *buf = mem_pool_alloc(1024);
    assert(buf != NULL);

    ret = mem_pool_when2free(buf->addr, buf->size, 5);
    assert(ret == 0);

    mem_pool_process_when2free(6);

    printf("  PASSED\n");
}

void test_wraparound(void) {
    printf("Test: wraparound\n");

    mem_pool_config_t config = {.pool_size = 1024 * 1024};
    int ret = mem_pool_init(&config);
    assert(ret == 0);

    for (int i = 0; i < 100; i++) {
        buffer_handle_t *buf = mem_pool_alloc(100);
        if (buf == NULL) {
            printf("  Allocation failed at iteration %d\n", i);
            break;
        }
        mem_pool_free(buf->addr);
    }

    printf("  PASSED\n");
}

int main(void) {
    printf("=== Memory Pool Tests ===\n\n");

    test_basic_alloc_free();
    test_multiple_alloc();
    test_pool_exhaustion();
    test_metadata();
    test_when2free();
    test_wraparound();

    printf("\n=== All Tests Passed ===\n");
    return 0;
}