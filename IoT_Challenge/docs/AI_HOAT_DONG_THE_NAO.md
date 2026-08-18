# AI trong Smart IV làm gì — khi nào báo động, khi nào không

Tài liệu này viết cho người **không đọc code**: bác sĩ, y tá, ban giám khảo,
hoặc thành viên mới trong nhóm. Mục tiêu là trả lời đúng ba câu:

1. AI ở đây thực sự làm gì, và làm được gì mà một cái ngưỡng cố định không làm được?
2. **Khi nào** nó kêu?
3. **Khi nào nó cố tình KHÔNG kêu** — phần này quan trọng ngang phần trên.

Chi tiết kỹ thuật (kiến trúc model, dataset, số đo) nằm ở
[`AI_TIME_SERIES_TAT_TAN_TAT.md`](AI_TIME_SERIES_TAT_TAN_TAT.md) và
[`Dataset_va_Phuong_phap_AI_SmartIV.md`](Dataset_va_Phuong_phap_AI_SmartIV.md).

---

## 1. Toàn bộ AI chạy TRÊN CHIP ở đầu giường

Không có cloud, không có server nào tham gia vào việc **quyết định báo động**.
Chip EFR32xG26 gắn ở giường tự đo, tự chạy model, tự quyết định, rồi mới gửi
kết quả đi. Hệ quả thực tế: **rút mạng, sập Wi-Fi, tắt server thì thiết bị vẫn
báo động bình thường** — chỉ là không ai ở trạm điều dưỡng nhìn thấy.

Mỗi giây chip làm đúng một vòng: đọc cảm biến → chạy 2 model + luật lâm sàng →
quyết định → hiện lên màn hình đầu giường + đèn/còi + gửi Zigbee.

---

## 2. Thiết bị đo bốn thứ

| Kênh | Cảm biến | Ý nghĩa |
|---|---|---|
| Nhịp tim (HR) | MAX30102 kẹp đầu ngón tay | bpm |
| SpO2 | cùng con MAX30102 | % bão hoà oxy |
| Tốc độ truyền (Flow) | loadcell + HX711 cân túi dịch | so với **mức bác sĩ đặt**, không phải con số tuyệt đối |
| Giọt/phút (Drops) | cảm biến giọt ở buồng đếm | so với mức bác sĩ đặt |

Hai kênh dưới luôn được quy về **tỉ lệ so với y lệnh**: `1.00×` = đang chảy
đúng như bác sĩ kê. Nhờ vậy cùng một bộ ngưỡng dùng được cho mọi y lệnh, và
bác sĩ đổi y lệnh từ xa thì AI đổi cách đánh giá ngay lập tức, không cần nạp
lại chip.

---

## 3. Ba "bộ não" chạy song song

Điểm cốt lõi: **AI không thay thế luật lâm sàng, nó chạy SONG SONG**. Cái nào
kêu trước thì báo. Thiết bị y tế không được phép bỏ sót chỉ vì model đoán sai.

### 3.1 Luật lâm sàng cứng — cái không bao giờ được tắt

Ngưỡng cố định, ai cũng kiểm tra được, không có gì bí ẩn:

| Điều kiện | Ngưỡng |
|---|---|
| SpO2 thấp | < 90% |
| Nhịp tim | lệch > 30% so với **baseline riêng của bệnh nhân đó**, hoặc < 45, hoặc > 150 bpm |
| Đường truyền | chảy > 1.5× mức đặt (free-flow) hoặc < 0.3× (tắc) |
| Mất tín hiệu | cảm biến đã lắp nhưng 3 giây không có mẫu mới |

"Baseline riêng của bệnh nhân" là phần đáng chú ý: 60 giây đầu sau khi gắn
máy, thiết bị đo nhịp tim nền của **chính người đó** rồi so lệch phần trăm với
mức nền ấy. Một cụ già nhịp nền 55 và một thanh niên nhịp nền 85 không bị đo
bằng cùng một cái thước.

### 3.2 Autoencoder — "cái này trông lạ"

Model học **dáng vẻ bình thường** của 6 con số (HR, SpO2, flow, drops, 2 cờ
mất tín hiệu). Khi tổ hợp hiện tại khác lạ so với mọi thứ nó từng thấy, sai số
tái tạo tăng vọt → báo.

Nó bắt được thứ mà ngưỡng không định nghĩa nổi: **từng chỉ số đều nằm trong
giới hạn nhưng tổ hợp thì vô lý**. Ví dụ nhịp tim 130 (chưa quá 150) đi kèm
SpO2 92% (chưa dưới 90) và dịch chảy 1.4× (chưa quá 1.5): không có luật nào
kêu, nhưng cả cụm đó đứng cạnh nhau là bất thường.

