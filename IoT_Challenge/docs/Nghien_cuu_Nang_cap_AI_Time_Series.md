# Nghiên cứu: nâng cấp AI Smart IV từ "ảnh tức thời" sang time series

Tài liệu này trả lời nhận xét của thầy: AI hiện tại *"giống như set ngưỡng, giỏi
điện tử là làm được"*, *"lãng phí dữ liệu tươi thu thập được"*, và *"nhịp tim/giọt
nhảy bất thường vài giây rồi bình thường lại thì báo nhầm, gây khó chịu cho bác sĩ"*.

Mọi con số về phần cứng dưới đây đã được **kiểm chứng trực tiếp** trong SDK đang
dùng (`aiml 2.2.2` / `simplicity_sdk 2025.12.3`), không phải suy đoán.

---

## 1. Chẩn đoán: vì sao nhận xét của thầy là đúng

Đọc lại chính xác model đang chạy (`ai_monitor.c`, `model_runner.cpp`, `model_data.h`):

| Hạng mục | Thực tế hiện tại |
|---|---|
| Đầu vào | **6 số tại MỘT thời điểm** duy nhất |
| Kiến trúc | Autoencoder `6→4→2→4→6`, **chỉ 1 loại op: FULLY_CONNECTED** |
| Kích thước | 3.272 byte, arena 8 KB |
| Quyết định | `MSE(input, reconstruct) > 1.4336` **OR** các luật ngưỡng cứng |

**Vấn đề gốc: model không có trục thời gian.** Nó là một hàm chỉ phụ thuộc vào
thời điểm hiện tại. Autoencoder với nút thắt 2 chiều trên 6 đặc trưng tức thời
thực chất học một *mặt cong bao quanh vùng "bình thường"*, và sai số tái tạo
chính là **khoảng cách tới vùng đó**. Về mặt toán học, đây là một **ngưỡng đa
biến phi tuyến** — nên nhận xét "giỏi điện tử là làm được" là công bằng: một bộ
ngưỡng theo từng kênh cộng ngưỡng theo tỉ lệ, tinh chỉnh tốt, sẽ cho kết quả xấp xỉ.

**Hệ quả trực tiếp — đúng như thầy nói về báo nhầm:** vì không có ký ức, hai tình
huống sau **giống hệt nhau** dưới mắt model:

- HR = 130 trong **2 giây** (bệnh nhân trở người, nhiễu PPG, chạm tay)
- HR = 130 trong **10 phút** (nhịp nhanh thật, cần can thiệp)

Model buộc phải chọn: báo cả hai (→ báo nhầm liên tục, bác sĩ mất tin) hoặc không
báo cả hai (→ bỏ sót ca thật). Không có lựa chọn thứ ba. Đây không phải lỗi tinh
chỉnh ngưỡng, mà là **giới hạn của kiến trúc**.

**Và đúng là đang lãng phí dữ liệu.** Firmware chạy AI **1 lần/giây**, nhưng mỗi
tick được đánh giá độc lập rồi **bỏ đi**. Toàn bộ thông tin nằm trong *cách các
giá trị biến đổi* — xu hướng, độ biến thiên, thời gian kéo dài, tương quan theo
thời gian giữa các kênh — chưa được dùng một chút nào.

> **Một điểm cần đính chính:** tài liệu `Dataset_va_Phuong_phap_AI_SmartIV.md` mô
> tả pipeline huấn luyện bằng Keras + TFLite (`train_autoencoder_pct.py`), và
> trong repo **không có dấu vết nào của MLTK** (Silicon Labs Machine Learning
> Toolkit). Cái đang dùng trên chip là **TensorFlow Lite Micro** (bản Silabs đóng
> gói trong extension `aiml`). Nên trình bày với thầy là "TFLM trên EFR32 có MVP
> accelerator" để tránh bị hỏi lại.

---

## 2. Time series mở ra được gì mà ngưỡng không làm được

Bốn nhóm thông tin chỉ tồn tại khi có cửa sổ thời gian:

**a) Thời lượng (persistence)** — phân biệt thoáng qua với kéo dài. Đây là chìa
khóa giải quyết đúng vấn đề thầy nêu.

**b) Xu hướng / độ dốc (trend, slope)** — bịch dịch đang cạn *nhanh dần* báo hiệu
chảy tự do **trước khi** tỉ lệ vượt ngưỡng. Y văn về bơm truyền dịch cho thấy
thuật toán ngưỡng áp suất truyền thống ở lưu lượng thấp (≤ 1 ml/h) có thể **trễ
báo động tới 2 giờ**, trong khi thuật toán dùng độ dốc / độ lệch chuẩn giảm trễ
đáng kể. Ngưỡng tức thời **không thể** biết "đang cạn nhanh dần".

