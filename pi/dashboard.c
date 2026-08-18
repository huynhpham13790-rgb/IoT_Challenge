#define _CRT_SECURE_NO_WARNINGS
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "dashboard.h"

#include <errno.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET socket_handle;
#define CLOSE_SOCKET closesocket
#define THREAD_RESULT DWORD WINAPI
#define THREAD_RETURN 0
#define STRNICMP _strnicmp
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <strings.h>
typedef int socket_handle;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define CLOSE_SOCKET close
#define THREAD_RESULT void *
#define THREAD_RETURN NULL
#define STRNICMP strncasecmp
#endif

#define MAX_DEVICES 48
#define MAX_NAME 128
#define MAX_STATE 32
#define RESPONSE_BUFFER 65536
#define MAX_HTTP_HEADER 32768
#define MAX_UPLOAD_SIZE (4U * 1024U * 1024U)

typedef struct {
    bool used;
    char name[MAX_NAME];
    char ieee_address[32];
    char model[96];
    char device_type[32];
    bool supports_ota;
    char last_seen[24];
    double temperature;
    double humidity;
    double battery;
    double linkquality;
    double update_progress;
    double update_remaining;
    double installed_version;
    double latest_version;
    bool has_temperature;
    bool has_humidity;
    bool has_battery;
    bool has_linkquality;
    bool has_update_progress;
    bool has_update_remaining;
    bool has_installed_version;
    bool has_latest_version;
    char update_state[MAX_STATE];
} device_snapshot;

static device_snapshot devices[MAX_DEVICES];
static unsigned long long message_count = 0;
static bool mqtt_connected = false;
static bool zigbee_online = false;
static volatile bool web_running = false;
static struct mosquitto *mqtt_client = NULL;
static char mqtt_base_topic[128] = "zigbee2mqtt";
static int web_port = 8090;
static bool dashboard_initialized = false;

#ifdef _WIN32
static CRITICAL_SECTION state_lock;
static HANDLE web_thread_handle = NULL;
#define LOCK_STATE() EnterCriticalSection(&state_lock)
#define UNLOCK_STATE() LeaveCriticalSection(&state_lock)
#else
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t web_thread_handle;
static bool web_thread_created = false;
#define LOCK_STATE() pthread_mutex_lock(&state_lock)
#define UNLOCK_STATE() pthread_mutex_unlock(&state_lock)
#endif

static void current_time_text(char *output, size_t output_size)
{
    time_t now = time(NULL);
    struct tm local;
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    strftime(output, output_size, "%H:%M:%S", &local);
}

static bool json_number(const char *json, const char *field, double *value)
{
    char key[64];
    const char *position;
    char *end;
    if (json == NULL || field == NULL || value == NULL) return false;
    if (snprintf(key, sizeof(key), "\"%s\"", field) < 0) return false;
    position = strstr(json, key);
    if (position == NULL || (position = strchr(position + strlen(key), ':')) == NULL) return false;
    errno = 0;
    *value = strtod(position + 1, &end);
    return end != position + 1 && errno != ERANGE;
}

static bool json_string_from(const char *json, const char *field, char *output, size_t output_size)
{
    char key[64];
    const char *start;
    const char *end;
    size_t length;
    if (json == NULL || field == NULL || output == NULL || output_size == 0) return false;
    if (snprintf(key, sizeof(key), "\"%s\"", field) < 0) return false;
    start = strstr(json, key);
    if (start == NULL || (start = strchr(start + strlen(key), ':')) == NULL) return false;
    start++;
    while (*start == ' ' || *start == '\t') start++;
    if (*start != '\"') return false;
    start++;
    end = strchr(start, '\"');
    if (end == NULL) return false;
    length = (size_t)(end - start);
    if (length >= output_size) length = output_size - 1;
    memcpy(output, start, length);
    output[length] = '\0';
    return true;
}

static bool json_string_from_last(const char *json, const char *field, char *output, size_t output_size)
{
    char key[64];
    const char *position;
    const char *last = NULL;
    if (json == NULL || field == NULL) return false;
    if (snprintf(key, sizeof(key), "\"%s\"", field) < 0) return false;
    position = json;
    while ((position = strstr(position, key)) != NULL) {
        last = position;
        position += strlen(key);
    }
    return last != NULL && json_string_from(last, field, output, output_size);
}

