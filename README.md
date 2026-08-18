# Smart IV Monitor — hệ thống giám sát dịch truyền có AI

Thiết bị gắn ở mỗi giường bệnh tự đo nhịp tim, SpO2, tốc độ truyền dịch và số
giọt/phút, chạy AI ngay trên chip để phát hiện bất thường, rồi gửi về màn hình
trung tâm qua Zigbee. Firmware cập nhật được từ xa bằng Zigbee OTA.

Dự án IoT Challenge 2026 — nhóm ICTU.

> **Tài liệu này viết để một người (hoặc một AI) chưa từng đụng vào dự án đọc
> xong là chạy được.** Mọi đường dẫn, cổng, tên file đều là giá trị thật đang
> dùng, không phải ví dụ.

---

## 1. Kiến trúc

```
┌────────────────────┐            ┌────────────────────┐
│  BOARD CẢM BIẾN    │   Zigbee   │  BOARD NCP         │
│  BRD2709A          │ ─────────► │  BRD2608A          │
│  EFR32MG26B510     │  2.4 GHz   │  EFR32MG26B510     │
│  F3200IM48-B       │            │  F3200IM68         │
│                    │            │  (Coordinator)     │
│  Cảm biến + AI     │            └─────────┬──────────┘
│  + OLED + 3 LED    │                      │ USB / EZSP
│  VCOM = COM8       │                      │ COM10
└────────────────────┘                      ▼
                                  ┌────────────────────────┐
                                  │  MÁY WINDOWS           │
                                  │  ├ Mosquitto   :1885   │
                                  │  ├ Zigbee2MQTT :8080   │
                                  │  └ zigbee_reader :8090 │
                                  └────────────────────────┘
```

Bốn khối rời nhau, nối bằng giao thức chuẩn (Zigbee → EZSP → MQTT → HTTP), sửa
khối nào cũng không đụng ba khối kia.

---

## 2. Bốn thư mục trong repo

| Thư mục | Chạy ở đâu | Việc |
|---|---|---|
| `IoT_Challenge/` | Chip EFR32MG26 ở giường bệnh | Đọc cảm biến, chạy AI, quyết định báo động, hiện OLED, gửi Zigbee, nhận OTA |
| `Xg26_Gateway_NCP/` | Chip EFR32MG26 cắm vào PC | NCP UART — "phiên dịch" Zigbee ↔ USB. Mẫu chuẩn Silicon Labs, gần như không sửa |
| `SmartIV_MQTT/` | Máy Windows | Mosquitto + Zigbee2MQTT + file OTA |
| `pi/` | Máy Windows (hoặc Raspberry Pi) | `zigbee_reader` viết bằng C: đọc MQTT, dựng dashboard web, điều khiển OTA |

---

## 3. Phần cứng

| Board | Chip | J-Link | Cổng COM | Vai trò |
|---|---|---|---|---|
| BRD2709A Rev A03 (Explorer) | EFR32MG26B510F3200IM48-B | 440364712 | COM8 | Thiết bị đầu giường |
| BRD2608A Rev A04 (Dev Kit) | EFR32MG26B510F3200IM68 | 440371997 | COM10 | Coordinator / NCP |

Thiết bị đầu giường: `IEEE = 0x64028ffffe641802`

### Cảm biến và đèn báo (đấu trên header mikroBUS của BRD2709A)

| Thành phần | Chân |
|---|---|
| Cảm biến giọt | PD02 (AN) |
| MAX30102 (HR + SpO2), OLED SSD1306 | I2C PC05 = SCL, PC07 = SDA |
| HX711 (load cell) | PC xx — xem `firmware/sensor_hub.c` |
| LED xanh | PA07 (PWM) |
| LED vàng | PA04 (TX) |
| LED đỏ | PA05 (RX) |
| Còi | PC04 (CS) |

Cả bốn ngõ ra đèn/còi đều **active HIGH**. Nếu module của bạn là loại
active-low, đổi `ALERT_ACTIVE_HIGH` thành `0` trong `firmware/sensor_hub.c`.

---

## 4. AI chạy ở đâu

Cả hai model chạy **trên chip**, không cần mạng, không gửi dữ liệu bệnh nhân đi
đâu để suy luận:

- **Autoencoder** 6 đặc trưng (TensorFlow Lite Micro) — `firmware/model_data.h`,
  3.272 byte. Học "dáng vẻ bình thường"; đầu vào lệch khỏi những gì từng thấy
  thì sai số tái tạo tăng vọt.
