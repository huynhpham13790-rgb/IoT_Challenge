const $ = (id) => document.getElementById(id);
const els = {
  connectionBadge: $('connectionBadge'), connectionText: $('connectionText'), mqttState: $('mqttState'),
  zigbeeState: $('zigbeeState'), deviceCount: $('deviceCount'), messageCount: $('messageCount'),
  deviceSelect: $('deviceSelect'), otaState: $('otaState'), otaDetail: $('otaDetail'), otaPercent: $('otaPercent'),
  installedVersion: $('installedVersion'), latestVersion: $('latestVersion'),
  progressBar: $('progressBar'), progressLabel: $('progressLabel'), remainingLabel: $('remainingLabel'),
  checkButton: $('checkButton'), updateButton: $('updateButton'), abortButton: $('abortButton'),
  firmwareFile: $('firmwareFile'), firmwareFileName: $('firmwareFileName'), firmwareFileMeta: $('firmwareFileMeta'),
  chooseFirmwareButton: $('chooseFirmwareButton'), uploadFirmwareButton: $('uploadFirmwareButton'),
  actionMessage: $('actionMessage'), devicesGrid: $('devicesGrid'), lastRefresh: $('lastRefresh'), footerPort: $('footerPort')
};
let devices = [];
let selected = '';

const escapeHtml = (value) => String(value ?? '').replace(/[&<>'"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;',"'":'&#39;','"':'&quot;'}[c]));
const metric = (value, unit = '—') => Number.isFinite(value) ? `${Math.round(value * 10) / 10}${unit}` : '—';

function updateSelect(next) {
  const names = next.map(d => d.name);
  const otaDevices = next.filter(d => d.supports_ota);
  const previous = selected || els.deviceSelect.value;
  els.deviceSelect.innerHTML = names.length
    ? next.map(device => `<option value="${escapeHtml(device.name)}">${escapeHtml(device.name)}${device.supports_ota ? ' · OTA' : ' · không hỗ trợ OTA'}</option>`).join('')
    : '<option value="">Chưa phát hiện thiết bị</option>';
  selected = names.includes(previous) ? previous : (otaDevices[0]?.name || names[0] || '');
  els.deviceSelect.value = selected;
  const selectedDevice = next.find(d => d.name === selected);
  els.checkButton.disabled = !selectedDevice?.supports_ota;
  els.updateButton.disabled = !selectedDevice?.supports_ota;
}

function renderDevices(next) {
  if (!next.length) {
    els.devicesGrid.innerHTML = '<div class="empty-state"><span>⌁</span><strong>Đang chờ thiết bị Zigbee</strong><p>Dữ liệu sẽ xuất hiện ngay khi gateway nhận được bản tin MQTT.</p></div>';
    return;
  }
  els.devicesGrid.innerHTML = next.map(device => `
    <article class="device-card">
      <div class="device-top">
        <div class="device-name"><span class="device-avatar">⌁</span><div><strong title="${escapeHtml(device.name)}">${escapeHtml(device.name)}</strong><small>${escapeHtml(device.model || device.type || 'Thiết bị Zigbee')} · ${escapeHtml(device.ieee_address || '')}</small></div></div>
        <span class="live-pill ${device.supports_ota ? '' : 'muted'}">${device.supports_ota ? '● OTA' : '● ZIGBEE'}</span>
      </div>
      <div class="sensor-values">
        <div class="sensor"><small>Nhiệt độ</small><strong>${metric(device.temperature, '°C')}</strong></div>
        <div class="sensor"><small>Độ ẩm</small><strong>${metric(device.humidity, '%')}</strong></div>
        <div class="sensor"><small>Pin</small><strong>${metric(device.battery, '%')}</strong></div>
        <div class="sensor"><small>Link quality</small><strong>${metric(device.linkquality, '')}</strong></div>
      </div>
      <div class="device-update"><span>${escapeHtml(device.update_state || 'OTA chưa kiểm tra')}</span><span>${escapeHtml(device.last_seen || '')}</span></div>
    </article>`).join('');
}

function renderOta() {
  const device = devices.find(d => d.name === selected);
  const state = device?.update_state || 'idle';
  const progress = Number.isFinite(device?.update_progress) ? Math.max(0, Math.min(100, device.update_progress)) : 0;
  const installed = Number.isFinite(device?.installed_version) ? Math.trunc(device.installed_version) : null;
  const latest = Number.isFinite(device?.latest_version) ? Math.trunc(device.latest_version) : null;
  const isLatest = installed !== null && latest !== null && installed === latest;
  const labels = {idle:isLatest ? 'Đã cập nhật mới nhất' : 'Chưa có bản cập nhật',available:'Có firmware mới',scheduled:'Đã lên lịch',updating:'Đang cập nhật'};
  els.otaState.textContent = labels[state] || state || 'Chưa kiểm tra';
  els.otaDetail.textContent = selected ? (device?.supports_ota ? `Thiết bị OTA: ${selected}` : `${selected} chưa khai báo hỗ trợ OTA`) : 'Chọn thiết bị để bắt đầu';
  els.otaPercent.textContent = state === 'updating' ? `${Math.round(progress)}%` : '—';
  els.installedVersion.textContent = installed === null ? '—' : `v${installed}`;
  els.latestVersion.textContent = latest === null ? '—' : `v${latest}`;
  els.progressBar.style.width = `${state === 'updating' ? progress : 0}%`;
  els.progressLabel.textContent = state === 'updating' ? 'Đang truyền firmware' : 'Sẵn sàng';
  els.remainingLabel.textContent = Number.isFinite(device?.update_remaining) ? `Còn khoảng ${device.update_remaining}s` : '—';
  els.abortButton.classList.toggle('hidden', state !== 'updating');
  els.updateButton.classList.toggle('hidden', state === 'updating');
  els.checkButton.disabled = !device?.supports_ota || state === 'updating';
  els.updateButton.disabled = !device?.supports_ota;
}

