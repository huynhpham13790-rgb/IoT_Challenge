# Smart IV & Patient Safety — Bộ dữ liệu, đặc trưng và phương pháp AI

Tài liệu này mô tả đầy đủ phần AI phát hiện bất thường của thiết bị: dùng những
thông số gì, thông số nào do bác sĩ đặt, cách lấy baseline nhịp tim cá nhân, tại
sao chọn cách huấn luyện autoencoder, và các bài báo/nguồn chứng minh dữ liệu là
thật cũng như phương pháp là đúng chuẩn.

---

## 1. Tổng quan

Thiết bị giám sát bệnh nhân truyền dịch (IV) và cảnh báo an toàn. Phần AI nhận
**6 đặc trưng** mỗi giây, chạy một **autoencoder** (mô hình học sâu nén–giải nén)
kết hợp với **luật lâm sàng**, và báo động khi phát hiện bất thường.

Nguyên tắc thiết kế: mỗi đặc trưng phải khớp với **một cảm biến thật**. Kênh nào
chưa nối cảm biến thì đánh dấu "chưa có" (DISABLED) và **không báo động nhầm**;
kênh đã nối mà mất tín hiệu (LOST) thì báo mất tín hiệu.

---

## 2. Sáu đặc trưng đầu vào

| # | Đặc trưng | Ý nghĩa | Cảm biến | Dải bình thường |
|---|-----------|---------|----------|-----------------|
| 0 | `heart_rate` | Nhịp tim (bpm) | MAX30102 (PPG) | 70–95 |
| 1 | `spo2` | Bão hòa oxy máu (%) | MAX30102 | 95–99 |
| 2 | `flow_ratio` | Lưu lượng thật / mức bác sĩ đặt | Loadcell / cảm biến lưu lượng | ≈ 1.0 |
| 3 | `drops_ratio` | Số giọt/phút thật / mức bác sĩ đặt | Cảm biến giọt (quang) | ≈ 1.0 |
| 4 | `vital_missing` | Cờ: mất tín hiệu sinh hiệu (MAX30102 hỏng → mất HR/SpO2) | (suy ra) | 0 |
| 5 | `line_missing` | Cờ: mất tín hiệu đường truyền (mất cảm biến giọt/lưu lượng) | (suy ra) | 0 |

- 4 đặc trưng đầu là **số liên tục**, được **chuẩn hóa** (StandardScaler) trước khi
  đưa vào mô hình.
- 2 đặc trưng cuối là **cờ 0/1**, giữ nguyên (không chuẩn hóa).

`flow_ratio` và `drops_ratio` là **tỉ lệ so với mức đặt**, không phải giá trị thô.
Nhờ vậy mô hình không phụ thuộc tốc độ truyền cụ thể của từng bệnh nhân: chạy đúng
mức đặt ⇒ tỉ lệ ≈ 1.0 (100%), bất kể bác sĩ kê 60 hay 150 mL/giờ.

### Ghi chú lịch sử

Bản đầu dùng **8 đặc trưng**, sau rút xuống **6** để khớp đúng cảm biến phần cứng
đo được. Hai đặc trưng bị bỏ là `pulse_rate` (nhịp mạch từ PPG) và `resp_rate`
(nhịp thở). **Dataset chưa từng có nhiệt độ** — nếu muốn thêm phải gắn cảm biến
nhiệt thật và lấy phân bố từ nguồn có nhiệt độ (MIMIC-III/IV, eICU), xem mục 9.

---

## 3. Thông số do BÁC SĨ đặt (doctor-set)

Khai báo trong `sensor_hub.h`, chỉnh theo đơn kê của từng bệnh nhân:

| Tham số | Giá trị mặc định | Ý nghĩa |
|---------|------------------|---------|
| `SET_FLOW_ML_H` | 100.0 | Tốc độ truyền đặt (mL/giờ) |
| `SET_DROPS_DPM` | 20.0 | Số giọt/phút đặt |

Đây là "mức chuẩn" để tính tỉ lệ: `flow_ratio = lưu_lượng_thật / SET_FLOW_ML_H`,
`drops_ratio = giọt_phút_thật / SET_DROPS_DPM`. Khi test trên giàn thực tế, phải
đặt hai giá trị này bằng đúng tốc độ mà giàn của bạn chạy ở trạng thái bình thường,
nếu không tỉ lệ sẽ lệch xa 100% và báo động (đúng logic nhưng sai mức đặt).