### 3.3 Bộ dự báo chuỗi thời gian — "cái này SẮP xấu"

Model nhìn **64 giây vừa qua** rồi dự báo **16 giây tới** cho cả 4 kênh. Từ đó
ra ba thứ mà một ngưỡng tức thời không thể có:

- **Xu hướng**: "nhịp tim đang tăng 22 bpm/phút" — không phải "nhịp tim đang là
  118".
- **Cảnh báo sớm**: dự báo cho thấy chỉ số sẽ vượt giới hạn lâm sàng trong 16
  giây tới, **trong khi hiện tại vẫn còn trong giới hạn**.
- **Bất thường**: model chỉ học dữ liệu bình thường, nên phần nào nó **không
  dự báo nổi** chính là phần bất thường.

---

## 4. Khi nào thiết bị BÁO ĐỘNG

Ba tầng, xếp theo mức độ:

### Tầng 1 — ĐỎ ngay lập tức, không chờ, không hoãn

- **Mất tín hiệu** một cảm biến đang được lắp.
- **SpO2 < 90%**.
- **Nhịp tim < 45 hoặc > 150 bpm** (giới hạn tuyệt đối).

Ba thứ này tuyệt đối không bị làm trễ. Cả tài liệu về "mệt mỏi vì báo động"
(alarm fatigue) đều ủng hộ việc hoãn báo với các tình huống chớp nhoáng vô
hại — nhưng lập luận đó **không áp dụng** cho ba trường hợp này.

### Tầng 2 — ĐỎ sau khi đã xác nhận kéo dài

- **Bộ dự báo báo bất thường liên tục 11 giây liền** (không phải một lần nháy).
- **Đường truyền tắc hoặc chảy tự do** theo luật lâm sàng.

### Tầng 3 — VÀNG, "đang xấu đi nhưng chưa nguy hiểm"

- **Cảnh báo sớm**: dự báo sẽ vượt giới hạn trong 16 giây tới.
- Nhịp tim lệch nhiều so với baseline riêng (nhưng vẫn trong 45–150).
- Autoencoder thấy tổ hợp lạ.

Cảnh báo sớm để VÀNG chứ không ĐỎ là có chủ ý: nó là một **dự đoán**, chưa
phải sự thật.

---

## 5. Khi nào nó CỐ TÌNH KHÔNG BÁO

Phần này mới là chỗ tốn công nhất, vì một thiết bị kêu suốt ngày thì y tá sẽ
tắt nó đi — và lúc đó nó vô dụng đúng vào lúc cần nhất.

| Tình huống | Vì sao không báo |
|---|---|
| **Nháy 2–6 giây rồi về bình thường** (cử động, nói chuyện, chỉnh lại kẹp tay) | Bất thường phải **kéo dài 11 giây liên tiếp** mới được kêu. Đo thực tế: quyết định ngay lập tức cho 17,6% báo nhầm trên các cú nháy; thêm điều kiện kéo dài kéo xuống **2,3%** |
| **Cảm biến chưa lắp bao giờ** | Kênh đó ở trạng thái "chưa lắp", bị bỏ qua hoàn toàn. Chỉ kênh **đã lắp rồi mất tín hiệu** mới báo |
| **64 giây đầu sau khi bật máy** | Bộ dự báo chưa gom đủ cửa sổ; nó im lặng, luật lâm sàng vẫn chạy đầy đủ |
| **60 giây đầu khi gắn máy cho bệnh nhân** | Đang đo baseline nhịp tim của người đó; chưa so lệch phần trăm với ai cả |
| **Xu hướng nhỏ hơn ±10 bpm/phút** | Đo được là chỉ đúng ~72% ở vùng đó, dưới nữa thì chủ yếu là nhiễu. Không hiện mũi tên xu hướng cho biến động nhỏ hơn thế |
| **Kênh mất tín hiệu đọc ra 0** | 0 không được coi là một chỉ số. Giường trống sẽ **không** kêu "SpO2 0% — nguy kịch"; nó báo đúng vấn đề thật: *chưa cắm cảm biến* |

---

## 6. Sáu ví dụ cụ thể

### Ví dụ 1 — Mọi thứ bình thường
HR 78, SpO2 98%, dịch chảy 1.02× mức đặt, giọt đều.
→ Luật lâm sàng: im. Autoencoder: sai số 0,4 (ngưỡng 1,43). Dự báo: 16 giây
tới vẫn vậy.
→ **Xanh.** Màn hình đầu giường hiện "BINH THUONG".

### Ví dụ 2 — Bệnh nhân cử động, nhịp tim nháy lên 3 giây
HR nhảy 82 → 148 trong 3 giây rồi về 85.
→ Chưa chạm 150 nên luật tuyệt đối không kêu. Bộ dự báo có thấy lạ, nhưng chỉ
lạ **4 giây** — chưa đủ 11 giây.
→ **Không báo.** Đây chính là kiểu báo nhầm mà thầy hướng dẫn phê bình, và là
lý do có điều kiện "kéo dài".