static device_snapshot *get_device(const char *name)
{
    int index;
    device_snapshot *empty = NULL;
    for (index = 0; index < MAX_DEVICES; index++) {
        if (devices[index].used && strcmp(devices[index].name, name) == 0) return &devices[index];
        if (!devices[index].used && empty == NULL) empty = &devices[index];
    }
    if (empty != NULL) {
        memset(empty, 0, sizeof(*empty));
        empty->used = true;
        snprintf(empty->name, sizeof(empty->name), "%s", name);
        snprintf(empty->update_state, sizeof(empty->update_state), "idle");
    }
    return empty;
}

static void parse_bridge_devices(const char *json)
{
    const char *cursor = json;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    const char *object_start = NULL;
    while (cursor != NULL && *cursor != '\0') {
        char ch = *cursor;
        if (in_string) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') in_string = false;
        } else if (ch == '"') {
            in_string = true;
        } else if (ch == '{') {
            if (depth == 0) object_start = cursor;
            depth++;
        } else if (ch == '}' && depth > 0) {
            depth--;
            if (depth == 0 && object_start != NULL) {
                size_t length = (size_t)(cursor - object_start + 1);
                char *object = (char *)malloc(length + 1U);
                if (object != NULL) {
                    char friendly[MAX_NAME] = "";
                    memcpy(object, object_start, length);
                    object[length] = '\0';
                    if (strstr(object, "\"type\":\"Coordinator\"") == NULL
                        && json_string_from(object, "friendly_name", friendly, sizeof(friendly))
                        && friendly[0] != '\0') {
                        device_snapshot *device = get_device(friendly);
                        if (device != NULL) {
                            json_string_from_last(object, "ieee_address", device->ieee_address, sizeof(device->ieee_address));
                            if (!json_string_from(object, "model", device->model, sizeof(device->model))) {
                                json_string_from(object, "model_id", device->model, sizeof(device->model));
                            }
                            if (strstr(object, "\"type\":\"Router\"") != NULL) snprintf(device->device_type, sizeof(device->device_type), "Router");
                            else snprintf(device->device_type, sizeof(device->device_type), "EndDevice");
                            device->supports_ota = strstr(object, "\"supports_ota\":true") != NULL;
                        }
                    }
                    free(object);
                }
                object_start = NULL;
            }
        }
        cursor++;
    }
}

void dashboard_set_mqtt_connected(bool connected)
{
    LOCK_STATE();
    mqtt_connected = connected;
    if (!connected) zigbee_online = false;
    UNLOCK_STATE();
}

void dashboard_record_message(const char *topic, const void *payload, int payload_length)
{
    char *json;
    char prefix[160];
    const char *tail;
    device_snapshot *device;
    const char *update;
    if (topic == NULL || payload == NULL || payload_length <= 0) return;
    json = (char *)malloc((size_t)payload_length + 1U);
    if (json == NULL) return;
    memcpy(json, payload, (size_t)payload_length);
    json[payload_length] = '\0';

    snprintf(prefix, sizeof(prefix), "%s/", mqtt_base_topic);
    if (strncmp(topic, prefix, strlen(prefix)) != 0) {
        free(json);
        return;
    }
    tail = topic + strlen(prefix);

    LOCK_STATE();
    message_count++;
    if (strcmp(tail, "bridge/state") == 0) {
        zigbee_online = strstr(json, "\"online\"") != NULL;
        UNLOCK_STATE();
        free(json);
        return;
    }
    if (strcmp(tail, "bridge/devices") == 0) {
        parse_bridge_devices(json);
        UNLOCK_STATE();
        free(json);
        return;
    }
    if (strchr(tail, '/') != NULL || strcmp(tail, "bridge") == 0) {
        UNLOCK_STATE();
        free(json);
        return;
    }
    device = get_device(tail);
    if (device != NULL) {
        device->has_temperature = json_number(json, "temperature", &device->temperature) || device->has_temperature;
        device->has_humidity = json_number(json, "humidity", &device->humidity) || device->has_humidity;
        device->has_battery = json_number(json, "battery", &device->battery) || device->has_battery;
        device->has_linkquality = json_number(json, "linkquality", &device->linkquality) || device->has_linkquality;
        current_time_text(device->last_seen, sizeof(device->last_seen));
        update = strstr(json, "\"update\"");
        if (update != NULL) {
            json_string_from(update, "state", device->update_state, sizeof(device->update_state));
            device->has_update_progress = json_number(update, "progress", &device->update_progress);
            device->has_update_remaining = json_number(update, "remaining", &device->update_remaining);
            device->has_installed_version = json_number(update, "installed_version", &device->installed_version)
                || device->has_installed_version;
            device->has_latest_version = json_number(update, "latest_version", &device->latest_version)
                || device->has_latest_version;
        }
    }
    UNLOCK_STATE();
    free(json);
}

