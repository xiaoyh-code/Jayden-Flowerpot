'use strict';

(() => {
  const $ = (id) => document.getElementById(id);
  const elements = {
    deviceStatus: $('device-status'), modelStatus: $('model-status'), notice: $('connection-notice'),
    deviceName: $('device-name'), deviceAddress: $('device-address'), modelName: $('model-name'),
    modelIndicator: $('model-indicator'), cameraImage: $('camera-image'), cameraEmpty: $('camera-empty'),
    imagePreview: $('image-preview'), imageCounter: $('image-counter'), imageTimestamp: $('image-timestamp'),
    capture: $('capture-button'), submit: $('submit-button'), submitLabel: $('submit-label'),
    prompt: $('prompt'), promptHint: $('prompt-hint'), modeLook: $('mode-look'), modeChat: $('mode-chat'),
    messages: $('messages'), emptyConversation: $('conversation-empty'), activity: $('activity-line'),
    activityText: $('activity-text'), stagePending: $('stage-pending'), stagePendingText: $('stage-pending-text'),
    settings: $('settings-dialog'), boardHost: $('board-host'), settingsError: $('settings-error'),
    saveSettings: $('save-settings'), settingsModel: $('settings-model'), settingsBridge: $('settings-bridge'),
    imageDialog: $('image-dialog'), largeImage: $('large-image'), largeImageTime: $('large-image-time'),
    streamButton: $('stream-button'), streamButtonLabel: $('stream-button-label'),
    streamImage: $('stream-image'), streamStatus: $('stream-status'), liveBadge: $('live-badge'),
    stageModeLabel: $('stage-mode-label'),
    cameraSettingsForm: $('camera-settings-form'), cameraSettingsFields: $('camera-settings-fields'),
    cameraSettingsStatus: $('camera-settings-status'), cameraFps: $('camera-fps'),
    cameraFlicker: $('camera-flicker'), cameraWb: $('camera-wb'),
    cameraBrightness: $('camera-brightness'), cameraSaturation: $('camera-saturation'),
    brightnessValue: $('brightness-value'), saturationValue: $('saturation-value'),
    saveCameraSettings: $('save-camera-settings'), reloadCameraSettings: $('reload-camera-settings'),
    toast: $('toast'),
  };
  let currentState = null;
  let mode = 'look';
  let requestInFlight = false;
  let activeJob = null;
  let pollInFlight = false;
  let stateTimer = null;
  let jobTimer = null;
  let toastTimer = null;
  let displayedImageId = null;
  let failedImageUrl = null;
  let serverAvailable = false;
  let transientError = '';
  let previewPhase = 'off';
  let previewUrl = null;
  let previewError = '';
  let previewAcknowledged = false;
  let previewGeneration = 0;
  let previewStopPromise = null;
  let previewStartTimer = null;
  let pageLeaving = false;
  let cameraSettings = null;
  let cameraSettingsDirty = false;
  let cameraSettingsReading = false;
  let cameraSettingsSaving = false;
  let cameraSettingsReadAttempted = false;
  let cameraSettingsError = '';
  let cameraSettingsNote = '';
  const entryNodes = new Map();

  function readableError(error) {
    if (error && error.name === 'AbortError') return '等候回應太耐，請檢查連接後再試。';
    if (error instanceof TypeError) return '連唔到 Mac 工作台，請確認啟動視窗仍然開啟。';
    return error && error.message ? error.message : '暫時未能完成，請再試一次。';
  }

  async function api(path, { method = 'GET', body, timeout = 15000 } = {}) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeout);
    const headers = { Accept: 'application/json' };
    if (body !== undefined) {
      headers['Content-Type'] = 'application/json';
      headers['X-CSRF-Token'] = currentState?.csrf_token || '';
    }
    try {
      const response = await fetch(path, {
        method, headers, body: body === undefined ? undefined : JSON.stringify(body),
        signal: controller.signal, credentials: 'same-origin', cache: 'no-store',
      });
      let result;
      try { result = await response.json(); }
      catch { throw new Error('工作台回應未能讀取，請重新整理頁面。'); }
      if (!response.ok) {
        const detail = typeof result.error === 'string' ? result.error : result.error?.message;
        const error = new Error(detail || `未能完成請求（${response.status}）。`);
        error.status = response.status;
        throw error;
      }
      return result;
    } finally {
      clearTimeout(timer);
    }
  }

  function formatTime(value, full = false) {
    if (value === undefined || value === null || value === '') return '';
    const normalized = typeof value === 'number' && value < 1e12 ? value * 1000 : value;
    const date = new Date(normalized);
    if (Number.isNaN(date.getTime())) return '';
    return date.toLocaleString('zh-HK', full
      ? { month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false }
      : { hour: '2-digit', minute: '2-digit', hour12: false });
  }

  function imageUrl(value) {
    if (typeof value !== 'string') return null;
    try {
      const parsed = new URL(value, window.location.origin);
      if (parsed.origin !== window.location.origin || !/^\/api\/images\/[a-zA-Z0-9_-]+\.jpg$/.test(parsed.pathname)) return null;
      return parsed.pathname;
    } catch { return null; }
  }

  function streamUrl(value) {
    if (typeof value !== 'string') return null;
    try {
      const parsed = new URL(value, window.location.origin);
      const session = parsed.searchParams.get('session');
      if (parsed.origin !== window.location.origin || parsed.pathname !== '/api/stream.mjpg' ||
          !session || !/^[a-zA-Z0-9_-]{1,160}$/.test(session) || parsed.hash) return null;
      return `${parsed.pathname}?session=${encodeURIComponent(session)}`;
    } catch { return null; }
  }

  function previewEngaged() {
    return previewPhase !== 'off' || Boolean(currentState?.preview?.active) ||
      Boolean(currentState?.preview?.starting) || Boolean(currentState?.preview?.stopping);
  }

  function detachPreview() {
    clearTimeout(previewStartTimer);
    previewUrl = null;
    elements.streamImage.removeAttribute('src');
    elements.streamImage.hidden = true;
    elements.liveBadge.hidden = true;
  }

  function reconcilePreview() {
    const preview = currentState?.preview;
    if (!preview) return;
    if (preview.active || preview.starting || preview.stopping) previewAcknowledged = true;
    if (typeof preview.error === 'string' && preview.error) previewError = preview.error;
    if (previewUrl && preview.active && Number(preview.frames) > 0 && previewPhase === 'starting') {
      previewPhase = 'live';
      clearTimeout(previewStartTimer);
    }
    if ((previewAcknowledged || previewPhase === 'stopping') && !preview.active && !preview.starting && !preview.stopping &&
        !currentState.busy && previewPhase !== 'off' && !previewStopPromise) {
      detachPreview();
      previewPhase = 'off';
      previewAcknowledged = false;
    }
  }

  function renderPreview() {
    const engaged = previewEngaged();
    const preview = currentState?.preview;
    const showing = Boolean(previewUrl) && previewPhase !== 'stopping';
    elements.streamImage.hidden = !showing;
    elements.liveBadge.hidden = !(showing && previewPhase === 'live');
    elements.imageCounter.classList.toggle('fps-caption', showing && previewPhase === 'live');
    elements.imageTimestamp.classList.toggle('fps-caption', showing && previewPhase === 'live');
    elements.stageModeLabel.textContent = showing ? '即時畫面' : '單次拍攝';
    elements.streamButton.setAttribute('aria-pressed', String(engaged));
    elements.streamButtonLabel.textContent = previewPhase === 'stopping'
      ? (previewStopPromise ? '正在停止…' : '重試停止') : engaged ? '停止直播' : '開始直播';
    if (showing) {
      elements.imagePreview.hidden = true;
      elements.cameraEmpty.hidden = true;
      elements.imageCounter.textContent = previewPhase === 'live'
        ? (cameraSettings ? `目標 ${cameraSettings.fps} fps` : 'LIVE PREVIEW') : 'CONNECTING';
      const fps = preview?.fps;
      elements.imageTimestamp.textContent = previewPhase === 'live'
        ? (typeof fps === 'number' && Number.isFinite(fps) && fps >= 0 && Number(preview.frames) >= 2
          ? `實際 ${fps.toFixed(1)} fps` : '實際幀率計算中…')
        : '等候相機畫面…';
    }
    let status = '';
    if (previewError) status = previewError;
    else if (previewPhase === 'stopping') status = '正在停止相機，請稍等確認。';
    else if (previewPhase === 'starting') status = '正在連接直播，可以隨時按「停止直播」。';
    else if (showing) status = '直播中 · 先停止直播，再拍照分析或文字對話。';
    else if (preview?.stopping) status = '正在停止相機，請稍等確認。';
    else if (preview?.active || preview?.starting) status = '直播已在其他頁面啟動；此頁唔會自動接收畫面。';
    elements.streamStatus.hidden = !status;
    elements.streamStatus.textContent = status;
    elements.streamStatus.dataset.error = String(Boolean(previewError));
  }

  function setStatus(element, state, label) {
    element.dataset.state = state;
    element.lastElementChild.textContent = label;
  }

  function showToast(text, isError = false) {
    clearTimeout(toastTimer);
    elements.toast.textContent = text;
    elements.toast.dataset.error = String(isError);
    elements.toast.hidden = false;
    toastTimer = setTimeout(() => { elements.toast.hidden = true; }, isError ? 7000 : 3500);
  }

  function setActivity(text, state = 'ready') {
    elements.activityText.textContent = text;
    elements.activity.dataset.state = state;
  }

  function validatedCameraSettings(value) {
    if (!value || typeof value !== 'object' || ![10, 15, 20, 25].includes(value.fps) ||
        !['auto', '50', '60'].includes(value.flicker_hz) ||
        !['auto', 'office', 'home', 'daylight'].includes(value.wb_mode) ||
        !Number.isInteger(value.brightness) || Math.abs(value.brightness) > 2 ||
        !Number.isInteger(value.saturation) || Math.abs(value.saturation) > 2) return null;
    return { fps: value.fps, flicker_hz: value.flicker_hz, wb_mode: value.wb_mode,
      brightness: value.brightness, saturation: value.saturation };
  }

  function updateAdjustmentLabels() {
    const format = (value) => Number(value) > 0 ? `+${Number(value)}` : String(Number(value));
    elements.brightnessValue.textContent = format(elements.cameraBrightness.value);
    elements.saturationValue.textContent = format(elements.cameraSaturation.value);
  }

  function renderCameraSettings() {
    const canonical = validatedCameraSettings(currentState?.camera_settings);
    if (canonical) cameraSettings = canonical;
    if (cameraSettings && !cameraSettingsDirty && !cameraSettingsSaving) {
      elements.cameraFps.value = String(cameraSettings.fps);
      elements.cameraFlicker.value = cameraSettings.flicker_hz;
      elements.cameraWb.value = cameraSettings.wb_mode;
      elements.cameraBrightness.value = String(cameraSettings.brightness);
      elements.cameraSaturation.value = String(cameraSettings.saturation);
      updateAdjustmentLabels();
    }
    const streaming = previewEngaged();
    const ready = serverAvailable && Boolean(currentState?.device?.connected) && Boolean(currentState?.csrf_token);
    const busy = Boolean(currentState?.busy) || requestInFlight || Boolean(activeJob);
    const blocked = !ready || busy || streaming || cameraSettingsReading || cameraSettingsSaving;
    elements.cameraSettingsForm.hidden = !cameraSettings;
    elements.cameraSettingsFields.disabled = blocked || !cameraSettings;
    elements.saveCameraSettings.disabled = blocked || !cameraSettings || !cameraSettingsDirty;
    elements.saveCameraSettings.textContent = cameraSettingsSaving ? '正在儲存調校…' : '儲存相機調校';
    elements.reloadCameraSettings.disabled = cameraSettingsReading || cameraSettingsSaving;
    elements.cameraSettingsStatus.dataset.error = String(Boolean(cameraSettingsError));
    elements.cameraSettingsStatus.textContent = cameraSettingsError ||
      (cameraSettingsSaving ? '正在儲存相機調校…'
        : streaming ? '先停止直播，再調校相機。'
        : cameraSettingsReading ? '正在讀取相機目前設定…'
        : !cameraSettings ? '未讀到相機設定，請確認 ESP32 已連接，再按「重新讀取」。'
        : !ready ? 'ESP32 未連接，連接後先可以儲存調校。'
        : busy ? '請等候目前的拍攝或對話完成，再調校相機。'
        : cameraSettingsDirty ? '有未儲存的調校；直播仍會使用目前已儲存設定。'
        : cameraSettingsNote || '目前相機設定已載入。調校會喺下次拍攝時套用。');
  }

  async function refreshCameraSettings() {
    if (cameraSettingsReading || cameraSettingsSaving) return;
    cameraSettingsReading = true;
    cameraSettingsReadAttempted = true;
    cameraSettingsError = '';
    renderCameraSettings();
    try {
      const result = await api('/api/camera/settings');
      const settings = validatedCameraSettings(result.settings);
      if (!settings) throw new Error('未讀到可用的相機調校設定。');
      cameraSettings = settings;
      if (currentState) currentState.camera_settings = settings;
      cameraSettingsDirty = false;
      cameraSettingsNote = '';
    } catch (error) {
      cameraSettingsError = readableError(error);
    } finally {
      cameraSettingsReading = false;
      renderCameraSettings();
      renderPreview();
    }
  }

  async function saveCameraSettings(event) {
    event.preventDefault();
    if (previewEngaged()) {
      cameraSettingsError = '先停止直播，再調校相機。';
      renderCameraSettings();
      return;
    }
    if (cameraSettingsSaving || cameraSettingsReading || currentState?.busy || requestInFlight || activeJob || !serverAvailable || !currentState?.device?.connected) return;
    const settings = validatedCameraSettings({
      fps: Number(elements.cameraFps.value), flicker_hz: elements.cameraFlicker.value,
      wb_mode: elements.cameraWb.value, brightness: Number(elements.cameraBrightness.value),
      saturation: Number(elements.cameraSaturation.value),
    });
    if (!settings) {
      cameraSettingsError = '請檢查相機調校的選項及數值。';
      renderCameraSettings();
      return;
    }
    cameraSettingsSaving = true;
    cameraSettingsError = '';
    render();
    try {
      const result = await api('/api/camera/settings', { method: 'POST', body: settings });
      const canonical = validatedCameraSettings(result.settings);
      if (!canonical) throw new Error('設定已傳送，但未能確認結果。請按「重新讀取」檢查。');
      cameraSettings = canonical;
      if (currentState) currentState.camera_settings = canonical;
      cameraSettingsDirty = false;
      cameraSettingsNote = '已儲存。下次開始直播或拍照時會使用新調校。';
      showToast('相機調校已儲存；冇拍攝相片。');
    } catch (error) {
      cameraSettingsError = error.status === 409 ? '相機仍然忙碌，請先停止直播，再儲存調校。' : readableError(error);
    } finally {
      cameraSettingsSaving = false;
      render();
    }
  }

  function renderAvailability() {
    const deviceReady = serverAvailable && Boolean(currentState?.device?.connected);
    const modelReady = serverAvailable && Boolean(currentState?.model?.connected);
    const bridgeReady = serverAvailable && Boolean(currentState?.bridge?.connected);
    const streaming = previewEngaged();
    const busy = requestInFlight || Boolean(activeJob) || Boolean(currentState?.busy) || streaming || cameraSettingsSaving;
    const writable = Boolean(currentState?.csrf_token);
    const allReady = deviceReady && modelReady && bridgeReady && writable;
    const chatTooLong = mode === 'chat' && new TextEncoder().encode(elements.prompt.value.trim()).length > 1023;
    elements.capture.disabled = busy || !deviceReady || !writable;
    // Stopping remains possible even if the model, device or state polling fails.
    elements.streamButton.disabled = streaming ? false : busy || !deviceReady || !writable;
    elements.submit.disabled = busy || !allReady || chatTooLong || (mode === 'chat' && !elements.prompt.value.trim());
    elements.modeLook.disabled = busy;
    elements.modeChat.disabled = busy;
    elements.prompt.disabled = busy;
    elements.submitLabel.textContent = streaming ? '請先停止直播' : busy
      ? (activeJob?.action === 'capture' ? '正在拍攝…' : activeJob?.action === 'look' ? '正在分析…' : '正在處理…')
      : mode === 'look' ? '拍照並分析' : '傳送訊息';
    elements.stagePending.hidden = !(previewPhase === 'starting' || previewPhase === 'stopping' || (activeJob && (activeJob.action === 'capture' || activeJob.action === 'look')));
    elements.stagePendingText.textContent = previewPhase === 'starting' ? '正在連接直播…'
      : previewPhase === 'stopping' ? '正在停止直播…'
      : activeJob?.action === 'look' ? '正在拍照並分析…' : '正在拍攝…';
    if (streaming) {
      setActivity(previewPhase === 'stopping' ? '正在停止直播，完成後即可繼續對話。' : '先停止直播，再拍照分析或文字對話。', 'ready');
    } else if (cameraSettingsSaving) {
      setActivity('正在儲存相機調校…', 'busy');
    } else if (activeJob || requestInFlight || currentState?.busy) {
      const message = activeJob?.action === 'capture' ? '相機正在拍攝，請稍等…'
        : activeJob?.action === 'look' ? 'Qwen 正在睇張相，可能需要一陣…'
        : activeJob?.action === 'chat' ? 'Qwen 正在回覆…' : '正在處理上一個請求…';
      setActivity(message, 'busy');
    } else if (chatTooLong) {
      setActivity('訊息太長，請分開幾次傳送。', 'error');
    } else if (transientError) {
      setActivity(transientError, 'error');
    } else if (!serverAvailable) {
      setActivity(currentState ? '工作台已斷線，正在重新連接…' : '正在連接工作台…', currentState ? 'error' : 'ready');
    } else if (!deviceReady) {
      setActivity('ESP32 未連接，可喺右上角更新位址。', 'error');
    } else if (!modelReady) {
      setActivity('請先啟動 Mac 上嘅 Qwen 模型。', 'error');
    } else if (!bridgeReady) {
      setActivity('Mac 連接未就緒，請啟動 ESPClaw。', 'error');
    } else {
      setActivity(mode === 'look' ? '已準備好 · 按掣先會拍攝' : '已準備好 · 今次只傳送文字');
    }
  }

  function renderConnections() {
    if (!currentState || !serverAvailable) {
      const label = currentState ? '未連接' : '連接中';
      const state = currentState ? 'offline' : 'pending';
      setStatus(elements.deviceStatus, state, `ESP32 ${label}`);
      setStatus(elements.modelStatus, state, `模型${label}`);
      elements.modelIndicator.classList.remove('online');
      return;
    }
    const device = currentState.device || {};
    const model = currentState.model || {};
    setStatus(elements.deviceStatus, device.connected ? 'online' : 'offline', device.connected ? 'ESP32 已連接' : 'ESP32 未連接');
    setStatus(elements.modelStatus, model.connected ? 'online' : 'offline', model.connected ? '模型已就緒' : '模型未連接');
    elements.deviceStatus.title = device.error || '';
    elements.deviceName.textContent = device.name || 'XIAO ESP32S3 SENSE';
    elements.deviceAddress.textContent = device.ip ? `ESP32 / ${device.ip}` : '尚未設定 ESP32 位址';
    elements.modelName.textContent = model.name || (model.connected ? '本地模型' : '等待模型連接');
    elements.modelIndicator.classList.toggle('online', Boolean(model.connected));
    elements.settingsModel.textContent = model.connected ? (model.name || '已連接') : '未連接';
    elements.settingsBridge.textContent = currentState.bridge?.connected ? '已連接' : '未連接';
    if (!elements.settings.open && device.ip) elements.boardHost.value = device.ip;
  }

  function renderImage() {
    const latest = currentState?.latest_image;
    const url = imageUrl(latest?.url);
    if (!url) {
      if (currentState) {
        displayedImageId = null;
        elements.cameraImage.removeAttribute('src');
        elements.largeImage.removeAttribute('src');
        elements.imagePreview.hidden = true;
        elements.cameraEmpty.hidden = false;
        elements.imageCounter.textContent = 'NO IMAGE YET';
        elements.imageTimestamp.textContent = '等待第一張相片';
        if (elements.imageDialog.open) elements.imageDialog.close();
      }
      return;
    }
    const imageId = String(latest.id ?? url);
    if (imageId !== displayedImageId) {
      displayedImageId = imageId;
      failedImageUrl = null;
      elements.cameraImage.src = url;
      elements.largeImage.src = url;
    }
    const valid = failedImageUrl !== url;
    elements.imagePreview.hidden = !valid;
    elements.cameraEmpty.hidden = valid;
    const timestamp = formatTime(latest.created_at, true);
    elements.imageCounter.textContent = 'LATEST CAPTURE';
    elements.imageTimestamp.textContent = valid ? (timestamp || '最新相片') : '相片未能載入，請再拍一張';
    elements.largeImageTime.textContent = timestamp;
  }

  function createEntry(entry) {
    const article = document.createElement('article');
    article.className = 'entry';
    const heading = document.createElement('div');
    heading.className = 'entry-heading';
    heading.append(document.createElement('strong'), document.createElement('time'));
    const text = document.createElement('p');
    text.className = 'entry-text';
    article.append(heading, text);
    updateEntry(article, entry);
    return article;
  }

  function updateEntry(article, entry) {
    const kind = ['user', 'assistant', 'error'].includes(entry.kind) ? entry.kind : 'assistant';
    article.dataset.kind = kind;
    article.firstElementChild.firstElementChild.textContent = kind === 'user' ? '你' : kind === 'error' ? '未能完成' : 'Qwen';
    article.firstElementChild.lastElementChild.textContent = formatTime(entry.created_at);
    article.children[1].textContent = typeof entry.text === 'string' ? entry.text : '';
    const url = imageUrl(entry.image_url);
    let image = article.querySelector('img');
    if (url) {
      if (!image) {
        image = document.createElement('img');
        image.className = 'entry-image';
        image.alt = '這次對話的相片';
        image.loading = 'lazy';
        article.append(image);
      }
      if (image.getAttribute('src') !== url) image.src = url;
    } else if (image) image.remove();
  }

  function renderEntries() {
    const entries = Array.isArray(currentState?.entries) ? currentState.entries : [];
    const wasAtBottom = elements.messages.scrollHeight - elements.messages.scrollTop - elements.messages.clientHeight < 90;
    const hadEntries = entryNodes.size > 0;
    let added = false;
    const present = new Set();
    entries.forEach((entry, index) => {
      const id = String(entry.id ?? `entry-${index}`);
      present.add(id);
      const signature = JSON.stringify([entry.kind, entry.text, entry.created_at, entry.image_url]);
      const existing = entryNodes.get(id);
      if (!existing) {
        const node = createEntry(entry);
        entryNodes.set(id, { node, signature });
        elements.messages.append(node);
        added = true;
      } else if (signature !== existing.signature) {
        updateEntry(existing.node, entry);
        existing.signature = signature;
      }
    });
    entryNodes.forEach((value, id) => {
      if (!present.has(id)) { value.node.remove(); entryNodes.delete(id); }
    });
    elements.emptyConversation.hidden = entries.length > 0;
    if (added && (wasAtBottom || !hadEntries)) elements.messages.scrollTop = elements.messages.scrollHeight;
  }

  function render() {
    renderConnections();
    renderImage();
    renderCameraSettings();
    renderPreview();
    renderEntries();
    renderAvailability();
  }

  async function refreshState() {
    if (pollInFlight) return;
    pollInFlight = true;
    clearTimeout(stateTimer);
    try {
      const result = await api('/api/state');
      if (!result || typeof result !== 'object') throw new Error('工作台回應未能讀取。');
      currentState = result;
      serverAvailable = true;
      elements.notice.hidden = true;
      reconcilePreview();
      render();
      if (!cameraSettingsReadAttempted && !cameraSettings && currentState.device?.connected) refreshCameraSettings();
    } catch (error) {
      serverAvailable = false;
      elements.notice.textContent = readableError(error);
      elements.notice.hidden = false;
      renderConnections();
      renderCameraSettings();
      renderAvailability();
    } finally {
      pollInFlight = false;
      stateTimer = setTimeout(refreshState, document.hidden ? 8000 : 2500);
    }
  }

  async function pollJob() {
    if (!activeJob) return;
    clearTimeout(jobTimer);
    const job = activeJob;
    try {
      const result = await api(`/api/jobs/${encodeURIComponent(job.id)}`);
      if (activeJob !== job) return;
      if (result.status === 'done' || result.status === 'error') {
        activeJob = null;
        transientError = result.status === 'error' ? (typeof result.error === 'string' ? result.error : '今次未能完成，請再試一次。') : '';
        if (transientError) showToast(transientError, true);
        else if (job.action === 'capture') showToast('新相片已收到。');
        await refreshState();
        renderAvailability();
        return;
      }
      // Keep polling while a camera request or inference is running.
    } catch (error) {
      if (error.status === 404) {
        activeJob = null;
        transientError = '工作台已重啟，請重新拍攝或傳送訊息。';
        showToast(transientError, true);
        await refreshState();
        renderAvailability();
        return;
      }
      setActivity('連接暫時中斷，正在等候工作台回應…', 'error');
    }
    if (activeJob === job) jobTimer = setTimeout(pollJob, 1200);
  }

  async function runAction(action) {
    if (requestInFlight || activeJob || currentState?.busy || previewEngaged() || cameraSettingsSaving) return;
    const prompt = action === 'capture' ? '' : elements.prompt.value.trim();
    if (action === 'chat' && !prompt) { elements.prompt.focus(); return; }
    requestInFlight = true;
    transientError = '';
    renderAvailability();
    try {
      const result = await api('/api/action', { method: 'POST', body: { action, prompt } });
      if (result.job_id === undefined || result.job_id === null) throw new Error('未收到工作台確認，請先檢查對話記錄。');
      activeJob = { id: String(result.job_id), action };
      if (action !== 'capture') elements.prompt.value = '';
      renderAvailability();
      refreshState();
      pollJob();
    } catch (error) {
      transientError = error.status === 409 ? '上一個請求仲未完成，請稍等。' : readableError(error);
      showToast(transientError, true);
      refreshState();
    } finally {
      requestInFlight = false;
      renderAvailability();
    }
  }

  async function stopPreview({ silent = false, preserveError = false } = {}) {
    ++previewGeneration;
    detachPreview();
    previewPhase = 'stopping';
    render();
    if (previewStopPromise) return previewStopPromise;
    previewStopPromise = (async () => {
      try {
        await api('/api/stream/stop', { method: 'POST', body: {}, timeout: 20000 });
        previewPhase = 'off';
        previewAcknowledged = false;
        if (!preserveError) previewError = '';
        if (currentState?.preview) currentState.preview = { ...currentState.preview, active: false, starting: false, stopping: false };
        if (!silent) showToast('直播已停止。');
      } catch (error) {
        previewError = `未確認直播已停止：${readableError(error)} 可以再按「重試停止」。`;
        if (!silent) showToast(previewError, true);
      } finally {
        previewStopPromise = null;
        render();
        if (!pageLeaving) refreshState();
      }
    })();
    render();
    return previewStopPromise;
  }

  async function startPreview() {
    if (previewEngaged() || requestInFlight || activeJob || currentState?.busy || cameraSettingsSaving || !serverAvailable || !currentState?.device?.connected) return;
    const generation = ++previewGeneration;
    previewPhase = 'starting';
    previewError = '';
    previewAcknowledged = false;
    transientError = '';
    if (elements.imageDialog.open) elements.imageDialog.close();
    render();
    try {
      const result = await api('/api/stream/start', { method: 'POST', body: {} });
      if (generation !== previewGeneration || pageLeaving) {
        // A stop may race with the start response. Stop the accepted lease too.
        if (previewStopPromise) await previewStopPromise;
        await stopPreview({ silent: true });
        return;
      }
      const url = streamUrl(result.stream_url);
      if (!url) throw new Error('未收到有效的直播位址。');
      previewUrl = url;
      elements.streamImage.src = url; // Only this explicit click path assigns a stream source.
      previewStartTimer = setTimeout(() => {
        if (previewPhase !== 'starting' || generation !== previewGeneration) return;
        previewError = '未收到直播畫面，已要求停止相機。請檢查連接後再試。';
        showToast(previewError, true);
        stopPreview({ silent: true, preserveError: true });
      }, 30000);
      render();
      refreshState();
    } catch (error) {
      if (generation !== previewGeneration || pageLeaving) return;
      previewError = readableError(error);
      showToast(previewError, true);
      await stopPreview({ silent: true, preserveError: true });
    }
  }

  function stopPreviewOnLeave() {
    pageLeaving = true;
    const ownsPreview = previewPhase !== 'off' || Boolean(previewUrl);
    ++previewGeneration;
    detachPreview();
    if (!ownsPreview || !currentState?.csrf_token) return;
    // keepalive preserves the stop request when navigating or closing the tab.
    fetch('/api/stream/stop', {
      method: 'POST', credentials: 'same-origin', keepalive: true,
      headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': currentState.csrf_token },
      body: '{}',
    }).catch(() => {}); // The server also stops the camera when the viewer disconnects.
    previewPhase = 'off';
    previewAcknowledged = false;
  }

  function changeMode(nextMode) {
    mode = nextMode;
    elements.modeLook.setAttribute('aria-pressed', String(mode === 'look'));
    elements.modeChat.setAttribute('aria-pressed', String(mode === 'chat'));
    elements.prompt.placeholder = mode === 'look' ? '例如：張相入面有啲咩？' : '輸入你想問 ESPClaw 嘅問題…';
    elements.promptHint.textContent = mode === 'look'
      ? '按下後會拍攝一張新相片，再交畀 Qwen 分析。'
      : '只傳送文字，今次唔會使用相機。';
    transientError = '';
    renderAvailability();
  }

  function privateIpv4(value) {
    if (!/^\d{1,3}(\.\d{1,3}){3}$/.test(value)) return false;
    const parts = value.split('.').map(Number);
    if (parts.some((number) => number > 255)) return false;
    return parts[0] === 10 || (parts[0] === 172 && parts[1] >= 16 && parts[1] <= 31) || (parts[0] === 192 && parts[1] === 168);
  }

  elements.modeLook.addEventListener('click', () => changeMode('look'));
  elements.modeChat.addEventListener('click', () => changeMode('chat'));
  elements.capture.addEventListener('click', () => runAction('capture'));
  elements.streamButton.addEventListener('click', () => {
    if (previewEngaged()) stopPreview();
    else startPreview();
  });
  $('composer').addEventListener('submit', (event) => {
    event.preventDefault();
    if (!elements.submit.disabled) runAction(mode);
  });
  elements.prompt.addEventListener('input', renderAvailability);
  elements.prompt.addEventListener('keydown', (event) => {
    if ((event.metaKey || event.ctrlKey) && event.key === 'Enter' && !event.isComposing) {
      event.preventDefault();
      if (!elements.submit.disabled) runAction(mode);
    }
  });
  if (!/Mac|iPhone|iPad/.test(navigator.platform)) $('keyboard-hint').textContent = 'Ctrl ↵';
  $('sample-prompt').addEventListener('click', () => {
    if (requestInFlight || activeJob || currentState?.busy || previewEngaged()) return;
    changeMode('look');
    elements.prompt.value = '張相入面有啲咩？';
    elements.prompt.focus();
    renderAvailability();
  });
  function openSettings(camera = false) {
    elements.settingsError.hidden = true;
    cameraSettingsDirty = false;
    cameraSettingsError = '';
    if (currentState?.device?.ip) elements.boardHost.value = currentState.device.ip;
    elements.settings.showModal();
    renderCameraSettings();
    refreshCameraSettings();
    if (camera) {
      $('camera-tuning-title').scrollIntoView({ block: 'start' });
      $('camera-tuning-title').focus({ preventScroll: true });
    } else elements.boardHost.focus();
  }
  $('open-settings').addEventListener('click', () => openSettings());
  $('open-camera-settings').addEventListener('click', () => openSettings(true));
  elements.reloadCameraSettings.addEventListener('click', refreshCameraSettings);
  elements.cameraSettingsForm.addEventListener('submit', saveCameraSettings);
  [elements.cameraFps, elements.cameraFlicker, elements.cameraWb, elements.cameraBrightness, elements.cameraSaturation].forEach((input) => {
    input.addEventListener('input', () => {
      cameraSettingsDirty = true;
      cameraSettingsError = '';
      cameraSettingsNote = '';
      updateAdjustmentLabels();
      renderCameraSettings();
    });
  });
  $('close-settings').addEventListener('click', () => elements.settings.close());
  $('settings-form').addEventListener('submit', async (event) => {
    event.preventDefault();
    const boardHost = elements.boardHost.value.trim();
    if (!privateIpv4(boardHost)) {
      elements.settingsError.textContent = '請填入有效的本地 IPv4 位址，例如 192.168.1.25。';
      elements.settingsError.hidden = false;
      elements.boardHost.focus();
      return;
    }
    elements.saveSettings.disabled = true;
    elements.saveSettings.textContent = '正在儲存…';
    elements.settingsError.hidden = true;
    try {
      await api('/api/settings', { method: 'POST', body: { board_host: boardHost } });
      elements.settings.close();
      transientError = '';
      showToast('已更新 ESP32 位址。');
      await refreshState();
    } catch (error) {
      elements.settingsError.textContent = readableError(error);
      elements.settingsError.hidden = false;
    } finally {
      elements.saveSettings.disabled = false;
      elements.saveSettings.textContent = '儲存位址';
    }
  });
  elements.imagePreview.addEventListener('click', () => {
    if (elements.cameraImage.getAttribute('src')) elements.imageDialog.showModal();
  });
  $('close-image').addEventListener('click', () => elements.imageDialog.close());
  [elements.settings, elements.imageDialog].forEach((dialog) => {
    dialog.addEventListener('click', (event) => {
      const rect = dialog.getBoundingClientRect();
      if (event.target === dialog && (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom)) dialog.close();
    });
  });
  elements.cameraImage.addEventListener('error', () => {
    failedImageUrl = elements.cameraImage.getAttribute('src');
    elements.imagePreview.hidden = true;
    elements.cameraEmpty.hidden = false;
    elements.imageTimestamp.textContent = '相片未能載入，請再拍一張';
  });
  elements.streamImage.addEventListener('error', () => {
    if (!previewUrl || pageLeaving || previewPhase === 'stopping') return;
    previewError = '直播連接已中斷，已要求停止相機。按「開始直播」先會重新連接。';
    showToast(previewError, true);
    stopPreview({ silent: true, preserveError: true });
  });
  window.addEventListener('pagehide', stopPreviewOnLeave);
  window.addEventListener('pageshow', () => {
    pageLeaving = false;
    // Restoring a tab must never restore an MJPEG source or restart the camera.
    render();
    refreshState();
  });
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden) refreshState();
  });
  window.addEventListener('online', refreshState);
  refreshState();
})();
