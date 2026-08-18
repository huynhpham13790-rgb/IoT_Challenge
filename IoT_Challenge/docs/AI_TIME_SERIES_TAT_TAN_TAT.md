# AI time series Smart IV — tất tần tật

Tài liệu đầy đủ về **model AI mới** (dự báo chuỗi thời gian), thay cho model
autoencoder tức thời cũ. Mọi con số trong đây là **số đo thật** từ các lần chạy
đã thực hiện, không phải ước lượng — kèm lệnh để tái lập.

- Ngày: 2026-07-30
- Phần cứng: BRD2709A (EFR32MG26B510F3200IM48), SDK `simplicity_sdk 2025.12.3` + extension `aiml 2.2.2`
- Liên quan: `Nghien_cuu_Nang_cap_AI_Time_Series.md` (phần nghiên cứu/lý luận),
  `Dataset_va_Phuong_phap_AI_SmartIV.md` (mô tả model CŨ)

> **TRẠNG THÁI: đã tích hợp xong vào logic báo động và chạy thật trên chip.**
> `ts_monitor.c` giữ cửa sổ 64 giây, chạy dự báo mỗi giây, và
> `alert_level_from_result()` trong `app.c` quyết định đèn/còi theo 3 tầng
> (xem mục 12). Kết quả dự báo cũng hiện trên dashboard ở mục
> "AI forecast (on-chip)".

---

## 1. Tóm tắt trong 30 giây

| | Model CŨ | Model MỚI |
|---|---|---|
| Đầu vào | 6 số tại **1 thời điểm** | **64 giây × 4 kênh** |
| Đầu ra | Tái tạo lại 6 số đó | **Dự báo 16 giây tiếp theo** |
| Kiến trúc | FC `6→4→2→4→6` | CNN 1D (Conv2D height-1) + FC |
| Kích thước | 3.272 byte | 30.616 byte |
| Tham số | ~100 | 21.504 |
| Thời gian suy luận | (chưa đo) | **4,77 ms** = 0,47% chu kỳ AI |
| Làm được gì thêm | — | xu hướng, cảnh báo sớm, phân biệt thoáng qua/kéo dài |

**Kết quả cốt lõi:** so với cách ngưỡng tức thời, recall tăng gần **gấp đôi**
(29,8% → 58,3%) mà báo nhầm do nhiễu thoáng qua lại **giảm** (5,1% → 3,4%).

---

## 2. Vì sao phải đổi

Model cũ nhận 6 số tại **một thời điểm**, autoencoder nút thắt 2 chiều, quyết
định bằng `MSE > 1.4336`. Về mặt toán học đó là một **ngưỡng đa biến phi tuyến** —
nên nhận xét "giỏi điện tử là làm được" là công bằng.

Hệ quả nghiêm trọng hơn: **không có ký ức** nên hai tình huống này giống hệt nhau
dưới mắt model:

- HR = 130 trong **2 giây** (bệnh nhân trở người, chạm tay vào cảm biến PPG)
- HR = 130 trong **10 phút** (nhịp nhanh thật, cần can thiệp)

Model buộc phải báo cả hai (→ báo nhầm liên tục) hoặc bỏ cả hai (→ bỏ sót ca
thật). Đây là **giới hạn kiến trúc**, không phải chuyện tinh chỉnh ngưỡng.

Thêm nữa, firmware chạy AI 1 lần/giây nhưng mỗi tick được đánh giá độc lập rồi
**bỏ đi** — toàn bộ thông tin về xu hướng, độ biến thiên, thời lượng, tương quan
theo thời gian đều không dùng.

---

## 3. Kiến trúc mới

### 3.1 Vào / ra

```
Đầu vào : 64 giây quá khứ × 4 kênh   (shape 1×1×64×4, int8)
Đầu ra  : dự báo 16 giây tiếp theo × 4 kênh, PHẲNG 64 số (int8)
          index: out[h * 4 + c]   với h = 0..15 (giây), c = 0..3 (kênh)
```

**4 kênh** (thứ tự cố định, firmware phải giữ đúng):

| # | Kênh | Chuẩn hóa | Ghi chú |
|---|---|---|---|
| 0 | `heart_rate` | `(hr - 80) / 20` | bpm |
| 1 | `spo2` | `(spo2 - 97) / 2` | % |
| 2 | `drops_ratio` | `(dpm/target_dpm - 1.0) / 0.35` | tỉ lệ so mức bác sĩ đặt |
| 3 | cân nặng **tương đối** | `(weight - weight[đầu cửa sổ]) / 5.0` | gam |

Vì sao kênh 3 dùng giá trị **tương đối** so với đầu cửa sổ, không dùng tuyệt đối:
bịch dịch nào cũng bắt đầu ~500 g rồi cạn dần, nên giá trị tuyệt đối chỉ cho biết
"đã truyền bao lâu"; **thông tin thật nằm ở tốc độ giảm**. Dùng tương đối làm
model bất biến với khối lượng ban đầu.

Chuẩn hóa dùng **hằng số cố định**, không dùng `scaler.json` — firmware chỉ cần
một phép trừ và một phép chia, không phải nạp thêm file cấu hình.

### 3.2 Từ một model lấy ra được ba thứ

1. **Xu hướng** — độ dốc đường dự báo → *"HR đang đi lên/xuống X bpm/phút"*
2. **Cảnh báo sớm** — nếu đường dự báo **vượt ngưỡng lâm sàng trước khi** thực tế
   xảy ra → báo trước hàng chục giây
