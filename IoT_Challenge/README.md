# Smart IV Monitor — hệ thống giám sát dịch truyền có AI

Hệ thống theo dõi **dịch truyền tĩnh mạch** cho bệnh viện: một thiết bị gắn ở
mỗi giường tự đo nhịp tim, SpO2, tốc độ truyền và số giọt/phút, chạy AI ngay
trên chip để phát hiện bất thường, rồi gửi về màn hình trung tâm cho y tá qua
Zigbee. Khi có sự cố (tụt oxy, tắc dây, chảy tự do, tuột cảm biến) thì báo
động **ở cả hai nơi**: màn hình đầu giường và console ở trạm điều dưỡng.

Dự án làm cho IoT Challenge 2026 (nhóm ICTU).

---

## Hệ thống gồm những gì

```
┌──────────────┐  Zigbee   ┌──────────────┐  USB/UART  ┌──────────────────────┐
│ Board cảm    │ ────────► │  Board NCP   │ ─────────► │  Raspberry Pi        │
│ biến đầu     │           │ (Coordinator)│            │  mosquitto (MQTT)    │
│ giường       │           │  BRD2709A    │            │  zigbee2mqtt         │
│ EFR32xG26    │           │              │            │  gateway_test (C)    │
│ + OLED       │           └──────────────┘            └──────────┬───────────┘
└──────────────┘                                                  │ TCP JSON :5000
                                                                  ▼
                                                    ┌───────────────────────────┐
                                                    │  HIS Server (ASP.NET Core)│
                                                    │  MySQL + REST + SignalR   │
                                                    │  Web UI cho y tá          │
                                                    └───────────────────────────┘
```

Bốn "trạm" độc lập, nối với nhau bằng giao thức chuẩn (Zigbee → MQTT → TCP/
JSON), mỗi trạm viết bằng công nghệ hợp với việc của nó và **sửa được riêng**
mà không đụng ba trạm kia.

| # | Trạm | Chạy ở đâu | Việc |
|---|---|---|---|
| 1 | Firmware `smart-iv-monitor` | Chip EFR32xG26 gắn ở giường | Đọc cảm biến, chạy AI, quyết định báo động, hiện OLED, gửi Zigbee |
| 2 | Firmware NCP | Board thứ hai cắm vào Pi | "Phiên dịch" Zigbee ↔ USB, dùng nguyên bản Silicon Labs, không sửa gì |
| 3 | zigbee2mqtt + `gateway_test` | Raspberry Pi | Giải mã gói Zigbee thành JSON, bắn tiếp qua TCP tới server |
| 4 | HIS Server | Máy chủ / máy dev | Lưu MySQL, tính trạng thái giường, phục vụ web UI realtime |

## AI chạy ở đâu, làm gì

Cả hai model đều chạy **trên chip**, không cần mạng, không gửi dữ liệu bệnh
nhân đi đâu để suy luận:

- **Autoencoder** (6 đặc trưng, TensorFlow Lite Micro): học "dáng vẻ bình
  thường" của bệnh nhân; đầu vào lệch khỏi những gì model từng thấy thì sai số
  tái tạo tăng vọt → cảnh báo. Bắt được các bất thường **không định nghĩa
  trước được bằng ngưỡng**.
- **Bộ dự báo chuỗi thời gian** (cửa sổ 64 giây, dự báo 16 giây tới): cho biết
  chỉ số đang **đi về đâu**, không chỉ đang ở đâu — cảnh báo sớm trước khi
  vượt ngưỡng lâm sàng.

