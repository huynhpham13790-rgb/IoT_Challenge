# Smart IV Monitor — App di động (Android)

Giao diện tablet cho y tá: danh sách giường dạng thẻ + màn chi tiết theo
layout master-detail (`SlidingPaneLayout` — hai cột song song trên tablet
đủ rộng, tự co về một cột trên điện thoại), tìm/lọc theo mã giường/phòng
và theo mức cảnh báo. Nói chuyện với [`../server`](../server) qua REST,
cùng cơ chế đăng nhập cookie session với web console.

Xem lịch sử lỗi đã sửa (crash lúc mở app, nối API thật...) ở
[`../server/sua.md`](../server/sua.md).

## Kiến trúc màn hình

```
app/src/main/java/com/example/smartivmonitor/
  LoginActivity.kt         Màn đăng nhập: địa chỉ server + tài khoản
  MainActivity.kt          SlidingPaneLayout lưu trữ 2 fragment
  BedListFragment.kt       Danh sách giường + thống kê + tìm/lọc
  BedDetailFragment.kt     Chi tiết 1 giường (sinh hiệu, cảnh báo)
  BedAdapter.kt            RecyclerView adapter cho thẻ giường
  model/Bed.kt             Model dữ liệu giường dùng trong app
  net/                     Lớp kết nối server (xem bên dưới)
  data/BedRepository.kt    Gọi net/, map sang model/Bed
```

## Kết nối server

- **Không có discovery tự động** — y tá/kỹ thuật viên nhập địa chỉ server
  (`IP:port`, ví dụ `192.168.1.50:5100`) một lần ở màn đăng nhập, lưu vào
  `SharedPreferences` (`net/ServerPrefs.kt`). Thiết bị và server phải
  **cùng Wi-Fi/LAN**.
- **Xác thực bằng cookie**, giống hệt web console (`his_session`, xem
  `Program.cs` trên server) — không dùng token. Cookie được giữ bền qua
  `net/PersistentCookieJar.kt`, một `OkHttp CookieJar` tự viết mirror ra
  `SharedPreferences`, nên thoát app rồi mở lại không phải đăng nhập lại.
- **`GET /api/beds` polling mỗi 4 giây** (`BedListFragment.kt`) — chưa có
  SignalR client, đây là bước đầu chắc ăn. Khi có thời gian, thay bằng kết
  nối SignalR đẩy dữ liệu real-time thì chỉ cần đổi bên trong
  `BedRepository.kt`, không đụng tới UI.
- Server chạy plain HTTP trên LAN nội bộ, không có domain cố định để scope
  network security config theo domain — nên `AndroidManifest.xml` bật
  `android:usesCleartextTraffic="true"` toàn app (có comment giải thích).

## Build & chạy

Mở thư mục `mobile/` bằng Android Studio (Gradle sync tự chạy), hoặc dòng
lệnh:

```powershell
cd mobile
.\gradlew.bat :app:assembleDebug
```

Cần `server/` đang chạy và cùng mạng với thiết bị/emulator — xem
[`../server/README.md`](../server/README.md) mục "Configuration" để chạy
server với `--urls http://0.0.0.0:5100` (bắt buộc, không chỉ `localhost`).

## Còn thiếu (việc tiếp theo)

- Đổi polling sang SignalR thật (server đã có sẵn `MonitoringHub`, xem
  `server/src/HisServer/Hubs/MonitoringHub.cs`).
- Màn hình xác nhận cảnh báo (acknowledge) hiện chỉ ẩn banner tại chỗ,
  chưa gọi API — cần nối `POST /api/alerts/{id}/ack`.
- Chưa có màn "đổi mật khẩu bắt buộc" cho tài khoản `must_change_password`
  (web console đã có, xem `wwwroot/js/session.js`).