3. **Bất thường** — sai số dự báo (model chỉ học động lực học **bình thường**, nên
   điều gì không dự báo được chính là bất thường)

### 3.3 Các lớp

```
Input (1, 64, 4)                        # "ảnh" cao 1 pixel
  Conv2D 16 filters, kernel (1,5), stride (1,2), ReLU   -> (1, 32, 16)
  Conv2D 32 filters, kernel (1,5), stride (1,2), ReLU   -> (1, 16, 32)
  Conv2D 32 filters, kernel (1,3), stride (1,2), ReLU   -> (1,  8, 32)
  Reshape (256)                                          # kích thước TĨNH
  Dense 48, ReLU
  Dense 64                                               # = 16 giây × 4 kênh
```

**Tổng: 21.504 tham số.**

---

## 4. Ba quyết định kiến trúc theo đúng phần cứng xG26

Đây là phần quan trọng nhất để "tận dụng con chip", và cũng dễ chọn sai nhất. Ba
điều dưới đây tớ **kiểm chứng trực tiếp trong SDK**, không tra web.

### 4.1 Kernel nào được MVP tăng tốc

Tìm trong `aiml220b56d6ae053/p/src/kernels/mvp1/`:

```
add.cc  conv.cc  depthwise_conv.cc  fully_connected.cc
mul.cc  pooling.cc  transpose_conv.cc
```

→ `CONV_2D`, `DEPTHWISE_CONV_2D`, `FULLY_CONNECTED`, pooling, add, mul **được
tăng tốc bằng phần cứng**.

Trong `micro_mutable_op_resolver.h` có `UnidirectionalSequenceLSTM`, `Svdf`,
`CircularBuffer` — chạy được nhưng **trên CPU M33, không có MVP**.

### 4.2 KHÔNG dùng LSTM

Y văn TinyML cho thấy 1D-CNN đạt độ chính xác *ngang hoặc cao hơn* LSTM (~95%)
với chi phí bộ nhớ/tính toán thấp hơn nhiều trên MCU. Ở đây còn thêm lý do riêng:
**CNN được MVP tăng tốc, LSTM thì không** — chọn LSTM là tự bỏ phần cứng tăng tốc
mà chip này có sẵn.

### 4.3 KHÔNG dùng dilation, KHÔNG dùng Keras Conv1D

**Dilation:** MVP không hỗ trợ → TCN cổ điển (dilated convolution) sẽ rơi về
kernel chậm. Muốn nhìn xa theo thời gian thì dùng **stride + pooling xếp tầng**.

**Keras `Conv1D`:** đây là cái bẫy không hiển nhiên. TFLite dịch **mỗi** lớp
Conv1D thành ba op `EXPAND_DIMS → CONV_2D → RESHAPE`. Với 3 lớp conv, model phình
thành **15 op** mà chỉ 5 op được tăng tốc:

```
EXPAND_DIMS, CONV_2D, RESHAPE, EXPAND_DIMS, CONV_2D, RESHAPE,
EXPAND_DIMS, CONV_2D, RESHAPE, SHAPE, STRIDED_SLICE, PACK,
RESHAPE, FULLY_CONNECTED, FULLY_CONNECTED          (34.272 byte)
```

Dùng thẳng `Conv2D` với kernel `(1,k)` + `Reshape` kích thước tĩnh + batch cố
định = 1 khi convert, cho ra đúng **6 op**:

```
CONV_2D, CONV_2D, CONV_2D, RESHAPE, FULLY_CONNECTED, FULLY_CONNECTED
                                                    (30.616 byte)
```

Tất cả đều thuộc nhóm MVP tăng tốc (`RESHAPE` trong TFLM chỉ là đổi cách nhìn bộ
nhớ, không tốn phép tính). **Giảm 9 op và 12% kích thước.**

Cũng cần tránh `Flatten`: với batch động nó sinh ra `SHAPE`/`STRIDED_SLICE`/`PACK`.
Dùng `Reshape` với kích thước tĩnh.

### 4.4 Ràng buộc kích thước tensor

MVP yêu cầu `width × channels ≤ 2047` và mọi chiều `≤ 1024`. Model này:

| Tensor | width × channels | OK? |
|---|---|---|
| Input | 64 × 4 = 256 | ✓ |
| Sau conv1 | 32 × 16 = 512 | ✓ |
| Sau conv2 | 16 × 32 = 512 | ✓ |
| Sau conv3 | 8 × 32 = 256 | ✓ |

Số kênh để **chẵn** (16/32) để tăng khả năng được tăng tốc.

---

## 4.5 HAI dataset — mô phỏng thuần và LAI với dữ liệu ICU thật

Có **hai** bộ dữ liệu, dùng cùng một script train:

| | `iv_timeseries_1hz.csv` | `iv_hybrid_1hz.csv` ← **nên dùng bộ này** |
|---|---|---|
| HR / SpO2 | Mô phỏng (AR(1)) | **BIDMC — 52 bệnh nhân ICU THẬT** |
| Giọt / cân nặng | Mô phỏng | Mô phỏng (không có bộ công khai có nhãn) |
| Tổng | 72.000 mẫu (20,0 giờ) | 75.036 mẫu (20,8 giờ) |
| Số "ca" | 120 ảo | 156 (52 người thật × 3 lần tiêm biến cố khác nhau) |

