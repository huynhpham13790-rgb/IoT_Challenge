# Gói AI + luồng dự báo — Smart IV Monitor

Đây là **toàn bộ phần AI trên chip**, tách nguyên trạng ra khỏi firmware cũ để
ghép vào firmware mới. Code không bị sửa gì so với nhánh gốc: cùng ba model,
cùng ngưỡng, cùng logic quyết định. Chỉ có tài liệu này là mới.

## Có gì trong này

```
src/
  ai_engine.h / .cpp     Ba interpreter TFLM, ba arena riêng. Lượng tử hoá /
                         giải lượng tử hoá bằng scale+zero-point đọc thẳng từ
                         tensor. Không giữ lịch sử, không ra quyết định.
  ai_fusion.h / .c       Luồng dự báo mỗi giây: cửa sổ 64 s -> dự báo 16 s ->
                         residual -> bộ lọc bền vững K=11 -> mức cảnh báo.
  line_rules.h / .c      Luật load-cell (tắc / chảy tự do / hết dịch). Không
                         phải model, nhưng nhánh LINE trong fusion OR với nó.
  clinical_limits.h      Ngưỡng lâm sàng cứng. KHÔNG đi qua bộ lọc K=11.
  models/                Ba model đã nhúng sẵn dạng mảng C + opcode resolver
                         (sinh tự động bởi ml/export_c_headers.py).
tflite/                  Ba file .tflite gốc, đúng bản đang chạy trên chip.
contract/
  sensor_hub.h           API cảm biến mà gói này gọi tới. Firmware mới phải
                         cung cấp các hàm sh_* liệt kê bên dưới.
  sl_tflite_micro_config.h  Cấu hình TFLM. Quan trọng:
                         SL_TFLITE_MICRO_INTERPRETER_INIT_ENABLE phải = 0.
```

## Ba model

| Model | Vào | Ra | RAM arena |
|---|---|---|---|
| Drip forecaster | 64 s drops_ratio, int8 `[1,1,64,1]` | 16 s tới, `[1,16]` | 4 KB |
| Vitals forecaster | 64 s HR+SpO2 xen kẽ, `[1,1,64,2]` | 16 s tới, `[1,32]` | 4 KB |
| Vitals autoencoder | `[1,2]` = {HR lệch baseline, SpO2} | `[1,2]` -> MSE | 2 KB |

Ba model chạy độc lập, ba interpreter, ba arena. Cố ý như vậy: gộp lại thì một
model lỗi sẽ kéo chết cả ba (`AllocateTensors` all-or-nothing, `Invoke` dừng ở
operator lỗi đầu tiên).

## Ghép vào firmware mới — 3 bước

**1. Build.** Thêm `src/*.c`, `src/*.cpp`, `src/models/*.c` vào project. Cần
TensorFlow Lite Micro (component `tensorflow_lite_micro` của Simplicity SDK) và
`config/sl_tflite_micro_config.h` với `SL_TFLITE_MICRO_INTERPRETER_INIT_ENABLE 0`
— bộ khởi tạo mặc định của SDK xử lý lỗi bằng `while (1)`, treo máy tại giường
bệnh. `ai_engine.cpp` là C++, các file còn lại là C thuần.

**2. Cung cấp các hàm sh_* này** (khai báo trong `contract/sensor_hub.h`):

```c
typedef enum { CH_DISABLED = 0, CH_OK = 1, CH_LOST = 2 } ch_state_t;

float      sh_hr(void);                    // bpm
float      sh_spo2(void);                  // %
float      sh_drops_ratio(void);           // drops_per_min / target
float      sh_drops_per_min(void);
float      sh_target_drops_per_min(void);
float      sh_flow_weight_g(void);         // load cell, gram
ch_state_t sh_hr_state(void);
ch_state_t sh_spo2_state(void);
ch_state_t sh_flow_state(void);
ch_state_t sh_drops_state(void);
```

Cộng thêm `alert_level_t` (enum mức cảnh báo, xem `sensor_hub.h`). Đó là toàn bộ
bề mặt tiếp xúc — gói này không đụng tới I2C, Zigbee, OLED hay OTA.

**3. Gọi.**

```c
ai_fusion_init();                 // 1 lần lúc boot, không bao giờ fail chí mạng
ai_fusion_set_hr_baseline(bpm);   // sau ~60 s hiệu chỉnh HR của bệnh nhân

fusion_result_t f;                // rồi mỗi giây, SAU khi poll cảm biến:
ai_fusion_step(&f);
// f.level, f.headline, f.causes[], f.drip_forecast_16s, f.hr_forecast_16s, ...
```

## Ba điều dễ làm hỏng khi merge

1. **Hằng số chuẩn hoá** trong `ai_engine.h` (`AI_DRIP_CENTRE/SCALE`,
   `AI_HR_*`, `AI_SPO2_*`) phải khớp tuyệt đối với `ml/common.py` lúc train.
   Lệch một con số thì không có lỗi nào báo ra — model chỉ lặng lẽ trả sai.
2. **Ngưỡng** (`AI_DRIP_RESIDUAL_THRESHOLD`, `AI_VITALS_RESIDUAL_THRESHOLD`,
   `AI_AE_THRESHOLD`) đo trên **bản int8 đang ship**, không phải bản float.
   Đây không phải núm vặn cho demo đẹp.
3. **Luật lâm sàng không đi qua K=11.** SpO2 < 90% báo động ngay giây thấy nó.
   Đừng "dọn dẹp" cho nhất quán với nhánh AI.

## Kiểm chứng đúng model đang chạy

Log boot in ra tham số lượng tử hoá đọc từ chính tensor, khớp với ba file
`tflite/`:

```
[AI] drip   ready: arena 2836/4096 B, in scale 39214/1e6, zp -55
[AI] vitals ready: arena 2868/4096 B, in scale 76469/1e6, zp  49
[AI] ae     ready: arena 1108/2048 B, in scale 66618/1e6, zp  75
```

Nếu một model không load, `ai_fusion_init()` vẫn trả về bình thường, nhánh của
model đó không bao giờ kích hoạt, và luật lâm sàng vẫn gánh thiết bị.
