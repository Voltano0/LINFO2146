/* e-border-router.c */

#include "contiki.h"
#include "energest.h"
#include "dev/serial-line.h"
#include "lib/random.h"
#include "net/netstack.h"
#include "net/nullnet/nullnet.h"
#include "net/linkaddr.h"
#include <stdio.h>
#include <string.h>

#define HELLO_INTERVAL    (CLOCK_SECOND * 10)

#ifndef BORDER_NODE_ID
#define BORDER_NODE_ID    1
#endif

/* Battery model */
#define BATTERY_MAX        100.0f
#define LPM_THRESHOLD      30.0f
#define DEEP_LPM_THRESHOLD 10.0f
#define WAKE_THRESHOLD     90.0f
#define CPU_COST           0.2f
#define LPM_COST           0.02f
#define TX_COST            1.0f
#define RX_COST            1.0f
#define COST_HELLO         1.0f
#define COST_FORWARD       1.0f

typedef struct {
  uint8_t type, node;
  uint16_t value;
} packet_t;

static uint16_t   my_rank;
static struct etimer hello_timer, energy_timer;
static float      battery_level = BATTERY_MAX;
static enum { STATE_ACTIVE, STATE_LPM, STATE_DEEP_LPM } power_state = STATE_ACTIVE;
static uint32_t   last_cpu, last_lpm, last_tx, last_rx;

PROCESS(border_router_process, "E-Border router");
AUTOSTART_PROCESSES(&border_router_process);

/* Update battery from energest deltas */
static void
update_battery(void)
{
  energest_flush();
  uint32_t d_cpu = energest_type_time(ENERGEST_TYPE_CPU)      - last_cpu;
  uint32_t d_lpm = energest_type_time(ENERGEST_TYPE_LPM)      - last_lpm;
  uint32_t d_tx  = energest_type_time(ENERGEST_TYPE_TRANSMIT) - last_tx;
  uint32_t d_rx  = energest_type_time(ENERGEST_TYPE_LISTEN)   - last_rx;
  last_cpu = energest_type_time(ENERGEST_TYPE_CPU);
  last_lpm = energest_type_time(ENERGEST_TYPE_LPM);
  last_tx  = energest_type_time(ENERGEST_TYPE_TRANSMIT);
  last_rx  = energest_type_time(ENERGEST_TYPE_LISTEN);
  float s_cpu = d_cpu / (float)CLOCK_SECOND;
  float s_lpm = d_lpm / (float)CLOCK_SECOND;
  float s_tx  = d_tx  / (float)CLOCK_SECOND;
  float s_rx  = d_rx  / (float)CLOCK_SECOND;
  battery_level -= s_cpu*CPU_COST + s_lpm*LPM_COST
                 + s_tx*TX_COST  + s_rx*RX_COST;
}

/* Broadcast 5-byte HELLO */
static void
broadcast_rank(void)
{
  uint8_t buf[5] = {
    1,
    (uint8_t)(my_rank>>8), (uint8_t)my_rank,
    (uint8_t)battery_level,
    (uint8_t)power_state
  };
  nullnet_buf = buf;
  nullnet_len = sizeof(buf);
  battery_level -= COST_HELLO;
  NETSTACK_NETWORK.output(NULL);
  printf("TREE : Node %u: HELLO rank=%u bat=%u state=%u\n",
         linkaddr_node_addr.u8[0], my_rank, buf[3], buf[4]);
}

/* Handle sensor readings only */
static void
input_callback(const void *data, uint16_t len,
               const linkaddr_t *src, const linkaddr_t *dest)
{
  if(len==4) {
    const uint8_t *buf = data;
    if(buf[0]==2) {
      packet_t pkt;
      memcpy(&pkt, data, sizeof(pkt));
      printf("PROCESS : Server got ID=%u, value=%u\n", pkt.node, pkt.value);
      battery_level -= COST_FORWARD;
    }
  }
}

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(border_router_process, ev, data)
{
  PROCESS_BEGIN();

  /* allow PC→mote commands */
  serial_line_init();
  nullnet_set_input_callback(input_callback);

  energest_init();
  energest_flush();
  last_cpu = energest_type_time(ENERGEST_TYPE_CPU);
  last_lpm = energest_type_time(ENERGEST_TYPE_LPM);
  last_tx  = energest_type_time(ENERGEST_TYPE_TRANSMIT);
  last_rx  = energest_type_time(ENERGEST_TYPE_LISTEN);
  etimer_set(&energy_timer, CLOCK_SECOND);

  my_rank = 0xFFFF;
  if(linkaddr_node_addr.u8[0]==BORDER_NODE_ID) {
    my_rank=0;
    printf("TREE : Node %u: I am root (rank 0)\n",
           linkaddr_node_addr.u8[0]);
  }
  etimer_set(&hello_timer, random_rand()%HELLO_INTERVAL);

  while(1) {
    PROCESS_WAIT_EVENT();

    /* PC commands → radio */
    if(ev == serial_line_event_message) {
      char *line = (char*)data;
      uint8_t t, n; uint16_t c;
      if(sscanf(line, "%hhu %hhu %hu", &t, &n, &c)==3) {
        uint8_t cmd[4] = {t,n};
        memcpy(cmd+2,&c,sizeof(c));
        nullnet_buf = cmd; nullnet_len = sizeof(cmd);
        linkaddr_t dst = {{n}};
        NETSTACK_NETWORK.output(&dst);
        battery_level -= COST_FORWARD;
        printf("BORDER: Sent cmd type=%u to %u\n", t, n);
      }
      continue;
    }

    /* Battery & modes */
    if(etimer_expired(&energy_timer)) {
      update_battery();
      static uint8_t lpmc, dpc;
      if(power_state==STATE_LPM      && ++lpmc>=10)    { battery_level+=1; lpmc=0; }
      if(power_state==STATE_DEEP_LPM && ++dpc>=2)      { battery_level+=1; dpc=0; }
      if(power_state==STATE_ACTIVE   && battery_level<=LPM_THRESHOLD)      power_state=STATE_LPM;
      if(power_state==STATE_LPM      && battery_level<=DEEP_LPM_THRESHOLD) power_state=STATE_DEEP_LPM;
      if(power_state==STATE_DEEP_LPM && battery_level>=WAKE_THRESHOLD)     power_state=STATE_ACTIVE;
      etimer_reset(&energy_timer);
    }

    /* HELLO */
    if(etimer_expired(&hello_timer)) {
      broadcast_rank();
      etimer_reset(&hello_timer);
    }
  }

  PROCESS_END();
}