**c) Độ biến thiên (variability)** — đường truyền bắt đầu tắc thì nhịp giọt trở
nên **không đều trước khi trở nên chậm**. Độ lệch chuẩn của khoảng cách giữa các
giọt bắt được điều này sớm hơn giá trị giọt/phút trung bình. Tương tự, HRV (biến
thiên nhịp tim) là chỉ dấu lâm sàng thật, và **không thể** tính từ một thời điểm.

**d) Tương quan theo thời gian giữa các kênh** — HR tăng dần *đồng thời* SpO2 giảm
dần trong 5 phút là hình mẫu lâm sàng có ý nghĩa, dù từng kênh riêng lẻ vẫn nằm
trong ngưỡng an toàn. Đây chính là loại "bất thường tổ hợp" mà autoencoder đáng
lẽ giỏi — nhưng chỉ khi được thấy chuỗi thời gian.

---

## 3. Ràng buộc phần cứng — đã kiểm chứng, quyết định kiến trúc

Đây là phần quan trọng nhất và cũng dễ chọn sai nhất.

**Kernel được MVP tăng tốc** (kiểm bằng `find` trong `aiml/src/kernels/mvp1`):

```
add.cc  conv.cc  depthwise_conv.cc  fully_connected.cc
mul.cc  pooling.cc  transpose_conv.cc
```

→ `CONV_2D`, `DEPTHWISE_CONV_2D`, `FULLY_CONNECTED`, pooling, add, mul **được
tăng tốc bằng phần cứng**.

**Op có trong TFLM nhưng KHÔNG được MVP tăng tốc** (kiểm trong
`micro_mutable_op_resolver.h`): `UnidirectionalSequenceLSTM`, `Svdf`,
`CircularBuffer` — có sẵn, chạy được, nhưng **chạy trên CPU M33**.

**Ba ràng buộc phải nhớ:**
1. **Dilation KHÔNG được MVP hỗ trợ** → TCN cổ điển (dilated convolution) sẽ bị
   rơi về kernel chậm. Muốn nhìn xa theo thời gian thì dùng **stride + pooling
   xếp tầng**, đừng dùng dilation.
2. `width × channels ≤ 2047`, mọi chiều `≤ 1024`.
3. Nên dùng **số kênh chẵn** để tăng khả năng được tăng tốc.

**Kết luận kiến trúc:** dùng **1D CNN trên cửa sổ trượt** (cài như `Conv2D` với
height = 1), **không dùng LSTM**. Điều này khớp với y văn TinyML: 1D-CNN đạt độ
chính xác *ngang hoặc cao hơn* LSTM (~95%) trên nhiều benchmark, với chi phí bộ
nhớ/tính toán thấp hơn nhiều trên MCU. Ở đây còn có thêm lý do riêng: **CNN được
MVP tăng tốc, LSTM thì không.** Chọn LSTM là tự bỏ phần cứng tăng tốc mà chip này
có sẵn.

Depthwise separable convolution giảm được 50–90% kích thước model so với CNN
thường — và `depthwise_conv` nằm trong danh sách được tăng tốc.

---

## 4. Kiến trúc đề xuất

### 4.1 Phương án chính: Conv1D Autoencoder trên cửa sổ

Giữ nguyên triết lý **học không giám sát, chỉ train trên dữ liệu bình thường** —
nên toàn bộ lập luận và danh mục trích dẫn hiện có trong
`Dataset_va_Phuong_phap_AI_SmartIV.md` **vẫn dùng được**, chỉ đổi đơn vị đầu vào:

| | Hiện tại | Đề xuất |
|---|---|---|
| Đầu vào | 6 đặc trưng × **1 thời điểm** | 6 kênh × **cửa sổ 60 giây** (60 mẫu @1Hz) |
| Kiến trúc | FC `6→4→2→4→6` | Conv1D/depthwise-separable + pooling → bottleneck → giải mã lại cửa sổ |
| Bất thường | MSE tại 1 điểm | MSE trên **cả cửa sổ** |

**Vì sao cách này tự động dập báo nhầm thoáng qua:** một cú nhảy 2 giây chỉ chiếm
2/60 mẫu, làm MSE cửa sổ nhích lên rất ít → dưới ngưỡng. Một biến cố kéo dài
chiếm phần lớn cửa sổ → MSE tăng mạnh → báo động. **Việc phân biệt thoáng qua /
kéo dài trở thành tính chất nội tại của model**, không phải một bộ lọc chắp thêm.

### 4.2 Phương án mở rộng: thêm nhánh dự báo (forecasting)