**Bộ lai giải quyết đúng lời phê bình "lãng phí dữ liệu".** Tài liệu cũ chỉ dùng
BIDMC để *"hiệu chỉnh dải giá trị bình thường"* — tức là bỏ đi trục thời gian của
chính bộ dữ liệu thật, đúng kiểu lãng phí mà thầy nói. Bộ lai dùng **nguyên chuỗi
1 Hz**: `bidmc_##_Numerics.csv` có sẵn HR/PULSE/RESP/SpO2 lấy mẫu 1 Hz, mỗi bản
ghi 8 phút.

Dải giá trị thật trong bộ lai: **HR 25–194 bpm (trung vị 89)**, **SpO2 74–100%
(trung vị 97)** — rộng và nhiễu hơn nhiều so với bộ mô phỏng, vì bệnh nhân ICU
thật vốn vậy.

**Bẫy phải tránh khi tách tập:** mỗi bản ghi BIDMC được dùng lại 3 lần với biến cố
khác nhau. Nếu tách theo `patient_id` (id từng "ca") thì **cùng một người thật sẽ
xuất hiện ở cả train và test** — model đã thấy trước động lực học HR/SpO2 của người
đó, kết quả đánh giá tốt giả tạo. `train_forecaster.py` vì thế tách theo cột
`bidmc_src` (**bệnh nhân thật**): train 31 / val 10 / test 11 người.

1/53 bản ghi bị loại vì thiếu quá 20% dữ liệu (máy theo dõi mất tín hiệu).

Tải dữ liệu:
```bash
mkdir -p ml/ai_timeseries/dataset/bidmc && cd ml/ai_timeseries/dataset/bidmc
for i in $(seq -w 1 53); do
  curl -fO "https://physionet.org/files/bidmc/1.0.0/bidmc_csv/bidmc_${i}_Numerics.csv"
done          # 53 file, tong 468 KB
```

### 4.5.1 Kết quả trên bộ LAI (dữ liệu ICU thật) — con số nên đem đi trình bày

| Cách quyết định | Báo nhầm (normal) | Báo nhầm (transient) | Recall |
|---|---|---|---|
| A. Ngưỡng tức thời — *cách hiện tại* | **9,6%** | **17,2%** | 34,6% |
| B. Dự báo, quyết định tức thời | 2,2% | 17,6% | 52,0% |
| **C. Dự báo + persistence K=11** | **1,5%** | **2,3%** | **41,4%** |

**Trên dữ liệu thật, kết luận còn mạnh hơn:** phương pháp C tốt hơn ở **cả ba**
chỉ số cùng lúc — báo nhầm trên cửa sổ bình thường giảm từ 9,6% xuống 1,5%
(**giảm 6 lần**), báo nhầm do nhiễu thoáng qua giảm từ 17,2% xuống 2,3%
(**giảm 7 lần**), mà recall vẫn **tăng** (34,6% → 41,4%).

Điểm đáng chú ý: **ngưỡng tức thời báo nhầm 9,6% ngay trên cửa sổ bình thường** —
so với chỉ 0,1% trên bộ mô phỏng. Lý do: bệnh nhân ICU thật có HR/SpO2 dao động ra
ngoài khoảng 45–150 bpm / ≥90% khá thường xuyên mà **không phải** biến cố cần báo.
Đây chính là cơ chế sinh ra alarm fatigue trong thực tế, và chỉ lộ ra khi dùng dữ
liệu thật — bộ mô phỏng đã che mất nó.

Xu hướng trên dữ liệu thật (vùng chết ±10 bpm/phút):

| Nguồn | Đúng 3 nhãn | Đúng hướng |
|---|---|---|
| Từ dự báo của model | **83,5%** | **66,0%** |
| Từ độ dốc 64 giây đã quan sát | 80,7% | **40,1%** |

Phát hiện hồi quy về trung bình **vẫn đúng trên dữ liệu thật**: ngoại suy độ dốc
cho 40,1%, vẫn tệ hơn tung đồng xu.

Recall thấp hơn bộ mô phỏng (41,4% so với 58,3%) là **hợp lý và trung thực**: HR/
SpO2 của bệnh nhân ICU thật khó dự báo hơn nhiều so với chuỗi AR(1) nhân tạo.

---

## 5. Dataset mô phỏng thuần (chi tiết)

`ml/ai_timeseries/make_timeseries_dataset.py` → `dataset/iv_timeseries_1hz.csv`

### 5.1 Khác biệt then chốt so với bộ cũ

Bộ cũ (`iv_vitals_synthetic_labeled.csv`) gán nhãn theo **từng dòng độc lập** →
không thể dùng để dạy/đánh giá một model có trục thời gian.

Bộ mới sinh **quy trình theo thời gian**, và tách rõ **hai loại** bất thường mà
model phải đối xử **khác nhau**:

| Loại | Thời lượng | Nhãn | Model phải |
|---|---|---|---|
| **TRANSIENT** | 2–6 giây | `label_alarm = 0` | **KHÔNG** báo động |
| **SUSTAINED** | ≥ 45 giây | `label_alarm = 1` | **PHẢI** báo động |

**Không có loại thứ nhất thì không có gì để ĐO việc giảm báo nhầm** — đây chính
là thiếu sót khiến model cũ không thể trả lời phê bình của thầy.