static size_t append_text(char *buffer, size_t size, size_t offset, const char *text)
{
    size_t length = strlen(text);
    if (offset + length >= size) length = size > offset + 1 ? size - offset - 1 : 0;
    if (length > 0) memcpy(buffer + offset, text, length);
    offset += length;
    if (offset < size) buffer[offset] = '\0';
    return offset;
}

static size_t append_json_string(char *buffer, size_t size, size_t offset, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    offset = append_text(buffer, size, offset, "\"");
    while (*cursor != '\0' && offset + 7 < size) {
        char escaped[8];
        if (*cursor == '\"' || *cursor == '\\') {
            escaped[0] = '\\'; escaped[1] = (char)*cursor; escaped[2] = '\0';
        } else if (*cursor < 0x20) {
            snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
        } else {
            escaped[0] = (char)*cursor; escaped[1] = '\0';
        }
        offset = append_text(buffer, size, offset, escaped);
        cursor++;
    }
    return append_text(buffer, size, offset, "\"");
}

static void append_optional_number(char *buffer, size_t size, size_t *offset, const char *key, bool has_value, double value)
{
    char number[96];
    if (has_value) snprintf(number, sizeof(number), ",\"%s\":%.2f", key, value);
    else snprintf(number, sizeof(number), ",\"%s\":null", key);
    *offset = append_text(buffer, size, *offset, number);
}

static char *create_status_json(void)
{
    char *json = (char *)calloc(RESPONSE_BUFFER, 1);
    char item[128];
    size_t offset = 0;
    int index;
    bool first = true;
    if (json == NULL) return NULL;
    LOCK_STATE();
    snprintf(item, sizeof(item), "{\"mqtt_connected\":%s,\"zigbee_online\":%s,\"message_count\":%llu,\"devices\":[",
        mqtt_connected ? "true" : "false", zigbee_online ? "true" : "false", message_count);
    offset = append_text(json, RESPONSE_BUFFER, offset, item);
    for (index = 0; index < MAX_DEVICES; index++) {
        device_snapshot *device = &devices[index];
        if (!device->used) continue;
        if (!first) offset = append_text(json, RESPONSE_BUFFER, offset, ",");
        first = false;
        offset = append_text(json, RESPONSE_BUFFER, offset, "{\"name\":");
        offset = append_json_string(json, RESPONSE_BUFFER, offset, device->name);
        offset = append_text(json, RESPONSE_BUFFER, offset, ",\"ieee_address\":");
        offset = append_json_string(json, RESPONSE_BUFFER, offset, device->ieee_address);
        offset = append_text(json, RESPONSE_BUFFER, offset, ",\"model\":");
        offset = append_json_string(json, RESPONSE_BUFFER, offset, device->model);
        offset = append_text(json, RESPONSE_BUFFER, offset, ",\"type\":");
        offset = append_json_string(json, RESPONSE_BUFFER, offset, device->device_type);
        offset = append_text(json, RESPONSE_BUFFER, offset, device->supports_ota ? ",\"supports_ota\":true" : ",\"supports_ota\":false");
        append_optional_number(json, RESPONSE_BUFFER, &offset, "temperature", device->has_temperature, device->temperature);
        append_optional_number(json, RESPONSE_BUFFER, &offset, "humidity", device->has_humidity, device->humidity);
        append_optional_number(json, RESPONSE_BUFFER, &offset, "battery", device->has_battery, device->battery);
        append_optional_number(json, RESPONSE_BUFFER, &offset, "linkquality", device->has_linkquality, device->linkquality);
        offset = append_text(json, RESPONSE_BUFFER, offset, ",\"update_state\":");
        offset = append_json_string(json, RESPONSE_BUFFER, offset, device->update_state);
        append_optional_number(json, RESPONSE_BUFFER, &offset, "update_progress", device->has_update_progress, device->update_progress);
        append_optional_number(json, RESPONSE_BUFFER, &offset, "update_remaining", device->has_update_remaining, device->update_remaining);
        append_optional_number(json, RESPONSE_BUFFER, &offset, "installed_version", device->has_installed_version, device->installed_version);
        append_optional_number(json, RESPONSE_BUFFER, &offset, "latest_version", device->has_latest_version, device->latest_version);
        offset = append_text(json, RESPONSE_BUFFER, offset, ",\"last_seen\":");
        offset = append_json_string(json, RESPONSE_BUFFER, offset, device->last_seen);
        offset = append_text(json, RESPONSE_BUFFER, offset, "}");
    }
    append_text(json, RESPONSE_BUFFER, offset, "]}");
    UNLOCK_STATE();
    return json;
}