Dự đoán giá trị kế tiếp từ cửa sổ quá khứ, bất thường = sai số dự báo. Ưu điểm
lớn là **giải thích được cho bác sĩ**: *"dự kiến 20 giọt/phút, thực tế 4"* — dễ
tin hơn nhiều so với "sai số tái tạo 1.87". Hướng kết hợp cả hai (autoencoder +
regression) đã có công bố (AER, arXiv 2212.13558).

Đề xuất: làm 4.1 trước, 4.2 là bước sau.

### 4.3 Bổ sung đặc trưng cửa sổ tường minh (rẻ, nên làm ngay)

Không cần chờ model mới, tính ngay trên chip từ cửa sổ:

- **Độ dốc** của cân nặng (g/phút) → phát hiện cạn nhanh dần
- **Độ lệch chuẩn** khoảng cách giữa các giọt → tắc sớm (mục 2c)
- **Thời gian liên tục** vượt ngưỡng của từng kênh
- **Biên độ dao động** HR trong cửa sổ (đại diện HRV thô)

Đây là các đặc trưng **có ý nghĩa lâm sàng và giải thích được** — quan trọng với
thiết bị y tế, và cũng là câu trả lời trực tiếp cho "đang lãng phí dữ liệu tươi".

### 4.4 Nâng luật lâm sàng từ tức thời lên có thời gian

Nên **giữ** lớp luật lâm sàng (SpO2 < 90% phải là ngưỡng tuyệt đối, không để
model tự học), nhưng đổi từ "đúng tại thời điểm" sang **"đúng liên tục trong N
giây"**. Y văn về alarm fatigue cho các con số rất thuyết phục:

| Độ trễ xác nhận | Giảm báo động giả |
|---|---|
| 2 giây | ~25% |
| 5 giây | ~49% |
| 14 giây | ~50% |
| 15 giây | ~60–70% |

Trong khi đó, với biến cố nguy hiểm thật (VT), **100% các ca kéo dài ≥ 30 giây
đều là biến cố đe dọa tính mạng** — nghĩa là độ trễ vài chục giây gần như không
làm mất ca thật. Bối cảnh: ICU trung bình **43 báo động/giờ**, **42,5% không được
phản hồi** — đó chính là "sự khó chịu của bác sĩ" mà thầy nói.

**Lưu ý an toàn:** phải **miễn trừ** một số điều kiện khỏi độ trễ. Mất tín hiệu
cảm biến và SpO2 tụt sâu nên báo ngay; độ trễ chỉ áp cho các điều kiện mà thoáng
qua là vô hại.

---

## 5. Vấn đề dữ liệu — nút thắt thật, cần biết trước khi bắt tay

Kiểm tra DB hiện tại (`vital_samples`, giường BED-101):

| | Số dòng |
|---|---|
| Tổng | 2.871 |
| **Có đủ cả 4 kênh cùng lúc** | **105** |

**105 mẫu là quá ít để huấn luyện model cửa sổ.** Với cửa sổ 60 giây, 105 mẫu
rời rạc (cách nhau 10 giây) thậm chí không dựng nổi vài chục cửa sổ độc lập. Đây
là việc phải giải quyết trước, nếu không sẽ train ra model vô nghĩa.

**Ba việc cần làm:**

1. **Tăng tần số lưu.** `VitalsSave.IntervalSeconds = 10` → dữ liệu chỉ có 0,1 Hz,
   trong khi chip chạy AI ở 1 Hz. Muốn train cửa sổ 1 Hz thì phải lưu 1 Hz, ít
   nhất trong giai đoạn thu thập. Cần cân nhắc dung lượng: 1 Hz ≈ 86.400
   dòng/giường/ngày (so với 8.640 hiện tại).

2. **Dùng BIDMC như time series thật — hiện đang bị lãng phí đúng theo cách thầy
   phê bình.** Tài liệu hiện tại viết BIDMC chỉ dùng để *"hiệu chỉnh dải giá trị
   bình thường"*. Nhưng BIDMC vốn là **53 bản ghi ICU thật, 8 phút mỗi bản, lấy
   mẫu 1 Hz** — tức là đã sẵn sàng cho huấn luyện cửa sổ HR/SpO2. Khai thác trục
   thời gian của BIDMC vừa giải quyết được phần lớn nhu cầu dữ liệu, vừa là một
   lập luận mạnh: *"chúng em dùng dữ liệu ICU thật dạng chuỗi thời gian, không chỉ
   lấy dải giá trị"*.

3. **Sinh lại dataset mô phỏng phần IV có động lực học theo thời gian.** Bộ hiện
   tại gán nhãn theo *từng dòng*. Để chứng minh được việc dập báo nhầm thoáng qua,
   dataset **phải có cả hai loại nhãn**: nhiễu thoáng qua (phải KHÔNG báo) và biến
   cố kéo dài (phải báo). Không có loại thứ nhất thì không có gì để đo cải thiện.

---

## 6. Chạy ở đâu: chip hay server