### 5.2 Cách sinh tín hiệu

- Dùng **AR(1)** (`x[t] = rho·x[t-1] + noise`) thay vì lấy mẫu độc lập mỗi giây,
  để chuỗi có tương quan thời gian giống sinh lý thật. Lấy mẫu độc lập cho chuỗi
  trắng, model không học được gì về động lực học.
- HR: nền riêng mỗi ca (62–94 bpm) + AR(1) + dao động chậm (chu kỳ 30–120 s)
- SpO2: nền 96–99% + AR(1)
- Giọt: quanh mức bác sĩ đặt + jitter cơ học
- Cân: giảm dần theo lưu lượng (1 g ≈ 1 ml) + nhiễu load cell

**Biến cố kéo dài đều có giai đoạn khởi phát dần (ramp 15–40 s), không nhảy bậc.**
Đây là điều kiện để model dự báo có thể cảnh báo sớm — nếu mọi thứ nhảy bậc tức
thì thì không còn gì để dự báo.

Mức bác sĩ đặt khác nhau giữa các ca (15/18/20/25/30/40/50 dpm; 60/80/100/120/150
ml/h) → model buộc phải học theo **tỉ lệ**, không phải con số tuyệt đối.

### 5.3 Số liệu thực tế

```
tổng mẫu        : 72.000 (20,0 giờ @1Hz)
số bệnh nhân    : 120
label_alarm = 1 : 5.812 (8,1%)
mẫu transient   : 807 (1,1%) — tất cả đều label_alarm = 0
```

| Loại biến cố | Số mẫu |
|---|---|
| normal | 65.381 |
| occlusion (tắc) | 1.384 |
| bradycardia | 1.253 |
| desaturation | 1.178 |
| free_flow (chảy tự do) | 1.019 |
| tachycardia | 978 |
| transient_hr_spike | 232 |
| transient_hr_drop | 156 |
| transient_spo2_dip | 155 |
| transient_drop_miss | 139 |
| transient_drop_burst | 125 |

---

## 6. Huấn luyện

`ml/ai_timeseries/train_forecaster.py`

- **Tách theo BỆNH NHÂN** 60/20/20 (72/24/24 ca), không tách theo dòng → tránh rò
  rỉ dữ liệu, đánh giá trung thực
- **Học không giám sát:** train/val **chỉ** trên cửa sổ động lực học hoàn toàn
  bình thường (không biến cố kéo dài, cũng không nhiễu thoáng qua). Nếu để nhiễu
  vào, model sẽ học coi "nhảy 60 bpm" là bình thường và mất khả năng phát hiện.
- **Loss:** Huber (`delta = 1.0`) — bền với ngoại lai hơn MSE
- Adam `1e-3`, batch 128, EarlyStopping (patience 8), ReduceLROnPlateau
- Nhãn **chỉ dùng để đánh giá**, không dùng để train

Số cửa sổ: **train 23.750 / val 8.144 / test 12.480**

Phân bố tập test: normal 9.411 · transient 1.588 · sustained 1.481
Phân bố tập val: normal 8.976 · transient 2.133 · sustained 1.371

### 6.1 Lượng tử hóa int8

```
IN_SCALE  = 0.011372549459     IN_ZP  = -5
OUT_SCALE = 0.009817149490     OUT_ZP = -15
```

Convert bằng cách clone sang model **batch cố định = 1** rồi copy weights, sau đó
`from_keras_model`. (Đường `from_concrete_functions` thất bại với lỗi
`READ_VARIABLE` vì biến Keras chưa được khởi tạo trong graph.)

---

## 7. Kết quả — độ chính xác dự báo

Tập TEST, chỉ trên cửa sổ bình thường:

| Kênh | MAE toàn horizon | tại +1 giây | tại +16 giây |
|---|---|---|---|
| Nhịp tim | **1,46 bpm** | 0,81 | 2,13 |
| SpO2 | **0,22 %** | 0,13 | 0,28 |
| Tỉ lệ giọt | **0,04** × mức đặt | 0,02 | 0,05 |

Dự báo HR sai trung bình 1,46 bpm cho cả 16 giây phía trước — đủ chính xác để
dùng cho cảnh báo sớm.

---

## 8. Kết quả — chống báo nhầm (phần trả lời trực tiếp phê bình của thầy)

### 8.1 Phương pháp luận

Ba cách quyết định được so sánh trên **cùng** tập test:

- **A. Ngưỡng tức thời** — mô phỏng đúng cách làm hiện tại: chỉ xét mẫu mới nhất
  của cửa sổ, dùng đúng các ngưỡng trong `ai_monitor.h` (HR 45–150, SpO2 ≥ 90,
  tỉ lệ 0,3–1,5)
- **B. Sai số dự báo, quyết định tức thời**
- **C. Sai số dự báo + persistence** (phải vượt ngưỡng liên tục K bước)

**Mọi tham số** (cách tính điểm, ngưỡng, K) được chọn trên tập **VALIDATION**, rồi
báo cáo **một lần** trên tập TEST. Chọn tham số trên chính tập test là tự đánh giá
mình, con số sẽ đẹp giả tạo.

Tiêu chí chọn K **công bằng**: tối đa recall **với điều kiện** báo nhầm trên cửa
sổ transient không tệ hơn phương pháp A. Kết quả chọn: ngưỡng điểm ở phân vị
99,5%, **K = 8**.

