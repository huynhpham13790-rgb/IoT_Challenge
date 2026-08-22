# Ba model AI đang chạy trên chip

Đây là **đúng ba file** mà firmware nhúng vào và chạy — không phải bản gần
giống, không phải bản demo. Để ở đây để bất kỳ ai (ban giám khảo, thầy hướng
dẫn, thành viên mới) cũng mở ra xem được mà không cần build gì cả.

| File | Vai trò | Vào/ra | Kích thước |
|---|---|---|---|
| `drip_forecaster_int8.tflite` | **Dự báo** dòng chảy 16 giây tới từ 64 giây số giọt | int8 `[1,1,64,1]` → `[1,16]` | 22.400 B |
| `vitals_forecaster_int8.tflite` | **Dự báo** sinh hiệu 16 giây tới từ 64 giây HR+SpO2 | int8 `[1,1,64,2]` → `[1,32]` | 23.520 B |
| `vitals_ae_int8.tflite` | Trạng thái sinh lý **hiện tại** có bình thường không | int8 `[1,2]` → `[1,2]` | 3.136 B |

**Ba model chạy độc lập, trên ba vùng nhớ riêng.** Không model nào nhìn thấy dữ
liệu của model khác — đó là điểm cốt lõi của bản v2, và nó cho phép hệ thống trả
lời được câu *"hỏng ở dây truyền hay hỏng ở bệnh nhân?"*.

**Chứng minh đây đúng là model đang chạy:** log khởi động của chip in ra tham số
lượng tử hoá đọc từ tensor, và nó khớp tuyệt đối với ba file này:

```
[AI] drip   ready: arena 2836/4096 B, in scale 39214/1e6, zp -55
[AI] vitals ready: arena 2868/4096 B, in scale 76469/1e6, zp  49
[AI] ae     ready: arena 1108/2048 B, in scale 66618/1e6, zp  75
```

## Xem model bằng gì

- **Netron** (không cần cài gì): mở https://netron.app rồi kéo thả file vào.
  Thấy được từng lớp, kích thước tensor, tham số lượng tử hoá int8.
- **Python**: `tf.lite.Interpreter(model_path=...)` rồi `get_input_details()`.
- **MLTK / Simplicity Studio**: hai file này là `.tflite` chuẩn, dùng được với
  bộ công cụ AI/ML của Silicon Labs (profiler, flatbuffer converter) như bất kỳ
  model TFLite Micro nào khác.

## Quan hệ với code trên chip

| File model | Sinh thành | Chạy bởi |
|---|---|---|
| `drip_forecaster_int8.tflite` | `firmware/models/model_drip.{c,h}` + `model_drip_opcodes.h` | `firmware/ai_engine.cpp` |
| `vitals_forecaster_int8.tflite` | `firmware/models/model_vitals.{c,h}` + `model_vitals_opcodes.h` | `firmware/ai_engine.cpp` |
| `vitals_ae_int8.tflite` | `firmware/models/model_ae.{c,h}` + `model_ae_opcodes.h` | `firmware/ai_engine.cpp` |

Các file trong `firmware/models/` là **sinh tự động, đừng sửa tay**. Chúng được
tạo bằng **chính tool MLTK của Silicon Labs** (`ml/export_c_headers.py` gọi tool
đó ba lần) — không phải mảng byte viết tay. Giá trị lớn nhất nằm ở file
`*_opcodes.h`: nội dung của nó được suy ra bằng cách **phân tích chính
flatbuffer**, nên nó liệt kê đúng operator model dùng và không bao giờ lệch khi
đổi kiến trúc. Chi tiết: [`docs/MLTK_AUTOGEN.md`](../../docs/MLTK_AUTOGEN.md).

Sinh lại:

```bash
.venv-ai/bin/python ml/export_c_headers.py
```

Mảng byte trong các file `.c` đó **chính là nội dung** của ba file `.tflite`
này — kiểm chứng được bằng cách so byte, không cần tin lời:

```bash
python3 - <<'EOF'
import re, pathlib
for stem, tfl in (('drip','drip_forecaster_int8'),
                  ('vitals','vitals_forecaster_int8'),
                  ('ae','vitals_ae_int8')):
    c = pathlib.Path(f'firmware/models/model_{stem}.c').read_text()
    body = c[c.index('{')+1:c.rindex('}')]
    arr = bytes(int(t,16) for t in re.findall(r'0x[0-9a-fA-F]{2}', body))
    ref = pathlib.Path(f'ml/models/{tfl}.tflite').read_bytes()
    print(f'{stem:7s} khớp từng byte: {arr == ref}')
EOF
```

Huấn luyện lại và đánh giá: `ml/`, xem
[`docs/AI_TIME_SERIES_TAT_TAN_TAT.md`](../../docs/AI_TIME_SERIES_TAT_TAN_TAT.md)
mục 8. Giải thích AI làm gì, khi nào báo động:
[`docs/AI_HOAT_DONG_THE_NAO.md`](../../docs/AI_HOAT_DONG_THE_NAO.md).
