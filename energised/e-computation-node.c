/* e-computation-node.c */

#include "contiki.h"
#include "energest.h"
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

#ifndef BORDER_NODE_ID
#define BORDER_NODE_ID     1
#endif

/* Battery model */
#define BATTERY_MAX          100.0f
#define LPM_THRESHOLD        30.0f
#define DEEP_LPM_THRESHOLD   10.0f
#define WAKE_THRESHOLD       90.0f
#define CPU_COST             0.2f
#define LPM_COST             0.02f
#define TX_COST              1.0f
#define RX_COST              1.0f
#define COST_HELLO           1.0f
#define COST_SENSOR_TX       3.0f
#define COST_COMMAND_TX      2.0f
#define ENERGY_DIFF_THRESHOLD 30 

typedef struct {
  uint8_t  id, count, idx;
  uint16_t values[WINDOW_SIZE];
} sensor_window_t;

static uint16_t        my_rank;
static linkaddr_t      parent;
static uint8_t         parent_energy;
static struct etimer   hello_timer, energy_timer;
static sensor_window_t sensors[MAX_SENSORS];

static float           battery_level = BATTERY_MAX;
static enum { STATE_ACTIVE, STATE_LPM, STATE_DEEP_LPM } power_state = STATE_ACTIVE;
static uint32_t        last_cpu, last_lpm, last_tx, last_rx;

PROCESS(computation_node_process, "E-Computation node");
AUTOSTART_PROCESSES(&computation_node_process);

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

static sensor_window_t *
get_window(uint8_t id)
{
  for(int i=0;i<MAX_SENSORS;i++){
    if(sensors[i].count>0 && sensors[i].id==id) return &sensors[i];
  }
  for(int i=0;i<MAX_SENSORS;i++){
    if(sensors[i].count==0){
      memset(&sensors[i],0,sizeof(sensors[i]));
      sensors[i].id = id;
      return &sensors[i];
    }
  }
  return NULL;
}

static double
compute_slope(sensor_window_t *w)
{
  if(w->count < WINDOW_SIZE) return 0.0;
  const double sum_i  = (WINDOW_SIZE-1)*WINDOW_SIZE/2.0;
  const double sum_i2 = (WINDOW_SIZE-1)*WINDOW_SIZE*(2*WINDOW_SIZE-1)/6.0;
  double sv=0, siv=0;
  for(int i=0;i<WINDOW_SIZE;i++){
    sv  += w->values[i];
    siv += i*w->values[i];
  }
  double num = WINDOW_SIZE*siv - sum_i*sv;
  double den = WINDOW_SIZE*sum_i2 - sum_i*sum_i;
  return den ? num/den : 0.0;
}

static void
input_callback(const void *data, uint16_t len,
               const linkaddr_t *src, const linkaddr_t *dest)
{
  const uint8_t *buf = data;

  /* HELLO (5 bytes, buf[0]==1) */
  if(len==5 && buf[0]==1) {
    uint16_t recv = (buf[1]<<8)|buf[2];
    if(recv!=0xFFFF) {
      uint16_t cand = recv+1;
      uint8_t  energy = buf[3];
      if(cand < my_rank ||
         (cand==my_rank && !linkaddr_cmp(src,&parent) && energy>parent_energy + ENERGY_DIFF_THRESHOLD))
      {
        my_rank = cand;
        linkaddr_copy(&parent, src);
        parent_energy = energy;
        printf("TREE : Node %u: new parent -> %u (rank=%u, bat=%u)\n",
               linkaddr_node_addr.u8[0], src->u8[0], my_rank, energy);
      } else if(linkaddr_cmp(src,&parent)) {
        parent_energy = energy;
      }
    }
    return;
  }

  /* SENSOR reading (4 bytes, buf[0]==2) */
  if(len==4 && buf[0]==2) {
    uint8_t sid = buf[1];
    uint16_t v; memcpy(&v, buf+2, sizeof(v));
    if(power_state != STATE_DEEP_LPM){
      sensor_window_t *w = get_window(sid);
      if(w){
        w->values[w->idx] = v;
        if(w->count<WINDOW_SIZE) w->count++;
        w->idx = (w->idx+1)%WINDOW_SIZE;
        double slope = compute_slope(w);
        printf("PROCESS : Node %u: slope=%.2f sensor=%u\n",
               linkaddr_node_addr.u8[0], slope, sid);
        if(slope > SLOPE_THRESHOLD){
          uint8_t cmd[4] = {3, sid};
          uint16_t c = 1; memcpy(cmd+2,&c,sizeof(c));
          nullnet_buf = cmd; nullnet_len = 4;
          linkaddr_t dst = {{sid}};
          NETSTACK_NETWORK.output(&dst);
          battery_level -= COST_COMMAND_TX;
          printf("PROCESS : Node %u: OPEN_VALVE → %u\n",
                 linkaddr_node_addr.u8[0], sid);
        }
      }
    } else{
    /* forward */
    nullnet_buf = (uint8_t*)data; nullnet_len = len;
    NETSTACK_NETWORK.output(&parent);
    battery_level -= COST_SENSOR_TX;
    printf("PROCESS : Node %u: forward sensor %u to %u\n",
           linkaddr_node_addr.u8[0], sid, parent.u8[0]);
    }
    return;
  }
}

PROCESS_THREAD(computation_node_process, ev, data)
{
  PROCESS_BEGIN();

  nullnet_set_input_callback(input_callback);

  energest_init();
  energest_flush();
  last_cpu = energest_type_time(ENERGEST_TYPE_CPU);
  last_lpm = energest_type_time(ENERGEST_TYPE_LPM);
  last_tx  = energest_type_time(ENERGEST_TYPE_TRANSMIT);
  last_rx  = energest_type_time(ENERGEST_TYPE_LISTEN);
  etimer_set(&energy_timer, CLOCK_SECOND);

  my_rank = 0xFFFF;
  if(linkaddr_node_addr.u8[0]==BORDER_NODE_ID){
    my_rank=0; parent_energy=0;
    printf("TREE : Node %u: I am root\n", linkaddr_node_addr.u8[0]);
  }
  etimer_set(&hello_timer, random_rand()%HELLO_INTERVAL);
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

    if(etimer_expired(&hello_timer)) {
      broadcast_rank();
      etimer_reset(&hello_timer);
    }
  }

  PROCESS_END();
}



