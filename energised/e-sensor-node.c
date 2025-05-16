/* e-sensor-node.c */

#include "contiki.h"
#include "energest.h"
#include "net/netstack.h"
#include "net/nullnet/nullnet.h"
#include "lib/random.h"
#include "net/linkaddr.h"
#include "dev/leds.h"
#include <stdio.h>
#include <string.h>

/* Timing */
#define HELLO_INTERVAL    (CLOCK_SECOND * 15)
#define SENSOR_INTERVAL   (CLOCK_SECOND * 60)
#define VALVE_DURATION    (CLOCK_SECOND * 600)

#ifndef BORDER_NODE_ID
#define BORDER_NODE_ID    1
#endif

/* Battery model */
#define BATTERY_MAX         100.0f
#define LPM_THRESHOLD       30.0f
#define DEEP_LPM_THRESHOLD  10.0f
#define WAKE_THRESHOLD      90.0f
#define CPU_COST            0.2f
#define LPM_COST            0.02f
#define TX_COST             1.0f
#define RX_COST             1.0f
#define COST_HELLO          1.0f
#define COST_SENSOR_TX      3.0f
#define COST_VALVE_RX       1.0f
#define ENERGY_DIFF_THRESHOLD  30

static uint16_t my_rank;
static linkaddr_t parent;
static uint8_t  parent_energy = 0;

static struct etimer hello_timer, sensor_timer, valve_timer, energy_timer;
static bool sensor_timer_started = false, valve_open = false;

static float battery_level = BATTERY_MAX;
static enum { STATE_ACTIVE, STATE_LPM, STATE_DEEP_LPM } power_state = STATE_ACTIVE;
static uint32_t last_cpu, last_lpm, last_tx, last_rx;

PROCESS(sensor_node_process, "E-Sensor node");
AUTOSTART_PROCESSES(&sensor_node_process);

/*---------------------------------------------------------------------------*/
static void
update_battery(void)
{
  energest_flush();
  uint32_t d_cpu = energest_type_time(ENERGEST_TYPE_CPU)      - last_cpu;
  uint32_t d_lpm = energest_type_time(ENERGEST_TYPE_LPM)      - last_lpm;
  uint32_t d_tx  = energest_type_time(ENERGEST_TYPE_TRANSMIT) - last_tx;
  uint32_t d_rx  = energest_type_time(ENERGEST_TYPE_LISTEN)   - last_rx;
  last_cpu  = energest_type_time(ENERGEST_TYPE_CPU);
  last_lpm  = energest_type_time(ENERGEST_TYPE_LPM);
  last_tx   = energest_type_time(ENERGEST_TYPE_TRANSMIT);
  last_rx   = energest_type_time(ENERGEST_TYPE_LISTEN);
  float s_cpu = d_cpu / (float)CLOCK_SECOND;
  float s_lpm = d_lpm / (float)CLOCK_SECOND;
  float s_tx  = d_tx  / (float)CLOCK_SECOND;
  float s_rx  = d_rx  / (float)CLOCK_SECOND;
  battery_level -= s_cpu*CPU_COST + s_lpm*LPM_COST
                 + s_tx*TX_COST  + s_rx*RX_COST;
}

/*---------------------------------------------------------------------------*/
static void
broadcast_rank(void)
{
  uint8_t buf[5];
  buf[0] = 1;  /* HELLO type */
  buf[1] = (my_rank >> 8) & 0xFF;
  buf[2] =  my_rank       & 0xFF;
  buf[3] = (uint8_t)battery_level;
  buf[4] = power_state;
  nullnet_buf = buf;
  nullnet_len = sizeof(buf);
  battery_level -= COST_HELLO;
  NETSTACK_NETWORK.output(NULL);
  printf("TREE : Node %u: HELLO rank=%u bat=%u state=%u\n",
         linkaddr_node_addr.u8[0], my_rank, buf[3], buf[4]);
}