static int send_all(socket_handle client, const char *data, size_t size)
{
    size_t sent = 0;
    while (sent < size) {
        int result = send(client, data + sent, (int)(size - sent), 0);
        if (result <= 0) return -1;
        sent += (size_t)result;
    }
    return 0;
}

static void send_response(socket_handle client, int status, const char *type, const char *body, size_t body_size)
{
    char header[512];
    const char *label = status == 200 ? "OK" : status == 202 ? "Accepted" : status == 400 ? "Bad Request"
        : status == 413 ? "Payload Too Large" : status == 500 ? "Internal Server Error" : "Not Found";
    int length = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n",
        status, label, type, body_size);
    if (length > 0) send_all(client, header, (size_t)length);
    if (body != NULL && body_size > 0) send_all(client, body, body_size);
}

static void serve_file(socket_handle client, const char *name, const char *type)
{
    const char *root = getenv("WEB_ROOT");
    char path[512];
    FILE *file;
    long length;
    char *content;
    if (root == NULL || root[0] == '\0') root = "web";
    snprintf(path, sizeof(path), "%s/%s", root, name);
    file = fopen(path, "rb");
    if (file == NULL) {
        const char *message = "Dashboard asset not found";
        send_response(client, 404, "text/plain; charset=utf-8", message, strlen(message));
        return;
    }
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    rewind(file);
    if (length < 0 || (content = (char *)malloc((size_t)length)) == NULL) {
        fclose(file);
        send_response(client, 404, "text/plain", "Read error", 10);
        return;
    }
    if (length > 0) fread(content, 1, (size_t)length, file);
    fclose(file);
    send_response(client, 200, type, content, (size_t)length);
    free(content);
}

static bool body_string(const char *body, const char *field, char *output, size_t output_size)
{
    return json_string_from(body, field, output, output_size);
}