---

## 4. Ba trạng thái mỗi kênh (mấu chốt để không báo nhầm)

| Trạng thái | Ý nghĩa | Cờ missing | Báo động? |
|------------|---------|------------|-----------|
| `CH_DISABLED` | Chưa nối cảm biến | 0 | Không (điền giá trị nền trung tính) |
| `CH_OK` | Có dữ liệu tươi | 0 | Theo luật + autoencoder |
| `CH_LOST` | Đã nối nhưng mất tín hiệu | 1 | **Có** (mất tín hiệu) |

Kênh `CH_DISABLED` được điền **giá trị nền** = trung bình của scaler → sau chuẩn hóa
≈ 0 → autoencoder tái tạo gần đúng → không đẩy sai số lên → **không báo nhầm**.
Chỉ khi kênh đã bật mà mất tín hiệu (`CH_LOST`) mới bật cờ `vital_missing`/
`line_missing` và báo động.

---

## 5. Baseline nhịp tim cá nhân — cách lấy và vì sao

Nhịp tim bình thường khác nhau theo người (trẻ em, người già, người tập luyện).
Nếu dùng một ngưỡng cứng cho mọi người sẽ báo nhầm. Vì vậy HR được so với
**baseline riêng của từng bệnh nhân**.

**Khi huấn luyện:** baseline mỗi bệnh nhân = **median (trung vị) của HR trên các
dòng bình thường** của chính bệnh nhân đó (`label == 0`). Dùng median thay vì mean
để không bị nhiễu/ngoại lai kéo lệch.

**Trên thiết bị thật:** khi vừa gắn máy, firmware lấy **median HR trong ~60 giây
đầu** làm baseline cá nhân (cửa sổ hiệu chuẩn `HR_CALIB_MS = 60000`). Nếu chưa có
(kênh HR chưa bật), baseline mặc định = trung bình trong `scaler.json` (81.68).

**Luật HR:** báo khi lệch quá **30%** so baseline cá nhân (`AI_HR_PCT = 0.30`),
hoặc vượt lưới an toàn tuyệt đối `HR < 45` / `HR > 150` (phòng khi baseline hỏng).

---

## 6. Bộ dữ liệu: input là gì, output là gì để train

Dataset gồm **hai phần**:

### 6.1. Phần THẬT — HR/SpO2/nhịp thở: BIDMC (PhysioNet)

- **BIDMC PPG and Respiration Dataset**, PhysioNet — 53 bản ghi bệnh nhân ICU thật
  tại Beth Israel Deaconess Medical Center (Boston), trích từ MIMIC II, mỗi bản 8
  phút, lấy mẫu 1 Hz: HR, PULSE, RESP, SpO2.
- Link: https://physionet.org/content/bidmc/1.0.0/
- Dùng để **hiệu chỉnh dải giá trị bình thường** cho phần mô phỏng (HR ~70–95,
  SpO2 ~95–99, RESP ~14–26).

### 6.2. Phần MÔ PHỎNG có nhãn — cột truyền dịch IV

- File `dataset/iv_vitals_synthetic_labeled.csv`: 24.000 dòng, 40 bệnh nhân ảo,
  10 phút/người, 1 Hz. Sinh bằng `dataset/make_synthetic.py`.
- **Lý do phải mô phỏng:** không có bộ dữ liệu công khai **có gán nhãn** cho tốc độ
  truyền dịch IV. Đây là phần mới của đề tài, không phải điểm yếu.
- Dải giá trị bình thường được calibrate theo đúng phân bố BIDMC thật.
- **Sáu loại bất thường có nhãn** (~6% số dòng): `iv_occlusion` (tắc), `iv_free_flow`
  (chảy tự do), `desaturation` (tụt SpO2), `tachycardia` (tim nhanh), `bradycardia`
  (tim chậm), `sensor_dropout` (mất tín hiệu cảm biến).

### 6.3. Input / Output khi huấn luyện autoencoder

- **Input:** vector 6 đặc trưng (4 số đã chuẩn hóa + 2 cờ 0/1).
- **Output:** **chính 6 đặc trưng đó được tái tạo lại** (autoencoder học `X → X`).
- **Hàm mất mát:** MSE giữa input và bản tái tạo.
- **Nhãn để train:** KHÔNG dùng nhãn — chỉ train trên các dòng **bình thường**
  (`label == 0`, đã lọc sạch HR 60–110, SpO2 ≥ 93). Cột `label`/`anomaly_type`
  **chỉ dùng để đánh giá** (precision/recall/ROC) trên tập test.

