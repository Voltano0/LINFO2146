/* sensor-node.c */

// BORDER_NODE_ID can be overridden at compile time (e.g., make BORDER_NODE_ID=5)
#ifndef BORDER_NODE_ID
#define BORDER_NODE_ID 1
#endif

#include "contiki.h"
#include "net/netstack.h"
#include "net/nullnet/nullnet.h"
#include "lib/random.h"
#include "net/linkaddr.h"
#include "dev/leds.h"
#include <stdio.h>
#include <string.h>
#define HELLO_INTERVAL   (CLOCK_SECOND * 15)
#define SENSOR_INTERVAL  (CLOCK_SECOND * 60)
#define VALVE_DURATION   (CLOCK_SECOND * 600)  /* 10 minutes */

static uint16_t my_rank;
static linkaddr_t parent;
static struct etimer hello_timer, sensor_timer, valve_timer;
static bool sensor_timer_started = false;
static bool valve_open = false;

PROCESS(sensor_node_process, "Sensor node process");
AUTOSTART_PROCESSES(&sensor_node_process);

static void broadcast_rank(void) {
  nullnet_buf = (uint8_t *)&my_rank;
  nullnet_len = sizeof(my_rank);
  NETSTACK_NETWORK.output(NULL);
  printf("TREE : HELLO Node %u: broadcast rank %u\n", linkaddr_node_addr.u8[0], my_rank);
}

typedef struct {
  uint8_t type;
  uint8_t node;
  uint16_t value;
} packet_t;

static void input_callback(const void *data, uint16_t len,
                           const linkaddr_t *src, const linkaddr_t *dest) {
  if(len == sizeof(uint16_t)) {
    uint16_t recv_rank;
    memcpy(&recv_rank, data, sizeof(recv_rank));
    if(recv_rank + 1 < my_rank) {
      my_rank = recv_rank + 1;
      linkaddr_copy(&parent, src);
      printf("TREE : Node %u: new parent -> %u (rank %u)\n",
             linkaddr_node_addr.u8[0], src->u8[0], my_rank);
    }
  } else if(len == sizeof(packet_t)) {
    const packet_t *pkt = (const packet_t *)data;
    if(pkt->type == 3 && pkt->value == 1) {
      leds_on(LEDS_RED);
      valve_open = true;
      etimer_set(&valve_timer, VALVE_DURATION);
      printf("PROCESS : Node %u: valve OPEN\n", linkaddr_node_addr.u8[0]);
    }
  }
}

PROCESS_THREAD(sensor_node_process, ev, data) {
  PROCESS_BEGIN();
  nullnet_set_input_callback(input_callback);
  my_rank = 0xFFFF;
  /* Always root if this is the border node */
  if(linkaddr_node_addr.u8[0] == BORDER_NODE_ID) {
    my_rank = 0;
    printf("TREE : Node %u: I am root (rank 0)\n", linkaddr_node_addr.u8[0]);
  }
  etimer_set(&hello_timer, random_rand() % HELLO_INTERVAL);

  while(1) {
    PROCESS_WAIT_EVENT();
    if(etimer_expired(&hello_timer)) {
      broadcast_rank();
      etimer_reset(&hello_timer);
      /* Start sensor readings once tree is formed */
      if(!sensor_timer_started && my_rank != 0xFFFF) {
        etimer_set(&sensor_timer, SENSOR_INTERVAL);
        sensor_timer_started = true;
      }
    }
    if(sensor_timer_started && etimer_expired(&sensor_timer)) {
      uint16_t reading = random_rand() % 100;
      packet_t pkt = {2, linkaddr_node_addr.u8[0], reading};
      nullnet_buf = (uint8_t *)&pkt;
      nullnet_len = sizeof(pkt);
      NETSTACK_NETWORK.output(&parent);
      printf("PROCESS : Node %u: send reading %u to %u\n",
             linkaddr_node_addr.u8[0], reading, parent.u8[0]);
      etimer_reset(&sensor_timer);
    }
    if(valve_open && etimer_expired(&valve_timer)) {
      leds_off(LEDS_RED);
      valve_open = false;
      printf("PROCESS : Node %u: valve CLOSED\n", linkaddr_node_addr.u8[0]);
    }
  }
  PROCESS_END();
}