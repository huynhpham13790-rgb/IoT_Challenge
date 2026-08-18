# Nhật ký sửa lỗi — server

File này ghi lại các lỗi đã phát hiện và sửa trong `server/`, kèm nguyên
nhân gốc, để người đọc sau (hoặc AI) không phải dò lại từ đầu. Từ mục
2026-08-19 (app di động) trở đi, phạm vi mở rộng sang `mobile/` vì app và
server cùng nằm một repo và cùng một luồng làm việc — xem chi tiết app ở
[`mobile/README.md`](../mobile/README.md).

---

## 2026-08-19 — Room bị revert + tab luôn quay về Users

### Triệu chứng
1. Đổi Room của một giường ở tab **Bed directory**, tải lại trang thì Room
   lại quay về giá trị cũ.
2. Đang xem tab nào (ví dụ Bed directory), tải lại trang thì luôn bị đá về
   tab **Users**, dù trước đó không hề bấm vào Users.

### Nguyên nhân #1 — Room bị ghi đè bởi luồng ingestion
`BedTcpIngestionService.ProcessReadingAsync` (server nhận dữ liệu sinh hiệu
qua TCP :5000) ghi đè **vô điều kiện** `state.Room = reading.Room` mỗi khi
nhận được 1 gói tin từ thiết bị/gateway, rồi lưu thẳng xuống MySQL. Field
`room` trong JSON thiết bị gửi vốn được set cứng ở gateway (một dòng lệnh
systemd trên Pi), không phản ánh Room mà quản trị viên vừa gán qua UI.

Vì các gói tin sinh hiệu đến liên tục (mỗi giây, kể cả từ script fake test),
Room quản trị viên vừa sửa bị ghi đè lại chỉ trong vòng 1 giây — **không
liên quan gì đến việc F5**, F5 chỉ là lúc người dùng nhìn thấy giá trị đã bị
ghi đè từ trước.

**File sửa:** [`src/HisServer/Ingestion/BedTcpIngestionService.cs`](src/HisServer/Ingestion/BedTcpIngestionService.cs)
(hàm `ProcessReadingAsync`, quanh dòng 497).

**Cách sửa:** chỉ lấy `Room` từ thiết bị khi giường đó **chưa từng có
Room** (state.Room rỗng — tức lần đầu server thấy giường này). Sau khi đã
có Room, nó là dữ liệu quản trị viên quản lý qua Bed directory
(`Capabilities.ManageBeds`), thiết bị không còn quyền ghi đè.

```csharp
// Room là dữ liệu của quản trị viên, không phải của thiết bị. Chỉ set lần
// đầu server thấy giường này - ghi đè mỗi lần đọc sẽ âm thầm xoá bỏ mọi
// lần đổi Room qua Bed directory ngay khi thiết bị gửi gói tiếp theo.
if (string.IsNullOrWhiteSpace(state.Room))
{
    state.Room = reading.Room;
}
```

### Nguyên nhân #2 — không nhớ tab đang xem
`wwwroot/js/main.js` chọn tab mặc định lúc load trang theo một danh sách
capability cố định (`LANDING`): `ward.view → devices.manage →
users.manage → logs.view`. Vai trò Admin không có `ward.view` /
`devices.manage`, nên luôn rơi vào `users.manage` trước tiên — **kể cả khi
`beds.manage` (tab Bed directory) không hề nằm trong danh sách `LANDING`
đó**. UI trước giờ không lưu lại tab người dùng đang xem.

**File sửa:** [`src/HisServer/wwwroot/js/main.js`](src/HisServer/wwwroot/js/main.js)

**Cách sửa:** mỗi lần `switchTab()` được gọi, lưu tên tab vào
`localStorage` (`his.lastTab`). Lúc trang load lại, ưu tiên khôi phục tab
đã lưu — nếu nav-button của tab đó vẫn còn trong DOM sau khi
`Session.applyTo()` áp quyền (tức vai trò hiện tại vẫn được xem tab đó).
Chỉ dùng tab mặc định theo `LANDING` khi chưa từng lưu tab nào hoặc quyền
đã thay đổi.

### Cần làm sau khi sửa
- Phần C# (`BedTcpIngestionService.cs`) cần **build lại + restart server**:
  ```powershell
  cd server/src/HisServer
  dotnet build
  dotnet run --urls http://0.0.0.0:5100
  ```
- Phần JS (`main.js`) chỉ cần **hard refresh trình duyệt** (Ctrl+F5) —
  server đã ép `Cache-Control: no-cache, must-revalidate` cho static file
  nên không cần build gì thêm, nhưng cache trình duyệt vẫn có thể giữ bản
  cũ nếu không hard refresh.

### Ghi chú cho lần sau
- Bất kỳ field nào hiển thị ở **Bed directory** (Room, và tương lai có thể
  còn field khác quản trị viên chỉnh tay) đều phải coi là **dữ liệu do con
  người quản lý**, không phải dữ liệu do thiết bị báo cáo. Luồng ingestion
  (`ProcessReadingAsync`) chỉ nên ghi các field sinh hiệu/trạng thái kỹ
  thuật (spo2, heart_rate, drip_rate, device health...), không nên đụng
  vào field thuộc "cấu trúc khoa phòng" trừ khi đó là lần đầu tạo giường.
  Đây là đúng loại bug đã từng xảy ra một lần với cờ tín hiệu bị reset lúc
  restart (xem comment trong `BedRepository.UpdatePatientAsync`) — một
  luồng ghi "vô tình" đụng vào field không thuộc phạm vi của nó.