static uint16_t read_le16(const unsigned char *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const unsigned char *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static bool write_binary_file(const char *path, const unsigned char *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    bool ok;
    if (file == NULL) return false;
    ok = fwrite(data, 1, size, file) == size;
    if (fclose(file) != 0) ok = false;
    return ok;
}

static void handle_firmware_upload(socket_handle client, const unsigned char *body, size_t body_size)
{
    const char *upload_directory = getenv("OTA_UPLOAD_DIR");
    const char *index_path = getenv("OTA_INDEX_PATH");
    uint16_t header_length;
    uint16_t manufacturer_code;
    uint16_t image_type;
    uint32_t file_version;
    uint32_t total_size;
    char filename[96];
    char full_path[768];
    char index_json[512];
    char response[512];
    FILE *index_file;
    int index_length;

    if (body == NULL || body_size < 56U || read_le32(body) != 0x0BEEF11EU) {
        const char *error = "{\"error\":\"File không phải Zigbee OTA hợp lệ (.ota)\"}";
        send_response(client, 400, "application/json; charset=utf-8", error, strlen(error));
        return;
    }
    header_length = read_le16(body + 6);
    manufacturer_code = read_le16(body + 10);
    image_type = read_le16(body + 12);
    file_version = read_le32(body + 14);
    total_size = read_le32(body + 52);
    if (header_length < 56U || header_length > body_size || total_size != body_size) {
        const char *error = "{\"error\":\"Header hoặc kích thước file Zigbee OTA không hợp lệ\"}";
        send_response(client, 400, "application/json; charset=utf-8", error, strlen(error));
        return;
    }
    if (upload_directory == NULL || upload_directory[0] == '\0' || index_path == NULL || index_path[0] == '\0') {
        const char *error = "{\"error\":\"Gateway chưa cấu hình thư mục OTA\"}";
        send_response(client, 500, "application/json; charset=utf-8", error, strlen(error));
        return;
    }

    snprintf(filename, sizeof(filename), "xg24_uploaded_v%u.ota", (unsigned int)file_version);
#ifdef _WIN32
    _mkdir(upload_directory);
    snprintf(full_path, sizeof(full_path), "%s\\%s", upload_directory, filename);
#else
    mkdir(upload_directory, 0755);
    snprintf(full_path, sizeof(full_path), "%s/%s", upload_directory, filename);
#endif
    if (!write_binary_file(full_path, body, body_size)) {
        const char *error = "{\"error\":\"Không ghi được file firmware vào thư mục Zigbee2MQTT\"}";
        send_response(client, 500, "application/json; charset=utf-8", error, strlen(error));
        return;
    }

    index_length = snprintf(index_json, sizeof(index_json),
        "[\n  {\n    \"url\": \"ota/%s\",\n    \"manufacturerCode\": %u,\n    \"imageType\": %u,\n    \"fileVersion\": %u\n  }\n]\n",
        filename, (unsigned int)manufacturer_code, (unsigned int)image_type, (unsigned int)file_version);
    index_file = fopen(index_path, "wb");
    if (index_file == NULL) {
        remove(full_path);
        {
            const char *error = "{\"error\":\"Không cập nhật được OTA index của Zigbee2MQTT\"}";
            send_response(client, 500, "application/json; charset=utf-8", error, strlen(error));
        }
        return;
    }
    {
        bool index_ok = fwrite(index_json, 1, (size_t)index_length, index_file) == (size_t)index_length;
        if (fclose(index_file) != 0) index_ok = false;
        if (!index_ok) {
        remove(full_path);
            const char *error = "{\"error\":\"Không cập nhật được OTA index của Zigbee2MQTT\"}";
            send_response(client, 500, "application/json; charset=utf-8", error, strlen(error));
            return;
        }
    }

    snprintf(response, sizeof(response),
        "{\"ok\":true,\"message\":\"Đã tải firmware lên gateway\",\"file\":\"%s\",\"size\":%u,\"manufacturer_code\":%u,\"image_type\":%u,\"file_version\":%u}",
        filename, (unsigned int)body_size, (unsigned int)manufacturer_code, (unsigned int)image_type, (unsigned int)file_version);
    send_response(client, 200, "application/json; charset=utf-8", response, strlen(response));
}

static bool parse_content_length(const char *headers, size_t *content_length)
{
    const char *line = headers;
    *content_length = 0;
    while (line != NULL && *line != '\0') {
        const char *end = strstr(line, "\r\n");
        size_t length = end == NULL ? strlen(line) : (size_t)(end - line);
        if (length >= 15U && STRNICMP(line, "Content-Length:", 15) == 0) {
            char *number_end;
            unsigned long value;
            const char *value_start = line + 15;
            while (*value_start == ' ' || *value_start == '\t') value_start++;
            errno = 0;
            value = strtoul(value_start, &number_end, 10);
            if (errno != 0 || number_end == value_start || value > MAX_UPLOAD_SIZE) return false;
            *content_length = (size_t)value;
            return true;
        }
        if (end == NULL || length == 0U) break;
        line = end + 2;
    }
    return true;
}

static void handle_ota(socket_handle client, const char *body)
{
    char action[24];
    char device[MAX_NAME];
    char topic[256];
    char payload[200];
    const char *suffix;
    const char *message;
    int rc;
    if (!body_string(body, "action", action, sizeof(action)) || !body_string(body, "device", device, sizeof(device))) {
        const char *error = "{\"error\":\"Thiếu action hoặc device\"}";
        send_response(client, 400, "application/json; charset=utf-8", error, strlen(error));
        return;
    }
    if (strchr(device, '\"') != NULL || strchr(device, '\\') != NULL) {
        const char *error = "{\"error\":\"Tên thiết bị không hợp lệ\"}";
        send_response(client, 400, "application/json; charset=utf-8", error, strlen(error));
        return;
    }
    if (strcmp(action, "check") == 0) {
        suffix = "check"; message = "Đã gửi yêu cầu kiểm tra firmware.";
    } else if (strcmp(action, "update") == 0) {
        suffix = "update"; message = "Đã gửi yêu cầu bắt đầu OTA.";
    } else if (strcmp(action, "abort") == 0) {
        suffix = "update/abort"; message = "Đã gửi yêu cầu dừng OTA.";
    } else {
        const char *error = "{\"error\":\"Thao tác OTA không hợp lệ\"}";
        send_response(client, 400, "application/json; charset=utf-8", error, strlen(error));
        return;
    }
    snprintf(topic, sizeof(topic), "%s/bridge/request/device/ota_update/%s", mqtt_base_topic, suffix);
    snprintf(payload, sizeof(payload), "{\"id\":\"%s\"}", device);
    rc = mosquitto_publish(mqtt_client, NULL, topic, (int)strlen(payload), payload, 0, false);
    if (rc != MOSQ_ERR_SUCCESS) {
        const char *error = "{\"error\":\"MQTT chưa sẵn sàng để gửi yêu cầu\"}";
        send_response(client, 400, "application/json; charset=utf-8", error, strlen(error));
        return;
    }
    snprintf(payload, sizeof(payload), "{\"ok\":true,\"message\":\"%s\"}", message);
    send_response(client, 202, "application/json; charset=utf-8", payload, strlen(payload));
}

static void handle_client(socket_handle client)
{
    size_t capacity = 16384U;
    size_t used = 0;
    size_t header_size = 0;
    size_t content_length = 0;
    size_t expected_size = 0;
    char *request = (char *)malloc(capacity + 1U);
    char *header_end = NULL;
    unsigned char *body = NULL;
    if (request == NULL) return;
    while (header_end == NULL) {
        int received;
        if (used == capacity) {
            char *expanded;
            if (capacity >= MAX_HTTP_HEADER) {
                const char *error = "{\"error\":\"HTTP header quá lớn\"}";
                send_response(client, 413, "application/json; charset=utf-8", error, strlen(error));
                free(request);
                return;
            }
            capacity *= 2U;
            if (capacity > MAX_HTTP_HEADER) capacity = MAX_HTTP_HEADER;
            expanded = (char *)realloc(request, capacity + 1U);
            if (expanded == NULL) { free(request); return; }
            request = expanded;
        }
        received = recv(client, request + used, (int)(capacity - used), 0);
        if (received <= 0) { free(request); return; }
        used += (size_t)received;
        request[used] = '\0';
        header_end = strstr(request, "\r\n\r\n");
    }
    header_size = (size_t)(header_end - request) + 4U;
    if (!parse_content_length(request, &content_length)) {
        const char *error = "{\"error\":\"File OTA vượt quá giới hạn 4 MB hoặc Content-Length lỗi\"}";
        send_response(client, 413, "application/json; charset=utf-8", error, strlen(error));
        free(request);
        return;
    }
    expected_size = header_size + content_length;
    if (expected_size > capacity) {
        char *expanded = (char *)realloc(request, expected_size + 1U);
        if (expanded == NULL) { free(request); return; }
        request = expanded;
        capacity = expected_size;
    }
    while (used < expected_size) {
        int received = recv(client, request + used, (int)(expected_size - used), 0);
        if (received <= 0) { free(request); return; }
        used += (size_t)received;
    }
    request[used] = '\0';
    body = (unsigned char *)request + header_size;
    if (strncmp(request, "GET /api/status ", 16) == 0) {
        char *json = create_status_json();
        if (json != NULL) { send_response(client, 200, "application/json; charset=utf-8", json, strlen(json)); free(json); }
    } else if (strncmp(request, "POST /api/ota ", 14) == 0) {
        handle_ota(client, (const char *)body);
    } else if (strncmp(request, "POST /api/firmware ", 19) == 0) {
        handle_firmware_upload(client, body, content_length);
    } else if (strncmp(request, "GET / ", 6) == 0 || strncmp(request, "GET /index.html ", 16) == 0) {
        serve_file(client, "index.html", "text/html; charset=utf-8");
    } else if (strncmp(request, "GET /styles.css ", 16) == 0) {
        serve_file(client, "styles.css", "text/css; charset=utf-8");
    } else if (strncmp(request, "GET /app.js ", 12) == 0) {
        serve_file(client, "app.js", "application/javascript; charset=utf-8");
    } else {
        const char *message = "Not found";
        send_response(client, 404, "text/plain; charset=utf-8", message, strlen(message));
    }
    free(request);
}

static THREAD_RESULT web_thread(void *argument)
{
    socket_handle server_socket;
    struct sockaddr_in address;
    int reuse = 1;
    (void)argument;
#ifdef _WIN32
    WSADATA winsock_data;
    if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) return THREAD_RETURN;
#endif
    server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == INVALID_SOCKET) goto cleanup;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((unsigned short)web_port);
    if (bind(server_socket, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR || listen(server_socket, 8) == SOCKET_ERROR) {
        fprintf(stderr, "Khong mo duoc dashboard port %d\n", web_port);
        CLOSE_SOCKET(server_socket);
        goto cleanup;
    }
    printf("Dashboard: http://127.0.0.1:%d\n", web_port);
    fflush(stdout);
    while (web_running) {
        fd_set read_set;
        struct timeval timeout;
        int ready;
        FD_ZERO(&read_set);
        FD_SET(server_socket, &read_set);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
#ifdef _WIN32
        ready = select(0, &read_set, NULL, NULL, &timeout);
#else
        ready = select(server_socket + 1, &read_set, NULL, NULL, &timeout);
#endif
        if (ready > 0) {
            socket_handle client = accept(server_socket, NULL, NULL);
            if (client != INVALID_SOCKET) { handle_client(client); CLOSE_SOCKET(client); }
        }
    }
    CLOSE_SOCKET(server_socket);
cleanup:
#ifdef _WIN32
    WSACleanup();
#endif
    return THREAD_RETURN;
}

