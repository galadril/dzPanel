#include "mini_test.h"
#include "ru_osal.h"

#define N 1000
static void producer(void *arg) { ru_queue_t *q = arg; for (int i = 0; i < N; i++) ru_queue_send(q, &i, RU_WAIT_FOREVER); }
static ru_mutex_t *mx; static int counter;
static void incr(void *arg) { (void)arg; for (int i = 0; i < 10000; i++) { ru_mutex_lock(mx); counter++; ru_mutex_unlock(mx); } }

int main(void) {
  ru_queue_t *q = ru_queue_create(sizeof(int), 4);
  int x;
  CHECK(ru_queue_recv(q, &x, 0) == RU_ERR_TIMEOUT);
  CHECK(ru_queue_recv(q, &x, 20) == RU_ERR_TIMEOUT);
  for (int i = 0; i < 4; i++) CHECK(ru_queue_send(q, &i, 0) == RU_OK);
  CHECK(ru_queue_send(q, &x, 0) == RU_ERR_FULL);
  CHECK(ru_queue_send(q, &x, 20) == RU_ERR_TIMEOUT);
  CHECK(ru_queue_count(q) == 4);
  for (int i = 0; i < 4; i++) { CHECK(ru_queue_recv(q, &x, 0) == RU_OK); CHECK(x == i); }

  ru_task_t *t = ru_task_create("prod", producer, q);
  long sum = 0, order_ok = 1;
  for (int i = 0; i < N; i++) { ru_queue_recv(q, &x, RU_WAIT_FOREVER); if (x != i) order_ok = 0; sum += x; }
  ru_task_join(t);
  CHECK(order_ok); CHECK(sum == (long)N * (N - 1) / 2);
  ru_queue_destroy(q);

  mx = ru_mutex_create();
  ru_task_t *ts[4];
  for (int i = 0; i < 4; i++) ts[i] = ru_task_create("inc", incr, NULL);
  for (int i = 0; i < 4; i++) ru_task_join(ts[i]);
  CHECK(counter == 40000);
  ru_mutex_destroy(mx);

  uint32_t a = ru_now_ms(); ru_sleep_ms(30); CHECK(ru_now_ms() - a >= 29);
  MT_DONE();
}