- **Bộ dự báo chuỗi thời gian** — `firmware/model_data_ts.h`, 30.616 byte. Cửa
  sổ 64 giây, dự báo 16 giây tới, cảnh báo sớm trước khi vượt ngưỡng.

Tổng ~33,9 KB trọng số, khai báo là mảng `const` nên **nằm luôn trong file
firmware** — cập nhật OTA là cập nhật cả model.

Song song luôn có **luật lâm sàng cứng** (SpO2 < 90%, nhịp tim lệch > 30%
baseline, flow ngoài 0,3–1,5× mức bác sĩ đặt). Hai cơ chế OR với nhau: thà báo
thừa còn hơn bỏ sót.

---

## 5. Cần cài gì

| Phần mềm | Dùng để |
|---|---|
| Simplicity Studio / VS Code + extension Silicon Labs | Build firmware |
| Simplicity Commander | Nạp firmware, tạo file `.gbl` |
| Mosquitto (`C:\Program Files\mosquitto`) | MQTT broker |
| Node.js + corepack + pnpm | Chạy Zigbee2MQTT |
| Python 3 | Chạy `tools/make_ota.py` |
| MSVC (Visual Studio) | Build `zigbee_reader.exe` trên Windows |
| gcc + libmosquitto-dev | Build `zigbee_reader` trên Raspberry Pi |

Đường dẫn công cụ trên máy hiện tại:

```
Simplicity Commander : C:\Users\khanh\.silabs\slt\installs\archive\Simplicity Commander\commander.exe
Python               : C:\Users\khanh\.silabs\slt\installs\archive\python-v3.10.3\python\python.exe
```

> Thư mục Commander **có dấu cách**, nên trong PowerShell phải bọc nháy và gọi
> bằng toán tử `&`.

---

## 6. ⚠️ Đường dẫn cứng phải sửa khi đổi máy

Đây là phần hay làm người mới mất thời gian nhất. Năm chỗ dưới đây ghi cứng
đường dẫn/cổng của máy hiện tại:

| File | Dòng | Giá trị hiện tại |
|---|---|---|
| `pi/start_all.ps1` | `$Workspace` | `E:\SmartIV_MQTT` |
| `pi/start_all.ps1` | `$MosquittoExe` | `C:\Program Files\mosquitto\mosquitto.exe` |
| `pi/build_windows.cmd` | `VCVARS` | `E:\VisualStudio\VS2026\VC\Auxiliary\Build\vcvars64.bat` |
| `SmartIV_MQTT/config/mosquitto.conf` | `persistence_location`, `log_dest` | `E:/SmartIV_MQTT/...` |
| `SmartIV_MQTT/zigbee2mqtt/data/configuration.yaml` | `serial.port` | `COM10` |

Cắm board NCP vào cổng USB khác là `COM10` đổi số — kiểm tra trong Device
Manager rồi sửa `configuration.yaml`.

---

## 7. Chạy hệ thống

### 7.1. Cài Zigbee2MQTT lần đầu

`node_modules` không nằm trong repo (vài trăm MB). Cài lại:

```powershell
cd SmartIV_MQTT\zigbee2mqtt
corepack pnpm install
```

### 7.2. Nạp firmware cho hai board

```powershell
$commander = "C:\Users\khanh\.silabs\slt\installs\archive\Simplicity Commander\commander.exe"

# Board NCP (BRD2608A)
& $commander flash Xg26_Gateway_NCP\cmake_gcc\build\base\Xg26_Gateway_NCP.hex --serialno 440371997

# Board cảm biến (BRD2709A)
& $commander flash IoT_Challenge\cmake_gcc\build\base\smart-iv-monitor.hex --serialno 440364712
```

### 7.3. Khởi động toàn bộ bằng một lệnh

```powershell
cd pi
.\run_windows.cmd
```

Script `start_all.ps1` sẽ lần lượt: bật Mosquitto cổng 1885 → khởi động
Zigbee2MQTT với COM10 → đợi MQTT online → chạy `zigbee_reader.exe` → mở trình
duyệt.

| Giao diện | Địa chỉ |
|---|---|
| Dashboard Smart IV (tự viết) | http://127.0.0.1:8090 |
| Zigbee2MQTT frontend | http://127.0.0.1:8080 |
| MQTT broker | 127.0.0.1:1885 |

Nhấn `Ctrl+C` ở cửa sổ reader để dừng cả ba tiến trình.