bool dashboard_start(struct mosquitto *mosq, const char *base_topic)
{
    const char *port_text = getenv("WEB_PORT");
    long parsed_port;
    char *end;
    mqtt_client = mosq;
    if (base_topic != NULL && base_topic[0] != '\0') snprintf(mqtt_base_topic, sizeof(mqtt_base_topic), "%s", base_topic);
    if (port_text != NULL && port_text[0] != '\0') {
        parsed_port = strtol(port_text, &end, 10);
        if (*end == '\0' && parsed_port > 0 && parsed_port <= 65535) web_port = (int)parsed_port;
    }
#ifdef _WIN32
    InitializeCriticalSection(&state_lock);
#endif
    dashboard_initialized = true;
    web_running = true;
#ifdef _WIN32
    web_thread_handle = CreateThread(NULL, 0, web_thread, NULL, 0, NULL);
    if (web_thread_handle == NULL) { web_running = false; DeleteCriticalSection(&state_lock); dashboard_initialized = false; return false; }
#else
    if (pthread_create(&web_thread_handle, NULL, web_thread, NULL) != 0) { web_running = false; dashboard_initialized = false; return false; }
    web_thread_created = true;
#endif
    return true;
}

void dashboard_stop(void)
{
    if (!dashboard_initialized) return;
    web_running = false;
#ifdef _WIN32
    if (web_thread_handle != NULL) { WaitForSingleObject(web_thread_handle, 2000); CloseHandle(web_thread_handle); web_thread_handle = NULL; }
    DeleteCriticalSection(&state_lock);
#else
    if (web_thread_created) { pthread_join(web_thread_handle, NULL); web_thread_created = false; }
#endif
    dashboard_initialized = false;
}