Giờ đã có lịch sử trên server, có thể chạy model nặng hơn ở đó. Nhưng **thiết bị
phải tự báo động được khi mất mạng** — đây là yêu cầu an toàn, không thương lượng
(và hệ thống vừa chứng minh điều này rất thật: Pi đổi IP là đứt dữ liệu 29 giờ).

Đề xuất phân tầng:
- **Trên chip:** model cửa sổ nhỏ (Conv1D-AE) + luật có thời gian → điều khiển
  LED/còi tại giường. Tự chủ hoàn toàn.
- **Trên server:** model lớn hơn cho phân tích xu hướng dài hạn, dự báo, hiển thị
  trên dashboard. Không nằm trên đường an toàn tính mạng.

---

## 7. Lộ trình đề xuất

| Bước | Việc | Ghi chú |
|---|---|---|
| 1 | Tăng tần số lưu lên 1 Hz để thu dữ liệu | Chặn mọi việc sau |
| 2 | Luật lâm sàng có thời gian (persistence) | Rẻ, hiệu quả ngay, giảm 50–70% báo nhầm |
| 3 | Đặc trưng cửa sổ tường minh (độ dốc, SD, thời lượng) | Không cần train lại |
| 4 | Khai thác BIDMC dạng chuỗi + sinh lại dataset IV có transient | Nền cho bước 5 |
| 5 | Train Conv1D-AE trên cửa sổ, xuất int8, thay model trên chip | Cần sửa `MicroMutableOpResolver<1>` → thêm CONV_2D/pooling |
| 6 | Thêm nhánh dự báo để giải thích cho bác sĩ | Mở rộng |

Bước 2 và 3 đã đủ để trả lời trực tiếp phê bình của thầy về báo nhầm, và **không
cần huấn luyện lại gì cả** — nên làm trước để có kết quả sớm.

---

## 8. Nguồn

**Phần cứng / triển khai** (kiểm chứng cục bộ trong SDK + tài liệu Silabs)
- MVP Accelerator, danh sách kernel được tăng tốc và ràng buộc kích thước tensor:
  https://docs.silabs.com/machine-learning/latest/aiml-fundamentals/mvp-accelerator
- TensorFlow Lite for Microcontrollers (Silabs):
  https://docs.silabs.com/machine-learning/1.3.0/machine-learning-tensorflow-lite-for-microcontrollers/

**1D-CNN so với LSTM trên MCU**
- *Rethinking Temporal Models for TinyML: LSTM versus 1D-CNN in Resource-Constrained
  Devices*, arXiv 2603.04860: https://arxiv.org/abs/2603.04860
- *Memory-Efficient CNN Autoencoder for Real-Time ECG Anomaly Detection on
  TinyML-Enabled Edge Devices*, MDPI Future Internet 18(6):286:
  https://www.mdpi.com/1999-5903/18/6/286
- *TinyAD: Memory-efficient anomaly detection for time series data in Industrial
  IoT*, arXiv 2303.03611: https://arxiv.org/pdf/2303.03611
- *Machine Learning for Microcontroller-Class Hardware: A Review*, arXiv 2205.14550:
  https://arxiv.org/pdf/2205.14550

**Báo động giả / alarm fatigue (dùng cho lập luận về độ trễ xác nhận)**
- *Classification of Methods to Reduce Clinical Alarm Signals for Remote Patient
  Monitoring: A Critical Review*, arXiv 2302.03885: https://arxiv.org/pdf/2302.03885
- *Computational approaches to alleviate alarm fatigue in intensive care medicine:
  a systematic literature review*, Frontiers in Digital Health:
  https://www.frontiersin.org/journals/digital-health/articles/10.3389/fdgth.2022.843747/full
- *Insights into the Problem of Alarm Fatigue with Physiologic Monitor Devices*,
  PLOS One: https://journals.plos.org/plosone/article?id=10.1371%2Fjournal.pone.0110274
- *The effect of intelligent management interventions in intensive care units to
  reduce false alarms: an integrative review*, ScienceDirect:
  https://www.sciencedirect.com/science/article/pii/S2352013223001503

**Phát hiện tắc đường truyền bằng xu hướng/độ lệch chuẩn**
- Infusion Pump Pressure Sensing Alarms (tổng hợp y văn, gồm thuật toán hồi quy
  và thuật toán 2·SD, và trễ báo động tới 2 giờ ở lưu lượng thấp):
  https://www.ivteam.com/intravenous-literature/infusion-pump-pressure-sensing-alarms/

**Autoencoder + regression cho phát hiện bất thường chuỗi thời gian**
- *AER: Auto-Encoder with Regression for Time Series Anomaly Detection*,
  arXiv 2212.13558: https://arxiv.org/pdf/2212.13558