Nói gọn: *input = 6 chỉ số sinh hiệu + cờ mất tín hiệu; output = tái tạo lại chính
6 chỉ số đó; huấn luyện chỉ trên dữ liệu bình thường (học không giám sát).*

---

## 7. Vì sao huấn luyện theo cách này (autoencoder + luật lai)

**Ý tưởng autoencoder cho phát hiện bất thường:** cho mô hình học nén rồi bung lại
dữ liệu **bình thường**. Khi gặp dữ liệu lạ (bất thường), mô hình tái tạo sai nhiều
⇒ **sai số tái tạo (reconstruction error)** vượt ngưỡng ⇒ báo động. Ưu điểm: chỉ
cần dữ liệu bình thường để học, và bắt được cả những bất thường **tổ hợp** mà luật
cứng bỏ sót.

**Kiến trúc:** `6 → 4 → 2 → 4 → 6` (nút thắt cổ chai 2 chiều), MSE, tối đa 120 epoch,
EarlyStopping.

**Tách dữ liệu theo BỆNH NHÂN** 60/20/20 (train/val/test) — không để một bệnh nhân
xuất hiện ở cả train lẫn test, tránh rò rỉ dữ liệu, đánh giá trung thực hơn.

**Đặt ngưỡng:** lấy phân vị của sai số trên tập validation-bình-thường theo tỉ lệ
báo nhầm mục tiêu (`FP_TARGET = 2%`). Ngưỡng chốt: `AI_AE_THRESHOLD ≈ 1.4336`.

**Vì sao thêm LUẬT LÂM SÀNG (kết hợp OR):** một số ngưỡng phải theo chuẩn y khoa
tuyệt đối, không nên để mô hình "tự học":
- **SpO2 dùng ngưỡng tuyệt đối < 90%** — 88% là nguy hiểm với *mọi* bệnh nhân.
- **HR dùng % so baseline cá nhân** — chuẩn hóa theo người già/trẻ nhỏ.
- **Đường truyền:** tỉ lệ flow/drops ngoài `[0.3, 1.5]×` mức đặt → tắc hoặc chảy tự do.
- **Mất tín hiệu:** cờ missing → báo thẳng.

Báo động cuối = **OR** của (autoencoder vượt ngưỡng) HOẶC (bất kỳ luật nào đúng) —
ưu tiên **recall cao** (không bỏ sót ca nguy hiểm).

### Tham số luật (chốt trong `threshold.json` / `ai_monitor.h`)

| Tham số | Giá trị | Ý nghĩa |
|---------|---------|---------|
| `AI_HR_PCT` | 0.30 | HR lệch > 30% baseline cá nhân → nghi ngờ |
| `AI_HR_ABS_LOW` | 45 | Lưới an toàn dưới (bradycardia nặng) |
| `AI_HR_ABS_HIGH` | 150 | Lưới an toàn trên (tachycardia nặng) |
| `AI_SPO2_ABS` | 90 | SpO2 < 90% → tụt oxy (tuyệt đối) |
| `AI_FLOW_HI / LO` | 1.5 / 0.3 | Tỉ lệ flow/drops ngoài khoảng → tắc/chảy tự do |
| `AI_AE_THRESHOLD` | 1.4336 | Ngưỡng sai số tái tạo của autoencoder |

**Kết quả trên tập test:** Recall = 100% (bắt hết mọi loại bất thường), tỉ lệ báo
nhầm (FP) ≈ 3.4%.

---

## 8. Bằng chứng: dữ liệu là thật + phương pháp là đúng chuẩn

### 8.1. Chứng minh BIDMC là dữ liệu THẬT (có bài báo gốc, bình duyệt)

- **Pimentel M.A.F. et al. (2016)**, *"Toward a Robust Estimation of Respiratory
  Rate from Pulse Oximeters"*, **IEEE Transactions on Biomedical Engineering,
  64(8):1914–1923**. Bài gốc tạo ra BIDMC, dữ liệu đo trên bệnh nhân ICU thật, trích
  từ MIMIC II.
- Trang dữ liệu PhysioNet (mở, có bình duyệt, bắt buộc trích dẫn):
  https://physionet.org/content/bidmc/1.0.0/

### 8.2. Chứng minh NGƯỜI KHÁC đã dùng BIDMC (được cộng đồng công nhận)