Cách tính điểm cũng được chọn trên val (3 phương án thử):

| Cách tính điểm | normal | transient | recall |
|---|---|---|---|
| trung bình thô mọi kênh + thời gian | 2,0% | 25,8% | 73,5% |
| **max theo kênh, TB thời gian** ← chọn | 2,0% | 26,1% | **80,6%** |
| max cả hai trục | 2,0% | 28,4% | 77,6% |

Sai số từng kênh được chia cho độ lệch chuẩn của **chính kênh đó** trên dữ liệu
bình thường trước khi lấy max. Nếu lấy trung bình thô, một ca tụt oxy (chỉ hiện ở
kênh SpO2) bị 3 kênh còn lại pha loãng đến mức gần như vô hình.

### 8.2 Kết quả trên tập TEST

| Cách quyết định | Báo nhầm (normal) | **Báo nhầm (transient)** | **Recall (kéo dài)** |
|---|---|---|---|
| A. Ngưỡng tức thời — *cách hiện tại* | 0,1% | 5,1% | 29,8% |
| B. Dự báo, quyết định tức thời | 2,0% | 27,1% | 72,9% |
| **C. Dự báo + persistence K=8** | 0,3% | **3,4%** | **58,3%** |

**Đọc bảng này:**

- Recall gần **gấp đôi** (29,8% → 58,3%) mà báo nhầm thoáng qua **giảm**
  (5,1% → 3,4%). Tốt hơn ở cả hai chiều, không phải đánh đổi.
- **Dòng B rất quan trọng:** chỉ dùng model mà quyết định tức thời thì báo nhầm
  **tệ hơn nhiều** (27,1%). Vì một cú nhảy 3 giây cũng gây sai số dự báo lớn.
  → **Bước persistence là bắt buộc, không phải tùy chọn.** Có model mà quyết định
  sai cách thì còn tệ hơn không có.

---

## 9. Phát hiện đáng mang đi trình bày: hồi quy về trung bình

So sánh hai cách lấy xu hướng nhịp tim (vùng chết ±10 bpm/phút, tập test):

| Nguồn tín hiệu xu hướng | Đúng 3 nhãn | **Đúng HƯỚNG** |
|---|---|---|
| Từ **dự báo** của model | 70,7% | **72,7%** |
| Từ **độ dốc 64 giây đã quan sát** | 72,1% | **23,7%** |

Đo độ dốc rồi ngoại suy — đúng cách một người giỏi điện tử sẽ làm — cho **23,7%,
tệ hơn cả tung đồng xu**. Lý do: nhịp tim có tính **hồi quy về trung bình** (mean
reversion) — vừa tăng thì có xu hướng tụt lại. Model học được đúng động lực học đó
nên đạt 72,7%.

Đây là phản biện **trực tiếp và định lượng** cho câu "giỏi điện tử là làm được":
cách làm bằng kỹ thuật điện tử thuần không chỉ kém hơn, mà **sai hướng**.

### 9.1 Xu hướng theo các vùng chết khác nhau

| Vùng chết | Đúng 3 nhãn | Đúng hướng | n (thực sự có xu hướng) |
|---|---|---|---|
| ±2 bpm/phút | 46,9% | 64,3% | 7.972 |
| ±5 bpm/phút | 45,2% | 68,3% | 5.216 |
| ±10 bpm/phút | 70,7% | 72,7% | 2.334 |

Vùng chết càng rộng thì càng chính xác — hợp lý, vì xu hướng nhỏ phần lớn là
nhiễu. **Nên dùng ±10 bpm/phút** khi hiển thị cho bác sĩ.

---

## 10. Đo thật trên chip

Nạp firmware có đoạn benchmark, chạy 200 lần suy luận với cửa sổ có biến đổi thật:

```
[TS] Model du bao san sang: 30616 byte, arena dung 2964/8192 byte
[TS] Benchmark: 200/200 lan suy luan OK, tong 954 ms
     -> 4770 us/lan = 0.47% ngan sach chu ky AI (1 giay)
```

| Hạng mục | Số đo |
|---|---|
| Thời gian suy luận | **4,77 ms/lần** |
| Chiếm bao nhiêu chu kỳ AI 1 giây | **0,47%** |
| Arena RAM thực dùng | **2.964 byte** |
| Model trong flash | 30.616 byte |
| Tỉ lệ thành công | 200/200 |

Phép đo ổn định: chạy 20 lần cho 4.700 µs, chạy 200 lần cho 4.770 µs.

**Con chip gần như không phải làm gì — còn dư 99,5% ngân sách thời gian.** Nghĩa
là có thể chạy model to hơn nhiều, chạy dày hơn 1 Hz, hoặc thêm nhánh tái tạo cửa
sổ (AER) để nâng recall, đều gần như miễn phí.

Ban đầu cấp 24 KB arena theo tính toán trên giấy; đo thật chỉ cần 2.964 byte nên
đã hạ xuống 8 KB (vẫn gần 3× dự phòng) → **trả lại 16 KB RAM**.

Kích thước firmware sau khi thêm: `.text` 358.828 · `.data` 1.296 · `.bss` 31.236 byte.

### 10.1 Về việc MVP có thực sự chạy — nói cho chính xác

