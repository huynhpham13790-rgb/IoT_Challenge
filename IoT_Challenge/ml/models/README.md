# Hai model AI đang chạy trên chip

Đây là **đúng hai file** mà firmware nhúng vào và chạy — không phải bản gần
giống, không phải bản demo. Để ở đây để bất kỳ ai (ban giám khảo, thầy hướng
dẫn, thành viên mới) cũng mở ra xem được mà không cần build gì cả.

| File | Vai trò | Vào/ra | Kích thước |
|---|---|---|---|
| `autoencoder_int8.tflite` | Phát hiện bất thường **tức thời** trên 6 đặc trưng | int8 `[1,6]` → `[1,6]` | 3.272 B |
| `forecaster_int8.tflite` | **Dự báo** 16 giây tới từ 64 giây vừa qua, 4 kênh | int8 `[1,1,64,4]` → `[1,64]` | 30.616 B |

## Xem model bằng gì

- **Netron** (không cần cài gì): mở https://netron.app rồi kéo thả file vào.
  Thấy được từng lớp, kích thước tensor, tham số lượng tử hoá int8.
- **Python**: `tf.lite.Interpreter(model_path=...)` rồi `get_input_details()`.
- **MLTK / Simplicity Studio**: hai file này là `.tflite` chuẩn, dùng được với
  bộ công cụ AI/ML của Silicon Labs (profiler, flatbuffer converter) như bất kỳ
  model TFLite Micro nào khác.

## Quan hệ với code trên chip

| File model | Nhúng thành | Runner |
|---|---|---|
| `autoencoder_int8.tflite` | `firmware/model_data.h` | `firmware/model_runner.cpp` |
| `forecaster_int8.tflite` | `firmware/model_data_ts.h` | `firmware/ts_forecaster.cpp` |

Mảng byte trong hai file `.h` đó **chính là nội dung** của hai file `.tflite`
này — kiểm chứng được bằng cách so byte, không cần tin lời:

```bash
python3 - <<'EOF'
import re
h = open('firmware/model_data.h').read()
body = h.split('= {',1)[1].split('};',1)[0]
arr = bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})', body))
print(arr == open('ml/models/autoencoder_int8.tflite','rb').read())
EOF
```

Cách huấn luyện lại và đánh giá: `ml/ai_timeseries/`, xem mục 11 của
[`docs/AI_TIME_SERIES_TAT_TAN_TAT.md`](../../docs/AI_TIME_SERIES_TAT_TAN_TAT.md).
Giải thích AI làm gì, khi nào báo động:
[`docs/AI_HOAT_DONG_THE_NAO.md`](../../docs/AI_HOAT_DONG_THE_NAO.md).