- MDPI *Diagnostics* 14(3):284 (2024) — *"A Novel Respiratory Rate Estimation
  Algorithm from Photoplethysmogram Using Deep Learning Model"* (dùng BIDMC):
  https://www.mdpi.com/2075-4418/14/3/284
- MDPI *Bioengineering* 10(2):167 (2023) — *"Machine Learning-Based Respiration Rate
  and Blood Oxygen Saturation Estimation Using PPG Signals"* (BIDMC, ước lượng SpO2):
  https://www.mdpi.com/2306-5354/10/2/167
- Repo GitHub công khai — *rr-prediction-ppg-bidmc* (CNN/LSTM dự đoán nhịp thở từ
  BIDMC): https://github.com/hammadarif784/rr-prediction-ppg-bidmc
- Bản BIDMC tái xử lý trên Zenodo ("32s window") — nhiều nhóm tải về dùng lại.

### 8.3. Chứng minh CÁCH LÀM (autoencoder train trên bình thường → sai số tái tạo) là phương pháp đã công bố

- *IoT* (MDPI) 5(4):39 — *"Autoencoder-Based Neural Network Model for Anomaly
  Detection in Wireless Body Area Networks"* (đúng mô hình autoencoder unsupervised
  + ngưỡng trên tín hiệu sinh hiệu): https://doi.org/10.3390/iot5040039
- *Sensors* (2023, PMC10136265) — *"Anomaly Detection for Sensor Signals Utilizing
  Deep Learning Autoencoder-Based Neural Networks"*:
  https://www.ncbi.nlm.nih.gov/pmc/articles/PMC10136265/
- arXiv 2010.06846 — *"Reconstruct Anomaly to Normal: ... Autoencoder for Time-series
  Anomaly Detection"* (dùng cả BIDMC trong benchmark):
  https://arxiv.org/pdf/2010.06846

### Cách trình bày ngắn gọn

> Nền HR/SpO2/nhịp thở lấy từ BIDMC — bộ ICU thật của Beth Israel Deaconess, công bố
> trên IEEE TBME 2016, được nhiều bài deep learning dùng lại (MDPI 2023/2024,
> Zenodo, GitHub). Phương pháp autoencoder-train-trên-bình-thường của nhóm cũng là
> hướng đã được công bố cho phát hiện bất thường tín hiệu sinh hiệu. Riêng cột truyền
> dịch IV là mô phỏng có nhãn, calibrate theo phân bố BIDMC, vì chưa có bộ IV công
> khai có nhãn.

---

## 9. Hướng mở rộng

- **Nhiệt độ:** hiện KHÔNG thêm, vì dataset (BIDMC + mô phỏng) không có nhiệt độ và
  board chưa có cảm biến nhiệt — thêm cột bịa sẽ mâu thuẫn với lập luận "dữ liệu
  thật". Chỉ thêm khi đồng thời (1) gắn cảm biến nhiệt thật (MLX90614/thermistor) và
  (2) lấy phân bố nhiệt độ từ nguồn thật (MIMIC-III/IV, eICU), rồi sinh lại dataset
  thành 7 đặc trưng và train lại.
- **Bật thêm kênh cảm biến:** kiến trúc đã hỗ trợ (cơ chế DISABLED/OK/LOST) — chỉ cần
  bật `#define` tương ứng trong `sensor_hub.h` và điền code đọc thật, không phải sửa
  phần AI.

---

## 10. Các file liên quan

| File | Vai trò |
|------|---------|
| `dataset/make_synthetic.py` | Sinh lại dataset mô phỏng có nhãn |
| `dataset/download_bidmc_full.py` | Tải đủ 53 bản ghi BIDMC thật |
| `dataset/README_dataset.md` | Mô tả chi tiết dataset |
| `train_autoencoder_pct.py` | Huấn luyện autoencoder + luật % → xuất `.tflite`, `scaler.json`, `threshold.json` |
| `scaler.json` | Trung bình/độ lệch chuẩn (StandardScaler) + baseline HR từng bệnh nhân |
| `threshold.json` | Ngưỡng autoencoder + tham số luật lâm sàng |
| `ai_monitor.c/.h` | Gom 6 đặc trưng + chuẩn hóa + luật lâm sàng (firmware) |
| `model_runner.cpp`, `model_data.h` | Nạp & chạy model int8 trên chip (TFLM) |