function formatBytes(size) {
  if (size < 1024) return `${size} B`;
  if (size < 1024 * 1024) return `${(size / 1024).toFixed(1)} KB`;
  return `${(size / 1024 / 1024).toFixed(2)} MB`;
}

async function uploadFirmware() {
  const file = els.firmwareFile.files[0];
  if (!file) return;
  if (!file.name.toLowerCase().endsWith('.ota')) {
    els.actionMessage.className = 'notice error';
    els.actionMessage.textContent = 'Hãy chọn file Zigbee OTA có đuôi .ota, không chọn trực tiếp file .gbl.';
    return;
  }
  if (file.size > 4 * 1024 * 1024) {
    els.actionMessage.className = 'notice error';
    els.actionMessage.textContent = 'File vượt quá giới hạn 4 MB.';
    return;
  }
  els.uploadFirmwareButton.disabled = true;
  els.actionMessage.className = 'notice';
  els.actionMessage.textContent = 'Đang tải firmware lên gateway…';
  try {
    const response = await fetch('/api/firmware', {method:'POST', headers:{'Content-Type':'application/octet-stream'}, body:file});
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || 'Không tải được firmware');
    els.actionMessage.className = 'notice success';
    els.actionMessage.textContent = `Đã nhận ${result.file} · version ${result.file_version} · manufacturer 0x${Number(result.manufacturer_code).toString(16).padStart(4,'0').toUpperCase()}. Bây giờ bấm “Kiểm tra bản mới”.`;
    els.firmwareFileMeta.textContent = `Đã lưu trên gateway · ${formatBytes(result.size)} · version ${result.file_version}`;
  } catch (error) {
    els.actionMessage.className = 'notice error';
    els.actionMessage.textContent = error.message;
  } finally {
    els.uploadFirmwareButton.disabled = false;
  }
}

async function refresh() {
  try {
    const response = await fetch('/api/status', {cache:'no-store'});
    if (!response.ok) throw new Error('HTTP ' + response.status);
    const data = await response.json();
    els.connectionBadge.classList.toggle('online', data.mqtt_connected && data.zigbee_online);
    els.connectionText.textContent = data.mqtt_connected && data.zigbee_online ? 'Gateway đang trực tuyến' : 'Gateway chưa sẵn sàng';
    els.mqttState.textContent = data.mqtt_connected ? 'Đã kết nối' : 'Mất kết nối';
    els.zigbeeState.textContent = data.zigbee_online ? 'Đang hoạt động' : 'Chưa online';
    els.deviceCount.textContent = data.devices.length;
    els.messageCount.textContent = Number(data.message_count || 0).toLocaleString('vi-VN');
    devices = data.devices;
    updateSelect(devices);
    renderDevices(devices);
    renderOta();
    els.lastRefresh.textContent = `Cập nhật ${new Date().toLocaleTimeString('vi-VN')}`;
  } catch (_) {
    els.connectionBadge.classList.remove('online');
    els.connectionText.textContent = 'Mất kết nối dashboard';
  }
}

async function otaAction(action) {
  if (!selected) return;
  if (action === 'update' && !confirm(`Bắt đầu OTA cho “${selected}”?\n\nKhông ngắt nguồn thiết bị hoặc gateway trong quá trình cập nhật.`)) return;
  const button = action === 'check' ? els.checkButton : action === 'abort' ? els.abortButton : els.updateButton;
  button.disabled = true;
  els.actionMessage.className = 'notice';
  els.actionMessage.textContent = action === 'check' ? 'Đang yêu cầu thiết bị kiểm tra firmware…' : action === 'abort' ? 'Đang gửi yêu cầu dừng…' : 'Đã gửi yêu cầu OTA. Đang chờ thiết bị phản hồi…';
  try {
    const response = await fetch('/api/ota', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action,device:selected})});
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || 'Không gửi được yêu cầu');
    els.actionMessage.className = 'notice success';
    els.actionMessage.textContent = result.message;
    setTimeout(refresh, 500);
  } catch (error) {
    els.actionMessage.className = 'notice error';
    els.actionMessage.textContent = error.message;
  } finally { button.disabled = false; }
}

els.deviceSelect.addEventListener('change', () => { selected = els.deviceSelect.value; renderOta(); });
els.chooseFirmwareButton.addEventListener('click', () => els.firmwareFile.click());
els.firmwareFile.addEventListener('change', () => {
  const file = els.firmwareFile.files[0];
  els.firmwareFileName.textContent = file?.name || 'Chưa chọn file .ota';
  els.firmwareFileMeta.textContent = file ? `${formatBytes(file.size)} · sẵn sàng tải lên` : 'Tối đa 4 MB · gateway sẽ kiểm tra header trước khi nhận';
  els.uploadFirmwareButton.disabled = !file;
});
els.uploadFirmwareButton.addEventListener('click', uploadFirmware);
els.checkButton.addEventListener('click', () => otaAction('check'));
els.updateButton.addEventListener('click', () => otaAction('update'));
els.abortButton.addEventListener('click', () => otaAction('abort'));
els.footerPort.textContent = location.host;
refresh();
setInterval(refresh, 1500);