- Test bằng script fake devices: `Huynh/fake/fake_devices.py` — gửi
  `"room":"ICU-1"` cố định mỗi giây cho `BED-01`/`BED-02`, nên là công cụ
  tốt để tái hiện đúng lỗi #1 này (gói tin liên tục đến ngay lập tức ghi
  đè lại field vừa sửa tay).

---

## 2026-08-19 — App di động (Android) cho y tá: crash lúc mở app + nối API thật

App Android (`mobile/`, package `com.example.smartivmonitor`) là giao diện
tablet cho y tá: danh sách giường dạng thẻ + màn chi tiết theo layout
master-detail (`SlidingPaneLayout`), tìm/lọc theo giường và mức cảnh báo.
Ban đầu dựng bằng dữ liệu mẫu cứng trong code, sau đó nối vào `HisServer`
thật qua REST + cookie session — cùng cơ chế đăng nhập với web console.

### Lỗi #1 — Crash ngay khi mở app trên thiết bị thật

**Triệu chứng:** build chạy được trên máy dev nhưng cài lên tablet Samsung
Tab S10 FE thì mở lên là tắt ngay (`FATAL EXCEPTION` trong `AndroidRuntime`).

**Nguyên nhân:** `activity_main.xml` khai 2 fragment **tĩnh** (danh sách
giường + chi tiết giường) qua `FragmentContainerView android:name=...`.
`FragmentManager` lên lịch thêm 2 fragment đó lúc layout được **attach vào
window**, không phải lúc inflate xong — nên `MainActivity.onCreate()` gọi
thẳng vào fragment con (`BedDetailFragment.showBackButton(...)`) ngay sau
`setContentView()` thì view binding bên trong fragment đó còn `null` →
`NullPointerException`. Thử ép chạy bằng
`supportFragmentManager.executePendingTransactions()` cũng không giải quyết
được, vì transaction chưa hề được lên lịch tại thời điểm đó.

**File sửa:**
[`mobile/app/src/main/java/com/example/smartivmonitor/MainActivity.kt`](../mobile/app/src/main/java/com/example/smartivmonitor/MainActivity.kt),
[`BedDetailFragment.kt`](../mobile/app/src/main/java/com/example/smartivmonitor/BedDetailFragment.kt)

**Cách sửa:** bỏ hẳn việc `MainActivity` với tay vào fragment con lúc
`onCreate()`. Mỗi fragment tự cấu hình chính nó trong `onViewCreated()` của
nó (chắc chắn view đã tồn tại) — ví dụ `BedDetailFragment` tự quyết định
có hiện nút back hay không dựa vào `smallestScreenWidthDp`, và tự gọi
ngược lại `(activity as? MainActivity)?.closeDetailPane()` khi cần, thay vì
để `MainActivity` gọi vào nó.

**Ghi chú cho lần sau:** với `FragmentContainerView` khai tĩnh trong XML,
KHÔNG được giả định fragment con đã có view ngay sau `setContentView()` —
kể cả trong `onCreate()` của chính Activity chứa nó. An toàn nhất là để mỗi
fragment tự lo phần khởi tạo của mình trong `onViewCreated()`, chỉ giao
tiếp chéo qua các sự kiện do người dùng bấm (lúc đó mọi thứ chắc chắn đã
sẵn sàng).

### Việc #2 — Nối app vào server thật (không còn là bản demo)

App ban đầu dùng dữ liệu giả viết cứng (`SampleBeds.kt`, đã xoá). Đã thêm:

- **Màn đăng nhập** (`LoginActivity`) — nhập địa chỉ server (IP:port) một
  lần, lưu vào `SharedPreferences` (`ServerPrefs.kt`) nên lần sau mở app
  không phải nhập lại. Yêu cầu bắt buộc: **thiết bị và server phải cùng
  Wi-Fi/LAN**, vì server không có domain public, chỉ nghe trên IP LAN
  (`--urls http://0.0.0.0:5100` — xem `server/README.md`).
- **Phiên đăng nhập bằng cookie**, không phải token — khớp với cách server
  xác thực (`his_session`, xem `Program.cs` phần cookie auth). Cookie được
  lưu bền qua `PersistentCookieJar.kt` (OkHttp `CookieJar` tự viết, mirror
  ra `SharedPreferences`).
- **`GET /api/beds` polling mỗi 4 giây** thay vì SignalR thật — chọn cách
  chắc ăn trước, chưa làm real-time đẩy dữ liệu. Nếu 1 lần gọi lỗi (mất
  mạng chập chờn) thì **giữ nguyên dữ liệu cũ trên màn hình**, chỉ báo lỗi
  nhỏ (Snackbar) — tránh trường hợp khoa "biến mất" khỏi app chỉ vì rớt
  mạng 1 nhịp.
- Nhận `401` từ server (hết phiên) → tự động đưa về lại màn đăng nhập.

**Vì sao cho phép HTTP thường (không TLS):** server nội bộ bệnh viện chạy
plain HTTP trên LAN, không có domain cố định để scope network security
config theo domain — nên `AndroidManifest.xml` bật
`android:usesCleartextTraffic="true"` toàn app kèm comment giải thích rõ lý
do, thay vì cấu hình mập mờ.
