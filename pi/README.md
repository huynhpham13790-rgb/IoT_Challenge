# Zigbee2MQTT reader (C)

Chuong trinh nhan du lieu Zigbee tu MQTT. Khong mo truc tiep COM10; COM10 thuoc Zigbee2MQTT.

## Cai dat tren Raspberry Pi

```bash
sudo apt update
sudo apt install -y build-essential libmosquitto-dev
make
```

Neu Mosquitto/Zigbee2MQTT cung chay tren Pi:

```bash
./zigbee_reader
```

Neu broker dang chay tren may Windows, dung dia chi LAN cua may Windows:

```bash
MQTT_HOST=192.168.1.10 MQTT_PORT=1885 ./zigbee_reader
```

Tuy chon:

- `MQTT_HOST`: mac dinh `127.0.0.1`
- `MQTT_PORT`: mac dinh `1885`
- `Z2M_BASE_TOPIC`: mac dinh `zigbee2mqtt`
- `MQTT_USER`, `MQTT_PASSWORD`: neu broker yeu cau dang nhap

Chuong trinh subscribe `zigbee2mqtt/#`, loc cac topic he thong va xu ly payload trang thai thiet bi. Sua ham `handle_zigbee_data()` trong `main.c` de luu database, gui server, hoac xu ly payload JSON.

## Dashboard web va OTA

Khi chuong trinh chay, mo dashboard tai:

```text
http://127.0.0.1:8090
```

Khi chay `run_windows.cmd`, trinh duyet se tu mo dashboard.

Dashboard hien thi trang thai MQTT/Zigbee, cac thiet bi va du lieu cam bien moi nhat. Co the chon thiet bi de kiem tra, bat dau hoac dung Zigbee OTA thong qua MQTT API cua Zigbee2MQTT.

Tuy chon:

- `WEB_PORT`: cong dashboard, mac dinh `8090`
- `WEB_ROOT`: thu muc chua `index.html`, `styles.css`, `app.js`; mac dinh `web`

Khi chay tren Raspberry Pi, mo `http://<IP-cua-Pi>:8090` tu may tinh cung mang LAN.
## Build va chay tren Windows

Khong dung `sudo`, `apt` hoac `make` trong PowerShell. Chay:

```powershell
.\build_windows.cmd
.\run_windows.cmd
```
## Chay tat ca tren Windows

Lenh sau tu khoi dong Mosquitto 1885, Zigbee2MQTT COM10, cho MQTT online, sau do chay C reader:

```powershell
.\run_windows.cmd
```

XG26 tai giuong tu hoat dong khi duoc cap nguon; khong mo COM8/COM10 tu chuong trinh C. Khi reader ket thuc hoac nhan Ctrl+C, launcher se dung ca Zigbee2MQTT va Mosquitto.