Kiểm chứng được: kernel MVP **có** được biên dịch vào build
(`mvp1/conv.cc.obj`, `mvp1/fully_connected.cc.obj`), và mọi tensor đều thỏa ràng
buộc tài liệu Silabs (mục 4.4).

Nhưng **chưa chứng minh được bằng đo đạc** rằng MVP đang xử lý từng op, vì SDK sẽ
âm thầm rơi về CMSIS-NN nếu không tăng tốc được. 4,77 ms cho ~91k MAC nghe hơi
chậm nếu MVP chạy đầy đủ — nhiều khả năng **overhead cố định mỗi `Invoke()` đang
chiếm phần lớn** (6 op, ~0,8 ms/op). Với model bé thế này thì điều đó bình thường,
và vì dư 99,5% ngân sách nên không ảnh hưởng thực tế.

Muốn biết chắc: build thêm một bản **tắt** component kernel tăng tốc rồi so sánh
thời gian.

---

## 11. Cách tái lập

```bash
cd ~/SimplicityStudio/v6_workspace/smart-iv-monitor

# Môi trường (một lần)
python3 -m venv .venv-ai
.venv-ai/bin/pip install numpy pandas tensorflow-cpu

cd ai_timeseries

# 1a) Dataset mô phỏng thuần (72.000 mẫu)
../.venv-ai/bin/python make_timeseries_dataset.py

# 1b) Dataset LAI với BIDMC thật (75.036 mẫu) - NÊN DÙNG BỘ NÀY
mkdir -p dataset/bidmc && (cd dataset/bidmc && for i in $(seq -w 1 53); do \
  curl -fsO "https://physionet.org/files/bidmc/1.0.0/bidmc_csv/bidmc_${i}_Numerics.csv"; done)
../.venv-ai/bin/python make_hybrid_dataset.py

# 2) Train + xuất int8 (~1 phút). Mặc định dùng bộ mô phỏng; thêm --csv cho bộ lai:
../.venv-ai/bin/python train_forecaster.py --csv dataset/iv_hybrid_1hz.csv

# 3) Đánh giá (tune trên val, báo cáo trên test)
../.venv-ai/bin/python evaluate.py

# 4) Xuất mảng byte C rồi copy vào gốc project
../.venv-ai/bin/python export_model_header.py
cp out/model_data_ts.h ..

# 5) Build + nạp
cd ..
~/.silabs/slt/installs/archive/slc-cli-v6.0.20/slc_cli/slc generate smart-iv-monitor.slcp -d . -np \
  --sdk-package-path ~/.silabs/slt/installs/conan/p/simpl35774a752829c/p,~/.silabs/slt/installs/conan/p/aiml220b56d6ae053/p \
  --with cli:inst0
cd cmake_gcc/build && ~/.silabs/slt/installs/conan/p/ninja1b9fed093d653/p/ninja
~/.silabs/slt/installs/archive/commander/commander flash smart-iv-monitor.hex --serialno 440364712
```

### 11.1 Danh sách file

| File | Vai trò |
|---|---|
| `ml/ai_timeseries/make_timeseries_dataset.py` | Sinh dataset MÔ PHỎNG THUẦN |
| `ml/ai_timeseries/make_hybrid_dataset.py` | Sinh dataset LAI (HR/SpO2 từ BIDMC thật) ← nên dùng |
| `ml/ai_timeseries/dataset/bidmc/` | 53 file `*_Numerics.csv` tải từ PhysioNet (468 KB) |
| `ml/ai_timeseries/dataset/iv_hybrid_1hz.csv` | Dataset lai (75.036 dòng) |
| `ml/ai_timeseries/train_forecaster.py` | Train CNN dự báo + xuất int8 |
| `ml/ai_timeseries/evaluate.py` | Đánh giá: dự báo, xu hướng, so sánh 3 cách quyết định |
| `ml/ai_timeseries/export_model_header.py` | `.tflite` → mảng byte C |
| `ml/ai_timeseries/dataset/iv_timeseries_1hz.csv` | Dataset (72.000 dòng) |
| `ml/ai_timeseries/out/forecaster_int8.tflite` | Model int8 |
| `ml/ai_timeseries/out/quant_params.txt` | Hằng số quantize + chuẩn hóa |
| `model_data_ts.h` | Model nhúng dạng mảng byte (ở gốc project) |
| `ts_forecaster.cpp` / `.h` | Runner TFLM trên chip |

---

## 12. Tích hợp vào logic báo động — ĐÃ LÀM

Model đã nối vào đường ra quyết định. Cách hoạt động:

### 12.1 Quyết định báo động — 3 tầng (`alert_level_from_result()` trong `app.c`)

| Tầng | Điều kiện | Mức | Qua persistence? |
|---|---|---|---|
| 1 | Mất tín hiệu cảm biến đã lắp, SpO2 < 90, HR ngoài 45–150 | ĐỎ | **Không** — báo ngay |
| 2 | Bất thường đã xác nhận (K=11), hoặc tắc/chảy tự do | ĐỎ | Có |
| 3 | Cảnh báo sớm, HR lệch baseline, autoencoder cũ | VÀNG | — |

Tầng 1 **cố ý không** qua persistence: mất tín hiệu và tụt oxy phải báo tức thì.
Luật lâm sàng vẫn chạy song song — recall 41–58% chưa đủ để bỏ luật cứng.

### 12.2 Các mảnh đã triển khai