/*---------------------------------------------------------------------------*/
static void
input_callback(const void *data, uint16_t len,
               const linkaddr_t *src, const linkaddr_t *dest)
{
  const uint8_t *buf = data;

  /* OPEN-VALVE (type=3) */
  if(len == 4 && buf[0] == 3) {
    battery_level -= COST_VALVE_RX;
    leds_on(LEDS_RED);
    valve_open = true;
    etimer_set(&valve_timer, VALVE_DURATION);
    printf("PROCESS : Node %u: valve OPEN\n",
           linkaddr_node_addr.u8[0]);
    return;
  }

  /* HELLO (5 bytes, type=1) */
  if(len == 5 && buf[0] == 1) {
    uint16_t recv_rank = (buf[1] << 8) | buf[2];
    if(recv_rank != 0xFFFF) {
      uint16_t cand_rank   = recv_rank + 1;
      uint8_t  recv_energy = buf[3];

      /* energy-aware parent selection */
      if(cand_rank < my_rank ||
         (cand_rank == my_rank
          && !linkaddr_cmp(src, &parent)
          && recv_energy > parent_energy + ENERGY_DIFF_THRESHOLD))
      {
        my_rank = cand_rank;
        linkaddr_copy(&parent, src);
        parent_energy = recv_energy;
        printf("TREE : Node %u: new parent -> %u (rank=%u, bat=%u)\n",
               linkaddr_node_addr.u8[0], src->u8[0],
               my_rank, parent_energy);
        if(!sensor_timer_started) {
          etimer_set(&sensor_timer, SENSOR_INTERVAL);
          sensor_timer_started = true;
        }
      }
      else if(linkaddr_cmp(src, &parent)) {
        /* refresh energy from same parent */
        parent_energy = recv_energy;
      }
    }
    return;
  }
}

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(sensor_node_process, ev, data)
{
  PROCESS_BEGIN();

  nullnet_set_input_callback(input_callback);

  /* init energest */
  energest_init();
  energest_flush();
  last_cpu  = energest_type_time(ENERGEST_TYPE_CPU);
  last_lpm  = energest_type_time(ENERGEST_TYPE_LPM);
  last_tx   = energest_type_time(ENERGEST_TYPE_TRANSMIT);
  last_rx   = energest_type_time(ENERGEST_TYPE_LISTEN);
  etimer_set(&energy_timer, CLOCK_SECOND);

  /* start unjoined */
  my_rank = 0xFFFF;
  if(linkaddr_node_addr.u8[0] == BORDER_NODE_ID) {
    my_rank = 0;
    parent_energy = 0;
    printf("TREE : Node %u: I am root (rank 0)\n",
           linkaddr_node_addr.u8[0]);
  }
  etimer_set(&hello_timer, random_rand() % HELLO_INTERVAL);
  static uint8_t lpm_cnt = 0, deep_cnt = 0;

  while(1) {
    PROCESS_WAIT_EVENT();

    if(etimer_expired(&energy_timer)) {
      update_battery();

      if(power_state == STATE_LPM) {
        if(++lpm_cnt >= 10) {
          battery_level += 1.0f;
          if(battery_level > BATTERY_MAX) battery_level = BATTERY_MAX;
          lpm_cnt = 0;
        }
      } else if(power_state == STATE_DEEP_LPM) {
        if(++deep_cnt >= 2) {
          battery_level += 1.0f;
          if(battery_level > BATTERY_MAX) battery_level = BATTERY_MAX;
          deep_cnt = 0;
        }
      }
      
      if(power_state==STATE_ACTIVE  && battery_level<=LPM_THRESHOLD){
        printf("MODE : Node %u: LPM, battery=%.1f%%\n",
               linkaddr_node_addr.u8[0], battery_level);
        power_state=STATE_LPM;  
      }      
      if(power_state==STATE_LPM     && battery_level<=DEEP_LPM_THRESHOLD){
        printf("MODE : Node %u: DEEP LPM, battery=%.1f%%\n",
               linkaddr_node_addr.u8[0], battery_level);
        power_state=STATE_DEEP_LPM;
      }
      if(power_state==STATE_DEEP_LPM&& battery_level>=WAKE_THRESHOLD)     {
        printf("MODE : Node %u: WAKE, battery=%.1f%%\n",
               linkaddr_node_addr.u8[0], battery_level);
        power_state=STATE_ACTIVE;
      }
      etimer_reset(&energy_timer);
    }

    /* HELLO */
    if(etimer_expired(&hello_timer)) {
      broadcast_rank();
      etimer_reset(&hello_timer);
    }

    /* SENSOR reading */
    if(sensor_timer_started && etimer_expired(&sensor_timer)) {
      if(power_state != STATE_DEEP_LPM) {
        uint16_t reading = random_rand() % 100;
        uint8_t buf[4] = { 2, linkaddr_node_addr.u8[0] };
        memcpy(&buf[2], &reading, sizeof(reading));
        nullnet_buf = buf;
        nullnet_len = sizeof(buf);
        battery_level -= COST_SENSOR_TX;
        NETSTACK_NETWORK.output(&parent);
        printf("PROCESS : Node %u: send reading %u to %u\n",
               linkaddr_node_addr.u8[0], reading, parent.u8[0]);
      } else {
        /* Deep-LPM: skip sensor traffic, only HELLOs go out */
        printf("DLPM   : Node %u: in DEEP LPM, skipping sensor send\n",
               linkaddr_node_addr.u8[0]);
      }
      etimer_reset(&sensor_timer);
    
    }

    /* Valve timeout */
    if(valve_open && etimer_expired(&valve_timer)) {
      leds_off(LEDS_RED);
      valve_open = false;
      printf("PROCESS : Node %u: valve CLOSED\n",
             linkaddr_node_addr.u8[0]);
    }
  }

  PROCESS_END();
}