### 7.4. Build lại `zigbee_reader`

```powershell
cd pi
.\build_windows.cmd          # Windows, cần MSVC
```

```bash
cd pi
sudo apt install -y build-essential libmosquitto-dev
make                          # Raspberry Pi
```

---

## 8. Luồng dữ liệu

Board đầu giường ghi giá trị vào cluster tự định nghĩa **Smart IV Vitals**
(`0xFC01`, mfgCode `0x1049`) trên endpoint 2, plugin Reporting tự gửi báo cáo
khi giá trị đổi.

Zigbee2MQTT giải mã và publish lên:

```
zigbee2mqtt/0x64028ffffe641802
```

`zigbee_reader` subscribe `zigbee2mqtt/#`, lọc topic hệ thống, và đẩy lên
dashboard. Sửa hàm `handle_zigbee_data()` trong `pi/main.c` nếu muốn ghi
database hoặc chuyển tiếp tới server khác.

Board cũng in một dòng JSON đầy đủ ra VCOM mỗi giây (`[JSON]{...}`) — đây là
đường dự phòng đọc thẳng USB khi không có gateway.

Biến môi trường `zigbee_reader` đọc:

| Biến | Mặc định |
|---|---|
| `MQTT_HOST` | `127.0.0.1` |
| `MQTT_PORT` | `1885` |
| `Z2M_BASE_TOPIC` | `zigbee2mqtt` |
| `WEB_PORT` | `8090` |
| `WEB_ROOT` | `web` |
| `OTA_UPLOAD_DIR` | `<workspace>\zigbee2mqtt\data\ota` |
| `OTA_INDEX_PATH` | `<workspace>\zigbee2mqtt\data\xg26_ota_index.json` |

---

## 9. Cập nhật firmware qua Zigbee OTA

### 9.1. Bốn định danh phải khớp

| Tham số | Giá trị | Khai ở đâu |
|---|---|---|
| Manufacturer Code | `0x1049` (4169) | `config/zcl/zcl_config.zap` |
| Image Type | `0x0000` | `config/ota-client-policy-config.h` |
| Firmware Version | số bản build | `config/ota-client-policy-config.h` |
| Storage slot | slot 0, `0x08190000`–`0x08314000` (1.589.248 byte) | bootloader |

Sai manufacturer hoặc image type → client trả `INVALID_FIELD` và bỏ qua file.

### 9.2. Quy trình đầy đủ

**Bước 1 — tăng version TRƯỚC khi build.**

`IoT_Challenge/config/ota-client-policy-config.h`:

```c
#define SL_ZIGBEE_AF_PLUGIN_OTA_CLIENT_POLICY_FIRMWARE_VERSION   4
```

Số này phải **lớn hơn** version đang chạy trên board, và nó cũng điều khiển số
lần nháy LED lúc khởi động (xem mục 10).

**Bước 2 — build** trong VS Code.

**Bước 3 — tạo file `.gbl`.**

```powershell
cd IoT_Challenge\cmake_gcc\build

& "C:\Users\khanh\.silabs\slt\installs\archive\Simplicity Commander\commander.exe" gbl create base\smart-iv-monitor-v4.gbl --app base\smart-iv-monitor.s37
```

**Bước 4 — bọc thành `.ota`.**

Máy này không có `image-builder` của Zigbee SDK, và Commander không tạo được
file `.ota`. Dùng script kèm theo:

```powershell
& "C:\Users\khanh\.silabs\slt\installs\archive\python-v3.10.3\python\python.exe" ..\..\tools\make_ota.py base\smart-iv-monitor-v4.gbl 4
```

Output phải in `fileVersion : 4`. Nếu vẫn thấy số cũ → quên bước 1 hoặc quên
build lại.

**Bước 5 — tải lên gateway.** Mở http://127.0.0.1:8090 → *Chọn file* → *Tải
lên*. Gateway kiểm tra header, chép vào `data\ota\` và tự viết
`xg26_ota_index.json`.

**Bước 6 — bấm Cập nhật một lần duy nhất.** Đừng bấm hai lần: lần thứ hai bị
Z2M từ chối với thông báo "already in progress".

### 9.3. Cấu trúc file `.ota`

File `.ota` chỉ là file `.gbl` đội thêm **62 byte mũ**:

```
offset 0..55    Header OTA
                  magic 0x0BEEF11E, headerLen 56, manuf 0x1049,
                  imageType 0, fileVersion N, stackVersion 0x0002,
                  headerString[32], totalImageSize