- **Cửa sổ 64 giây** nằm trong `ts_monitor.c` (không phải `sensor_hub.c`): 3 kênh
  lưu đã chuẩn hóa, cân nặng lưu **thô** rồi quy đổi tương đối lúc chạy model —
  cách này tránh được lỗi chỉ số đã gặp khi thử lưu sẵn dạng chuẩn hóa.
- **Không đòi hỏi mọi kênh có tín hiệu.** Kênh mất tín hiệu được điền giá trị nền
  và **loại khỏi** điểm bất thường. Nếu bắt buộc cả HR lẫn SpO2 phải `CH_OK` thì
  toàn bộ phần dự báo chết khi chưa gắn cảm biến sinh hiệu — đúng lỗi đã gặp lần
  chạy thử đầu tiên (cửa sổ không bao giờ đầy).
- **Chặn giá trị chuẩn hóa ở ±4** trước khi vào model. Input model bị lượng tử hóa
  int8 (dải biểu diễn ~±1,5), nên so dự báo **đã bị chặn** với giá trị thực tế
  **không chặn** làm điểm bất thường vọt lên hàng nghìn lần ngưỡng. Lỗi cùng loại
  cũng tồn tại trong `ai_monitor.c` (đã sửa: `err = 2147483647` = INT32_MAX).
- **Bitmap dùng 4 bit trống (12–15)** cho: bất thường xác nhận, HR tăng, HR giảm,
  cảnh báo sớm — **không phải đổi schema ZCL**, gateway và server chạy nguyên.
- **Cờ "đáng tin" cho từng kênh dự báo.** Model chỉ học dữ liệu bình thường, nên
  khi một kênh ở trạng thái bất thường kéo dài nó **kéo dự báo về mức bình thường**
  thay vì đi theo xu hướng thật (đo được: giọt tụt 34→27 mà model vẫn báo ~49).
  Giao diện đổi nhãn thành *"expected if normal"* khi cờ này false — nếu không, bác
  sĩ sẽ đọc con số đó như một lời dự báo.

---

## 13. Hạn chế — nói thẳng

1. **Recall 58,3% chưa đủ cho thiết bị y tế thật.** Phải giữ luật lâm sàng tuyệt
   đối song song. Hướng nâng từng đề xuất — thêm nhánh tái tạo cửa sổ (AER,
   arXiv 2212.13558) — **đã thử và ĐO, kết quả là không ăn thua**: xem mục 13.1.
2. **Phần truyền dịch (giọt/cân nặng) vẫn là mô phỏng.** HR/SpO2 đã dùng dữ liệu
   ICU thật (BIDMC, 52 bệnh nhân) nhưng **chưa có bộ dữ liệu công khai có nhãn cho
   truyền dịch IV** — phần này buộc phải mô phỏng, dù được gắn lên chuỗi sinh hiệu
   thật nên động lực học tổng thể có gốc thực.
3. **Dữ liệu thật thu được còn quá ít.** DB có ~2.900 dòng nhưng chỉ ~105 dòng có
   đủ cả 4 kênh cùng lúc, và lưu ở 0,1 Hz (`VitalsSave.IntervalSeconds = 10`)
   trong khi model cần 1 Hz. **Phải tăng tần số lưu trước khi train trên dữ liệu
   thật.**
4. **Chưa xác nhận MVP tăng tốc từng op** (xem 10.1).
5. **Xu hướng chỉ đáng tin ở vùng chết rộng** (±10 bpm/phút → 72,7%). Đừng hiển
   thị mũi tên xu hướng cho biến động nhỏ hơn thế.
6. **Chưa kiểm chứng trên bệnh nhân/giàn thật.** Mọi con số ở đây là trên dataset
   mô phỏng, cộng phép đo thời gian chạy trên chip thật.

### 13.1 Đã thử nhánh tái tạo cửa sổ (AER) — và vì sao KHÔNG dùng

Ý tưởng của AER: model hiện tại chỉ hỏi *"64 giây vừa rồi thì 16 giây tới ra
sao"*, nên bất thường = phần tương lai nó không dự báo nổi. Câu hỏi đó có điểm
mù: tương lai gần của một sinh hiệu bị chi phối bởi **giá trị hiện tại**, nên
một đợt trôi chậm mà model đã "chấp nhận" là mức hiện hành vẫn được dự báo đúng
và bị chấm là bình thường — dù bản thân cửa sổ 64 giây đó đã bất thường. Nhánh
tái tạo hỏi câu bổ sung: *"nén cửa sổ này qua encoder rồi dựng lại được không"*
— một cửa sổ có **hình dạng** lạ thì không dựng lại được, bất kể nó dễ dự báo
tới đâu.

Đã cài đặt đầy đủ trong `ml/ai_timeseries/train_aer.py`: **chung encoder**, thêm
một đầu ra tái tạo, chấm điểm bằng đúng giao thức của `evaluate.py` (chọn tham
số trên tập validation, báo cáo **một lần** trên test, và không cho phép tỉ lệ
báo nhầm trên các cú nháy tệ hơn luật hiện tại).

**Lần 1 — không có bottleneck** (`--bottleneck 0`):

| Cách quyết định | normal (nhầm) | transient (NHẦM) | recall |
|---|---|---|---|
| A. Ngưỡng tức thời (luật hiện tại) | 9,6% | 17,2% | 34,6% |
| B. Chỉ dự báo, K=10 | 1,8% | 2,3% | **45,8%** |
| C. Chỉ tái tạo, K=22 | 5,0% | 6,9% | 14,8% |
| D. AER gộp (α=0,1), K=9 | 1,9% | 2,5% | **47,8%** |