### Ví dụ 3 — Tụt oxy thật
SpO2 tụt dần 96 → 93 → 90 → 88%.
→ Ở mức 88% luật lâm sàng kêu **ngay tại giây đó**, không đợi AI, không đợi
đủ 11 giây.
→ **ĐỎ ngay.** Console hiện "Critically low SpO2: 88%", màn hình đầu giường
hiện "SPO2 THAP", banner đỏ toàn màn hình ở trạm điều dưỡng.

### Ví dụ 4 — Dây truyền đang tắc dần (chỗ AI ăn điểm)
Giọt giảm từ từ: 50 → 44 → 38 → 33 giọt/phút trong khoảng một phút. Y lệnh là
60, nên ngưỡng "tắc" (0,3× = 18 giọt/phút) **vẫn còn xa**.
→ Luật lâm sàng: im, vì 33 vẫn trên 18. Bộ dự báo: 64 giây vừa qua không giống
bất kỳ đoạn bình thường nào nó từng học, sai số vượt ngưỡng và **giữ nguyên
suốt hơn 11 giây**; đồng thời dự báo 16 giây tới sẽ xuống dưới giới hạn.
→ **Cảnh báo sớm (vàng) trước, rồi ĐỎ.** Y tá tới trước khi dây tắc hẳn.
Đây là thứ mà một cái ngưỡng cố định về nguyên tắc không thể làm được: nó chỉ
biết "đã vượt" chứ không biết "đang đi tới".

### Ví dụ 5 — Y tá rút kẹp SpO2 ra để đo huyết áp
Kênh HR và SpO2 mất tín hiệu, cảm biến giọt và cân vẫn chạy.
→ **Vàng, kèm chữ "No signal from: HR, SpO2".** Không hiện xanh giả vờ bình
thường, cũng không hiện "SpO2 0% nguy kịch". Các kênh còn lại vẫn được theo
dõi bình thường.

### Ví dụ 6 — Tổ hợp lạ nhưng từng chỉ số đều "hợp lệ"
HR 132 (< 150 ✓), SpO2 92% (> 90 ✓), dịch chảy 1,45× (< 1,5 ✓).
→ Không luật nào kêu. Nhưng autoencoder chưa từng thấy ba con số này đứng
cạnh nhau ở dữ liệu bình thường → sai số tái tạo vượt ngưỡng.
→ **Vàng.** "AI model flagged an abnormal drip pattern".

---

## 7. Nói thẳng về giới hạn

Phần này để trong tài liệu có chủ đích — một hệ thống y tế mà chỉ khoe điểm
mạnh thì không đáng tin:

- **Model bắt được 58,3% các sự cố kéo dài** trên tập test. Nghĩa là **không
  được** dùng AI thay cho luật lâm sàng — nó là lớp bắt thêm, chạy song song.
- **Phần truyền dịch (giọt, cân nặng) vẫn là dữ liệu mô phỏng.** HR/SpO2 dùng
  dữ liệu ICU thật (BIDMC, 53 bệnh nhân từ PhysioNet), nhưng chưa có bộ dữ liệu
  công khai có nhãn cho truyền dịch IV.
- **Chưa kiểm chứng trên bệnh nhân thật.** Mọi con số là trên dataset, cộng
  phép đo thời gian chạy thật trên chip.
- Model chỉ học dữ liệu **bình thường**. Nên khi một kênh đang bất thường kéo
  dài, con số nó đưa ra không phải "dự báo" mà là "mức bình thường đáng lẽ phải
  có" — giao diện đổi nhãn thành *"expected if normal"* đúng lúc đó, thay vì
  hiện một con số trông như dự báo thật.

---

## 8. Muốn tự kiểm chứng model

Model không phải hộp đen: hai file `.tflite` nằm ngay trong repo và mở được
bằng công cụ phổ thông.

| Việc | Cách |
|---|---|
| Xem kiến trúc, từng lớp, kích thước tensor | Mở `ml/models/*.tflite` bằng [Netron](https://netron.app) — kéo thả file vào trình duyệt |
| Chạy lại toàn bộ huấn luyện | `ml/ai_timeseries/`, xem mục 11 của `AI_TIME_SERIES_TAT_TAN_TAT.md` |
| Đánh giá lại trên tập test | `evaluate.py` — chọn tham số trên tập validation rồi báo cáo **một lần** trên test |
| Xem model chạy thật trên chip | Cắm USB, mở serial 115200, xem dòng `[TS]` in mỗi giây: xu hướng, dự báo, điểm bất thường, bộ đếm kéo dài |