Hai file `.tflite` đang chạy nằm trong [`ml/models/`](ml/models/) — mở bằng
[Netron](https://netron.app) là xem được từng lớp, không phải hộp đen.

Song song với AI luôn có **luật lâm sàng cứng** (SpO2 < 90%, nhịp tim lệch
>30% baseline riêng của bệnh nhân, flow ngoài 0.3–1.5× mức bác sĩ đặt...).
Hai cơ chế OR với nhau: thà báo thừa còn hơn bỏ sót.

---

## Cấu trúc thư mục

Repo xếp theo **trạm xử lý**: mã của trạm nào nằm trong thư mục của trạm đó.

| Thư mục | Nội dung |
|---|---|
| `firmware/` | **Trạm 1** — mã nguồn chạy trên chip: đọc cảm biến, AI, báo động, OLED, Zigbee |
| `gateway/` | **Trạm 3** — `main.c` (cầu MQTT → TCP) và converter cho zigbee2mqtt, đều chạy trên Pi |
| `server/` | **Trạm 4** — HIS Server: ASP.NET Core + MySQL + SignalR + web UI |
| `ml/models/` | Hai file `.tflite` **đang chạy thật trên chip** — mở bằng Netron là xem được |
| `ml/ai_timeseries/` | Pipeline huấn luyện model dự báo (Python), xuất ra header nhúng vào firmware |
| `tools/` | `serial_gateway.py` — đường dự phòng đọc thẳng USB khi không có Pi |
| `docs/` | Toàn bộ tài liệu (xem bên dưới) |

Bốn thứ này **bắt buộc nằm ở gốc repo**, không gom vào `firmware/` được:
`smart-iv-monitor.slcp`, `config/`, `autogen/`, `cmake_gcc/` và `main.c`.
Simplicity Studio và `slc` coi gốc repo là gốc project và tự sinh ba thư mục
đó ở đúng vị trí; đẩy chúng xuống thư mục con sẽ làm hỏng cả việc mở project
trong Studio lẫn lệnh `slc generate`. Đường dẫn của mã trong `firmware/` được
khai trong `smart-iv-monitor.slcp` — thêm file `.c` mới thì phải khai vào đó
rồi chạy lại `slc generate`.

`simplicity_sdk_*/` và `aiml_*/` là bản copy SDK, **không** được commit (xem
`.gitignore`): build thật compile với SDK cài trong `~/.silabs/`.

---

## Tài liệu

| File | Nội dung |
|---|---|
| [`docs/HUONG_DAN_A_Z.md`](docs/HUONG_DAN_A_Z.md) | **Đọc file này trước.** Toàn bộ hệ thống từ chip tới web, giải thích cả *vì sao* thiết kế như vậy, kèm bảng lỗi thường gặp |
| [`docs/AI_HOAT_DONG_THE_NAO.md`](docs/AI_HOAT_DONG_THE_NAO.md) | AI làm gì, **khi nào báo động và khi nào cố tình không báo**, kèm 6 ví dụ cụ thể — viết cho người không đọc code |
| [`ml/models/`](ml/models/) | Hai file `.tflite` đang chạy thật trên chip, mở bằng Netron được |
| [`docs/AI_TIME_SERIES_TAT_TAN_TAT.md`](docs/AI_TIME_SERIES_TAT_TAN_TAT.md) | Model dự báo chuỗi thời gian: dữ liệu, huấn luyện, đánh giá, nhúng vào firmware |
| [`docs/Dataset_va_Phuong_phap_AI_SmartIV.md`](docs/Dataset_va_Phuong_phap_AI_SmartIV.md) | Dataset và phương pháp của autoencoder |
| [`docs/Nghien_cuu_Nang_cap_AI_Time_Series.md`](docs/Nghien_cuu_Nang_cap_AI_Time_Series.md) | Nghiên cứu nâng cấp phần AI |
| [`server/README.md`](server/README.md) | Riêng HIS Server: API, DB, cách chạy |

---

## Chạy nhanh

Chi tiết đầy đủ (kể cả cách pair Zigbee và xử lý sự cố) nằm trong
`docs/HUONG_DAN_A_Z.md`; đây chỉ là bản rút gọn.

**Firmware** (không cần mở GUI Simplicity Studio):

```bash
NINJA=~/.silabs/slt/installs/conan/p/ninja*/p/ninja
COMMANDER=~/.silabs/slt/installs/archive/commander/commander

cd cmake_gcc/build && $NINJA                      # build
$COMMANDER flash base/smart-iv-monitor.hex --serialno <SERIAL_BOARD>
```

Chỉ khi sửa `.slcp` hoặc file `.zap` mới cần sinh lại code (`slc generate`,
xem mục 1.6 của tài liệu A-Z).

**HIS Server** (cần .NET 8 + Docker):

```bash
docker start his-mysql                            # MySQL 8, lần đầu xem server/README.md
cd server/src/HisServer && dotnet run --urls http://0.0.0.0:5100
```

Web UI ở `http://localhost:5100`. Cổng **5000 để riêng cho TCP** nhận dữ liệu
từ gateway (`Tcp:Port` trong `appsettings.json`) — đừng cho web nghe trùng
cổng đó. `0.0.0.0` là để gateway trên Pi gọi vào được từ máy khác, không chỉ
localhost.

**Gateway trên Pi**: cả `mosquitto`, `zigbee2mqtt` và `gateway` đều chạy nền
bằng systemd, tự bật lại sau khi Pi khởi động:

```bash
sudo systemctl restart gateway
journalctl -u gateway -f
```

`ExecStart` trong `/etc/systemd/system/gateway.service` chứa **IP LAN của máy
chạy HIS Server** — máy đó đổi IP thì phải sửa dòng đó rồi
`daemon-reload` + `restart`, nếu không dashboard sẽ lặng lẽ ngừng cập nhật.

---

## Những quyết định thiết kế đáng chú ý

Phần này để người đọc code khỏi "sửa lại cho gọn" đúng những chỗ đã cân nhắc kỹ:

- **"Chưa có dữ liệu" không phải là "ổn định", cũng không phải là "nguy kịch".**
  Cảm biến chưa cắm đọc ra 0; nếu coi số 0 đó là chỉ số thật thì giường trống
  cũng kêu "SpO2 0% — nguy kịch". Kênh mất tín hiệu hiện `--` và đẩy giường
  sang **Warning** kèm lý do "No signal from: ...".
- **Chỉ một nơi được quyết định trạng thái giường**: `VitalsStatusEvaluator.cs`
  trên server. Gateway chỉ chuyển tiếp dữ liệu thô + cờ, không tự suy luận —
  trước đây hai nơi cùng tính và ra kết quả khác nhau.
- **Bác sĩ đổi ngưỡng từ xa, không bao giờ phải nạp lại chip.** Ngưỡng là biến
  runtime, ghi qua Zigbee và lưu vào NVM3 nên sống sót qua mất điện. Nạp lại
  chip giữa ca truyền là mất baseline nhịp tim, mất tare cân và mất kết nối
  hàng chục giây.
- **Nhịp báo cáo Zigbee chia theo vai trò**, không dùng chung một mức: cảnh báo
  đi ngay tick đầu tiên, còn cân nặng thì 10 giây/lần — giảm từ ~120 xuống 12
  bản tin/phút mà độ trễ cảnh báo không đổi.
- **Bất kỳ thứ gì hỏng ở tầng ngoài cũng không được làm hỏng việc theo dõi**:
  không có màn hình OLED, không load được model AI, không có mạng — thiết bị
  vẫn đo và vẫn báo động bằng luật lâm sàng.