Trông như +2,0 điểm. Nhưng nhánh tái tạo ở đây **không phải autoencoder thật**:
code của encoder là `(64/8)×32 = 256` giá trị, đúng bằng kích thước cửa sổ
`64×4 = 256`, tức **không nén gì cả** — nó học được cách gần như chép lại, mà
một cái máy chép thì dựng lại cửa sổ bất thường cũng ngon lành như cửa sổ bình
thường. Đúng như đo được: một mình nó chỉ đạt 14,8%.

**Lần 2 — ép qua bottleneck 16 chiều** (`--bottleneck 16`), đây mới là phép thử
công bằng:

| Cách quyết định | normal | transient | recall |
|---|---|---|---|
| B. Chỉ dự báo, K=11 | 1,4% | 2,2% | **39,2%** |
| C. Chỉ tái tạo, K=30 | 4,8% | 4,7% | 14,5% |
| D. AER gộp (α=0,2), K=10 | 1,8% | 1,9% | **39,4%** |

**Kết luận: không dùng.** Ba lý do, theo thứ tự quan trọng:

1. **Mức "cải thiện" nhỏ hơn nhiễu giữa các lần chạy.** Chỉ riêng cột "chỉ dự
   báo" đã dao động 45,8% → 39,2% giữa hai lần huấn luyện (**6,6 điểm**), trong
   khi phần AER thêm vào được 2,0 và 0,2 điểm. Không thể tuyên bố một mức tăng
   nhỏ hơn độ dao động của chính phép đo.
2. **Quét trọng số cho thấy tái tạo càng nặng càng tệ**: α = 0,0 → 1,0 làm recall
   trên validation rơi 57,5% → 35,2%. Nếu nhánh này thực sự bổ sung thông tin,
   đường cong đã không đơn điệu giảm như vậy.
3. **Giá phải trả là thật**: model từ 30.616 lên 63.064 byte (+32 KB flash), thêm
   một tensor đầu ra phải đọc và 256 phép tính sai số mỗi giây trên chip — đổi
   lấy 0,2 điểm.

Giữ lại `train_aer.py` trong repo để bất kỳ ai muốn kiểm chứng lại đều chạy được
`--eval-only` mà không cần huấn luyện lại. Hướng đáng đầu tư hơn cho recall là
**dữ liệu**, không phải kiến trúc: xem hạn chế 2 và 3 ngay trên.

---

## 14. Nguồn

**Phần cứng** (kiểm chứng cục bộ trong SDK + tài liệu Silabs)
- MVP Accelerator, danh sách kernel và ràng buộc tensor:
  https://docs.silabs.com/machine-learning/latest/aiml-fundamentals/mvp-accelerator
- TFLM cho vi điều khiển (Silabs):
  https://docs.silabs.com/machine-learning/1.3.0/machine-learning-tensorflow-lite-for-microcontrollers/

**1D-CNN so với LSTM trên MCU**
- *Rethinking Temporal Models for TinyML: LSTM versus 1D-CNN in Resource-Constrained
  Devices*, arXiv 2603.04860 — https://arxiv.org/abs/2603.04860
- *Memory-Efficient CNN Autoencoder for Real-Time ECG Anomaly Detection on
  TinyML-Enabled Edge Devices*, MDPI Future Internet 18(6):286 —
  https://www.mdpi.com/1999-5903/18/6/286
- *TinyAD: Memory-efficient anomaly detection for time series data in Industrial
  IoT*, arXiv 2303.03611 — https://arxiv.org/pdf/2303.03611

**Báo động giả / alarm fatigue** (cơ sở cho bước persistence)
- *Classification of Methods to Reduce Clinical Alarm Signals for Remote Patient
  Monitoring: A Critical Review*, arXiv 2302.03885 — https://arxiv.org/pdf/2302.03885
- *Computational approaches to alleviate alarm fatigue in intensive care medicine*,
  Frontiers in Digital Health —
  https://www.frontiersin.org/journals/digital-health/articles/10.3389/fdgth.2022.843747/full
- *Insights into the Problem of Alarm Fatigue with Physiologic Monitor Devices*,
  PLOS One — https://journals.plos.org/plosone/article?id=10.1371%2Fjournal.pone.0110274

Các nghiên cứu trên cho con số: độ trễ xác nhận 5 giây giảm ~49% báo động giả,
15 giây giảm 60–70%; và **100% biến cố VT kéo dài ≥ 30 giây đều đe dọa tính
mạng** — nghĩa là persistence vài chục giây gần như không làm mất ca thật.

**Autoencoder + regression cho chuỗi thời gian**
- *AER: Auto-Encoder with Regression for Time Series Anomaly Detection*,
  arXiv 2212.13558 — https://arxiv.org/pdf/2212.13558

**Phát hiện tắc đường truyền bằng xu hướng/độ lệch chuẩn**
- Infusion Pump Pressure Sensing Alarms — thuật toán hồi quy và 2·SD, và trễ báo
  động tới 2 giờ ở lưu lượng thấp với ngưỡng truyền thống:
  https://www.ivteam.com/intravenous-literature/infusion-pump-pressure-sensing-alarms/
