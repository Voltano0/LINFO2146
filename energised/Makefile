CONTIKI_PROJECT = e-sensor-node e-border-router e-computation-node
all: $(CONTIKI_PROJECT)

CONTIKI = ../../..
# ---- Add these two lines to switch OFF IPv6/RPL ----
# Use the “no-IP” NullNet network layer
MAKE_NET     = MAKE_NET_NULLNET
# Use the “no-routing” null-routing driver
MAKE_ROUTING = MAKE_ROUTING_NULLROUTING
include $(CONTIKI)/Makefile.include


