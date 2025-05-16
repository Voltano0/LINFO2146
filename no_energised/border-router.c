/* border-router.c */
#include "contiki.h"
#include "dev/serial-line.h"
#include "net/netstack.h"
#include "net/nullnet/nullnet.h"
#include "lib/random.h"
#include "net/linkaddr.h"
#include <stdio.h>
#include <string.h>

#define HELLO_INTERVAL   (CLOCK_SECOND * 10)

#ifndef BORDER_NODE_ID
#define BORDER_NODE_ID 1
#endif

typedef struct {
  uint8_t type;
  uint8_t node;
  uint16_t value;
} packet_t;

static uint16_t      my_rank;
static linkaddr_t    parent;
static struct etimer hello_timer;

PROCESS(border_router_process, "Border Router Process");
AUTOSTART_PROCESSES(&border_router_process);

/* Broadcast our rank in the tree */
static void
broadcast_rank(void)
{
  nullnet_buf = (uint8_t *)&my_rank;
  nullnet_len = sizeof(my_rank);
  NETSTACK_NETWORK.output(NULL);
  printf("TREE : Node %u: broadcast rank %u\n",
         linkaddr_node_addr.u8[0], my_rank);
}

/* Handle incoming NullNet packets */
static void
input_callback(const void *data, uint16_t len,
               const linkaddr_t *src, const linkaddr_t *dest)
{
  /* 1) Tree-ranking messages */
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

  /* 2) Sensor data packets */
  if(len == sizeof(packet_t)) {
    const packet_t *pkt = (const packet_t *)data;
    if(pkt->type == 2) {
      /* Print to server console via serial */
      printf("PROCESS : Server got ID=%u, value=%u\n", pkt->node, pkt->value);
    }
  }
}

PROCESS_THREAD(border_router_process, ev, data)
{
  PROCESS_BEGIN();

  /* Initialize serial-line for PC commands */
  serial_line_init();

  /* Initialize NullNet input callback */
  nullnet_set_input_callback(input_callback);

  /* Set initial rank */
  my_rank = 0xFFFF;
  if(linkaddr_node_addr.u8[0] == BORDER_NODE_ID) {
    my_rank = 0;
    printf("TREE : Node %u: I am root (rank 0)\n",
           linkaddr_node_addr.u8[0]);
  }

  /* Start periodic rank broadcasts */
  etimer_set(&hello_timer, random_rand() % HELLO_INTERVAL);

  while(1) {
    PROCESS_WAIT_EVENT();

    /* 1) PC command via serial-line: "<type> <node> <code>" */
    if(ev == serial_line_event_message) {
      char *line = (char *)data;
      unsigned int type, node, code;
      if(sscanf(line, "%u %u %u", &type, &node, &code) == 3) {
        uint8_t cmd[4];
        cmd[0] = (uint8_t)type;
        cmd[1] = (uint8_t)node;
        memcpy(&cmd[2], &code, sizeof(uint16_t));
        nullnet_buf = cmd;
        nullnet_len = sizeof(cmd);
        linkaddr_t dst = {{ (uint8_t)node }};
        NETSTACK_NETWORK.output(&dst);
        printf("BORDER: Sent cmd type=%u to %u (code=%u)\n",
               type, node, code);
      }
      continue;
    }

    /* 2) Periodic tree rank broadcast */
    if(etimer_expired(&hello_timer)) {
      broadcast_rank();
      etimer_reset(&hello_timer);
    }
  }

  PROCESS_END();
}
