#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <stdbool.h>
#include <mosquitto/libmosquitto.h>

bool dashboard_start(struct mosquitto *mosq, const char *base_topic);
void dashboard_stop(void);
void dashboard_set_mqtt_connected(bool connected);
void dashboard_record_message(const char *topic, const void *payload, int payload_length);

#endif