offset 56..61   Sub-element: tagId 0x0000, length = kích thước GBL
offset 62..     Toàn bộ file .gbl, không sửa đổi
```

`tools/make_ota.py` dựng đúng cấu trúc này. Đã kiểm chứng: tách phần GBL khỏi
một file `.ota` cũ rồi dựng lại cho ra **file giống hệt từng byte**.

### 9.4. Tốc độ truyền — hai giới hạn từ hai phía

Trong `SmartIV_MQTT/zigbee2mqtt/data/configuration.yaml`:

```yaml
ota:
  zigbee_ota_override_index_location: xg26_ota_index.json
  image_block_response_delay: 50      # tối thiểu 50, Z2M từ chối giá trị nhỏ hơn
  default_maximum_data_size: 63       # trần cứng 63, xem bên dưới
```

- **`default_maximum_data_size` không được vượt 63.** `ota-client.c:159` đặt
  `MAX_CLIENT_DATA_SIZE = 63`; dòng 1954 **drop thẳng** mọi block lớn hơn. Z2M
  cho phép tới 100 nhưng đặt 64 trở lên là OTA đứng im, không phải nhanh hơn.
- **`image_block_response_delay` không được nhỏ hơn 50.** Schema của Z2M chặn,
  đặt nhỏ hơn thì Z2M không khởi động được.

Bảng tham chiếu (file ~418 KB, RTT ~15 ms):

| size | delay | Số block | Thời gian |
|---|---|---|---|
| 50 | 250 ms (mặc định) | 8.357 | ~37 phút |
| 50 | 100 ms | 8.357 | ~16 phút |
| 63 | 100 ms | 6.633 | ~13 phút |
| **63** | **50 ms** | **6.633** | **~7 phút** ← nhanh nhất hợp lệ |

Đổi `configuration.yaml` **phải restart Zigbee2MQTT** mới có hiệu lực. Và phải
sửa **trước khi** bắt đầu OTA — Z2M ghi đè lại file này trong lúc chạy.

### 9.5. Cái gì sống sót qua OTA

Gecko Bootloader chỉ ghi đè vùng application, **không đụng NVM3**. Nên sau khi
lên bản mới, những thứ sau vẫn còn nguyên: network key, PAN ID (thiết bị không
rớt mạng, không phải join lại), hệ số hiệu chuẩn HX711, giá trị tare.

Mặt trái: nếu đổi **cấu trúc** dữ liệu NVM3 giữa hai version, firmware mới sẽ
đọc dữ liệu cũ theo layout mới → giá trị rác. Phải có chiến lược migrate hoặc
đánh version cho từng key NVM3.

---

## 10. Nhận biết version đang chạy

Ba cách, không cần cắm dây:

**Đếm số nhịp LED lúc khởi động.** Cả 3 LED nháy đồng thời, số nhịp = số
version. v4 → 4 nhịp. Số này lấy thẳng từ `FIRMWARE_VERSION` nên không bao giờ
lệch với version thật.

**Đọc dòng đầu trên VCOM:**

```
[BOOT] Smart IV firmware v4 - nhay 4 lan
```

**Gõ lệnh CLI trên VCOM** (Simplicity Commander → VCOM Console, nhớ bấm *Clear*
trước vì log in ra mỗi giây):

```
plugin ota-client info      → Manuf ID / Image Type / Current Version
plugin ota-client status    → state, waiting for response, download offset
plugin ota-client start     → khởi động máy trạng thái OTA
option print-rx-msgs disable → tắt dump gói tin cho đỡ nghẽn UART
```

---

## 11. Sự cố đã gặp và cách xử lý

### 11.1. OTA trả `Default Response: FAILURE`, không có Image Block Request

**Triệu chứng.** Board gửi Query Next Image Request, Z2M trả về SUCCESS với
đúng manufacturer/version/size, nhưng board đáp lại `cmdId=2, status=FAILURE`.
Sau 150 giây Z2M báo timeout. Firmware không hề bắt đầu tải.

**Nguyên nhân gốc.** Trong toàn bộ `commandParse()`, status `FAILURE` (0x01)
chỉ được trả về từ **đúng một chỗ** — `ota-client.c:1099`, khi
`currentBootloadState != BOOTLOAD_STATE_QUERY_NEXT_IMAGE`. Các nhánh lỗi khác
đều trả status khác (`MALFORMED_COMMAND` 0x80, `NOT_AUTHORIZED` 0x7E,
`INVALID_FIELD` 0x85, `INSUFFICIENT_SPACE` 0x89), nên loại trừ được hết.

Response của Z2M về quá muộn. Trong lúc chờ, client timeout nhiều lần, biến
`errors` chạm ngưỡng và tự chuyển sang `DISCOVER_SERVER`. Khi response tới nơi
thì state đã khác → FAILURE.

Chi tiết then chốt: `errors` tăng **hai đơn vị** mỗi lần timeout (một ở
`ota-client.c:534`, một ở `:1580`), nên ngưỡng 10 thực chất chỉ cho phép 5 lần
timeout ≈ 30 giây.

**Cách sửa** (trong `config/ota-client-config.h`, không đụng logic SDK):

```c
#define SL_ZIGBEE_AF_PLUGIN_OTA_CLIENT_QUERY_ERROR_THRESHOLD          60   // was 10
#define SL_ZIGBEE_AF_PLUGIN_OTA_CLIENT_SERVER_DISCOVERY_DELAY_MINUTES  1   // was 10
```

**Không được** sửa bằng cách xoá dòng kiểm tra state ở 1099. Làm vậy chỉ giấu
triệu chứng: client sẽ chấp nhận một response mà nó không còn ở đúng ngữ cảnh
để xử lý.

### 11.2. Z2M báo không có bản cập nhật

`FIRMWARE_VERSION` trong `ota-client-policy-config.h` phải **nhỏ hơn** version
của file `.ota`. Nếu để bằng nhau, board tự khai đã chạy bản mới nhất.

### 11.3. Đèn báo động không chuyển đỏ

Kiểm tra `firmware/app.c` xem còn sót dòng test ép cứng không:

```c
sh_alert_set_level(ALERT_GREEN);                        // SAI - code test
sh_alert_set_level(alert_level_from_result(&r, &ts));   // ĐÚNG
```

### 11.4. Tên file OTA hiện ra là `xg24_...`

`pi/dashboard.c` dòng ~497 ghi cứng chuỗi `"xg24_uploaded_v%u.ota"` từ thời còn
dùng board xG24. Chỉ là tên hiển thị, không ảnh hưởng chức năng. Sửa thành
`xg26_` nếu muốn cho gọn.

---

## 12. Git

Repo riêng tư: **https://github.com/Quan098b/FPT**

```powershell
cd F:\FPT\FPT
git add -A
git commit -m "Mô tả ngắn việc vừa làm"
git push
```

Thiết lập lần đầu:

```powershell
cd F:\FPT\FPT
git init
git branch -M main
git remote add origin https://github.com/Quan098b/FPT.git
git add -A
git commit -m "Smart IV: firmware xG26, NCP, Zigbee2MQTT, gateway"
git push -u origin main
```

`.gitignore` chỉ loại đúng một thứ: `node_modules` của Zigbee2MQTT (vài trăm
MB, GitHub chặn file trên 100 MB, cài lại bằng `corepack pnpm install`). Mọi
thứ khác — mã nguồn, cấu hình, model AI, SDK chép kèm, file build, firmware
`.ota` — đều được đẩy lên.

> Repo đang để **Private**. Trong đó có `coordinator_backup.json` chứa network
> key mạng Zigbee. Nếu đổi sang Public thì phải đổi khoá **trước**: xoá
> `coordinator_backup.json`, `database.db`, `state.json` rồi khởi động lại
> Zigbee2MQTT. Khoá cũ nằm trong lịch sử git vĩnh viễn, xoá file ở commit sau
> không gỡ được ra.

---

## 13. Tài liệu chi tiết hơn

| File | Nội dung |
|---|---|
| `IoT_Challenge/README.md` | Kiến trúc firmware, cấu trúc thư mục |
| `IoT_Challenge/docs/HUONG_DAN_A_Z.md` | Hướng dẫn đầy đủ từ đầu |
| `IoT_Challenge/docs/AI_HOAT_DONG_THE_NAO.md` | Autoencoder hoạt động ra sao |
| `IoT_Challenge/docs/AI_TIME_SERIES_TAT_TAN_TAT.md` | Bộ dự báo chuỗi thời gian |
| `IoT_Challenge/tools/make_ota.py` | Script đóng gói `.ota`, comment rõ từng offset |
| `pi/README.md` | Chi tiết `zigbee_reader` và dashboard |
