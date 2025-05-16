/* computation-node.c */
#include "contiki.h"
#include "net/netstack.h"
#include "net/nullnet/nullnet.h"
#include "lib/random.h"
#include "net/linkaddr.h"
#include <stdio.h>
#include <string.h>

#define HELLO_INTERVAL     (CLOCK_SECOND * 15)
#define WINDOW_SIZE        30
#define MAX_SENSORS        5
#define SLOPE_THRESHOLD    0.5
#define WINDOW_EXPIRY      (5 * 60)  /* seconds */

#ifndef BORDER_NODE_ID
#define BORDER_NODE_ID     1
#endif

/* Per-sensor sliding window */
typedef struct {
  uint8_t        id;                     /* sensor address */
  uint8_t        count;                  /* readings stored */
  uint8_t        idx;                    /* next write index */
  clock_time_t   last_ts;                /* timestamp of last reading */
  uint16_t       values[WINDOW_SIZE];    /* sensor values */
} sensor_window_t;

static uint16_t      my_rank;
static linkaddr_t    parent;
static struct etimer hello_timer;
static sensor_window_t sensors[MAX_SENSORS];

PROCESS(computation_node_process, "Computation node process");
AUTOSTART_PROCESSES(&computation_node_process);

/* Broadcast our rank and log it */
static void
broadcast_rank(void)
{
  nullnet_buf = (uint8_t *)&my_rank;
  nullnet_len = sizeof(my_rank);
  NETSTACK_NETWORK.output(NULL);
  printf("TREE : Node %u: broadcast rank %u\n",
         linkaddr_node_addr.u8[0], my_rank);
}

/* Find or allocate a window slot, expiring old data */
static sensor_window_t *
get_window(uint8_t id)
{
  int i;
  /* Expire stale windows */
  for(i = 0; i < MAX_SENSORS; i++) {
    if(sensors[i].count > 0
       && (clock_seconds() - sensors[i].last_ts) > WINDOW_EXPIRY) {
      memset(&sensors[i], 0, sizeof(sensor_window_t));
    }
  }
  /* Return existing slot */
  for(i = 0; i < MAX_SENSORS; i++) {
    if(sensors[i].count > 0 && sensors[i].id == id) {
      return &sensors[i];
    }
  }
  /* Allocate empty slot */
  for(i = 0; i < MAX_SENSORS; i++) {
    if(sensors[i].count == 0) {
      memset(&sensors[i], 0, sizeof(sensor_window_t));
      sensors[i].id = id;
      return &sensors[i];
    }
  }
  return NULL;
}

/* Compute fixed-interval slope over WINDOW_SIZE values */
static double
compute_slope_fixed(sensor_window_t *w)
{
  /* we only call when w->count >= WINDOW_SIZE */
  const double sum_i  = (WINDOW_SIZE - 1) * WINDOW_SIZE / 2.0;
  const double sum_i2 = (WINDOW_SIZE - 1) * WINDOW_SIZE * (2*WINDOW_SIZE - 1) / 6.0;
  double sum_v = 0, sum_iv = 0;
  /* Oldest value is at index w->idx */
  for(int k = 0; k < WINDOW_SIZE; k++) {
    int pos = (w->idx + k) % WINDOW_SIZE;
    double v = (double)w->values[pos];
    sum_v  += v;
    sum_iv += k * v;
  }
  double num = WINDOW_SIZE * sum_iv - sum_i * sum_v;
  double den = WINDOW_SIZE * sum_i2 - sum_i * sum_i;
  return (den != 0) ? (num / den) : 0.0;
}

/* Handle incoming packets */
static void
input_callback(const void *data, uint16_t len,
               const linkaddr_t *src, const linkaddr_t *dest)
{
  /* 1) Handle rank updates and log them */
  if(len == sizeof(uint16_t)) {
    uint16_t recv_rank;
    memcpy(&recv_rank, data, sizeof(recv_rank));
    if(recv_rank + 1 < my_rank) {
      my_rank = recv_rank + 1;
      linkaddr_copy(&parent, src);
      printf("TREE : Node %u: new parent -> %u (rank %u)\n",
             linkaddr_node_addr.u8[0], src->u8[0], my_rank);
    }
    return;
  }

  /* 2) Sensor data (type=2,id,value) */
  const uint8_t *buf = data;
  if(len >= 4 && buf[0] == 2) {
    uint8_t sid = buf[1];
    uint16_t val;
    memcpy(&val, buf + 2, sizeof(val));

    sensor_window_t *w = get_window(sid);
    if(w) {
      w->values[w->idx] = val;
      w->last_ts        = clock_seconds();
      if(w->count < WINDOW_SIZE) {
        w->count++;
      }
      w->idx = (w->idx + 1) % WINDOW_SIZE;

      /* Only compute and print slope once window is full */
      if(w->count >= WINDOW_SIZE) {
        double slope = compute_slope_fixed(w);
        printf("PROCESS : Node %u: slope=%.2f for sensor %u\n",
               linkaddr_node_addr.u8[0], slope, sid);
        if(slope > SLOPE_THRESHOLD) {
          uint8_t cmd[4];
          cmd[0] = 3;
          cmd[1] = sid;
          uint16_t code = 1;  // open valve command
          memcpy(&cmd[2], &code, sizeof(code));
          nullnet_buf = cmd;
          nullnet_len = sizeof(cmd);
          linkaddr_t dst = {{ sid }};
          NETSTACK_NETWORK.output(&dst);
          printf("PROCESS : Node %u: send OPEN_VALVE to %u\n",
                 linkaddr_node_addr.u8[0], sid);
        }
      }
    }
    /* Do NOT forward packet upstream to avoid duplicate handling */
  }
}

PROCESS_THREAD(computation_node_process, ev, data)
{
  PROCESS_BEGIN();

  nullnet_set_input_callback(input_callback);

  /* Initialize rank and log if root */
  my_rank = 0xFFFF;
  if(linkaddr_node_addr.u8[0] == BORDER_NODE_ID) {
    my_rank = 0;
    printf("TREE : Node %u: I am root (rank 0)\n",
           linkaddr_node_addr.u8[0]);
  }

  etimer_set(&hello_timer, random_rand() % HELLO_INTERVAL);
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&hello_timer));
    broadcast_rank();
    etimer_reset(&hello_timer);
  }

  PROCESS_END();
}
