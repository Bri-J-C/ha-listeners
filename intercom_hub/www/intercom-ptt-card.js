/**
 * Intercom PTT - Custom Lovelace Card
 *
 * Push-to-talk intercom card for Home Assistant dashboards.
 * Connects to the Intercom Hub add-on via ingress WebSocket.
 *
 * Installation:
 *   1. Copy this file to /config/www/intercom-ptt-card.js
 *   2. Add resource: Settings > Dashboards > Resources > /local/intercom-ptt-card.js (JavaScript Module)
 *   3. Add card to dashboard with type: custom:intercom-ptt-card
 *
 * Card config:
 *   type: custom:intercom-ptt-card
 *   default_target: "Bedroom"   # Optional: pre-select target room
 *   name: "Intercom"            # Optional: card title
 *   show_call: true             # Optional: show call button (default true)
 */

// AudioWorklet processor code (inlined for Blob URL — can't use relative path in Lovelace)
const PTT_PROCESSOR_CODE = `
class PTTProcessor extends AudioWorkletProcessor {
    constructor() {
        super();
        this.frameSize = 320;
        this.buffer = new Float32Array(this.frameSize);
        this.bufferIndex = 0;
        this.transmitting = false;
        this.port.onmessage = (event) => {
            if (event.data.type === 'ptt') {
                this.transmitting = event.data.active;
                if (!event.data.active) {
                    this.buffer = new Float32Array(this.frameSize);
                    this.bufferIndex = 0;
                }
            }
        };
    }
    process(inputs, outputs, parameters) {
        const input = inputs[0];
        if (!input || !input[0] || !this.transmitting) return true;
        const channel = input[0];
        for (let i = 0; i < channel.length; i++) {
            this.buffer[this.bufferIndex++] = channel[i];
            if (this.bufferIndex >= this.frameSize) {
                const int16 = new Int16Array(this.frameSize);
                for (let j = 0; j < this.frameSize; j++) {
                    const sample = Math.max(-1, Math.min(1, this.buffer[j]));
                    int16[j] = sample < 0 ? sample * 0x8000 : sample * 0x7FFF;
                }
                this.port.postMessage({ type: 'audio', pcm: int16.buffer }, [int16.buffer]);
                this.buffer = new Float32Array(this.frameSize);
                this.bufferIndex = 0;
            }
        }
        return true;
    }
}
registerProcessor('ptt-processor', PTTProcessor);
`;

const ADDON_SLUG = 'local_intercom_hub';

const CARD_STYLES = `
    :host {
        --cyan: #00D4FF;
        --purple: #6366F1;
        --gradient: linear-gradient(135deg, var(--cyan), var(--purple));
        --bg-dark: #0a0a1a;
        --bg-card: rgba(255, 255, 255, 0.05);
        --border: rgba(255, 255, 255, 0.1);
        --text: #ffffff;
        --text-dim: rgba(255, 255, 255, 0.5);
        --success: #10B981;
        --warning: #F59E0B;
        --error: #EF4444;
    }
    * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }

    .card-container {
        background: var(--bg-dark);
        background-image:
            radial-gradient(ellipse at top left, rgba(0, 212, 255, 0.08) 0%, transparent 50%),
            radial-gradient(ellipse at bottom right, rgba(99, 102, 241, 0.08) 0%, transparent 50%);
        border-radius: var(--ha-card-border-radius, 12px);
        padding: 20px;
        color: var(--text);
        font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
    }

    .header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        margin-bottom: 16px;
    }
    .title {
        font-size: 16px;
        font-weight: 600;
        background: var(--gradient);
        -webkit-background-clip: text;
        -webkit-text-fill-color: transparent;
        background-clip: text;
    }
    .version {
        font-size: 10px;
        color: rgba(255,255,255,0.2);
    }

    .status-bar {
        display: flex;
        justify-content: space-between;
        margin-bottom: 16px;
        padding: 10px 14px;
        background: var(--bg-card);
        border: 1px solid var(--border);
        border-radius: 10px;
        font-size: 13px;
    }
    .status-item {
        display: flex;
        align-items: center;
        gap: 6px;
    }
    .status-label { color: var(--text-dim); }
    .status-dot {
        width: 7px; height: 7px; border-radius: 50%;
        animation: pulse 2s ease-in-out infinite;
    }
    @keyframes pulse {
        0%, 100% { opacity: 1; }
        50% { opacity: 0.5; }
    }
    .dot-connected { background: var(--success); box-shadow: 0 0 6px var(--success); }
    .dot-disconnected { background: var(--error); box-shadow: 0 0 6px var(--error); }
    .dot-idle { background: var(--success); box-shadow: 0 0 6px var(--success); }
    .dot-transmitting { background: var(--cyan); box-shadow: 0 0 6px var(--cyan); animation: pulse 0.5s ease-in-out infinite; }
    .dot-receiving { background: var(--success); box-shadow: 0 0 6px var(--success); animation: pulse 0.5s ease-in-out infinite; }
    .dot-busy { background: var(--warning); box-shadow: 0 0 6px var(--warning); }
    .text-connected { color: var(--text); }
    .text-disconnected { color: var(--error); }
    .text-transmitting { color: var(--cyan); }
    .text-receiving { color: var(--success); }
    .text-idle { color: var(--text); }

    .target-row {
        display: flex;
        gap: 10px;
        margin-bottom: 16px;
    }
    .target-select {
        flex: 1;
        padding: 10px 12px;
        border-radius: 10px;
        border: 1px solid var(--border);
        background: var(--bg-card);
        color: var(--text);
        font-size: 14px;
        cursor: pointer;
        appearance: none;
        background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='rgba(255,255,255,0.5)' stroke-width='2'%3E%3Cpath d='M6 9l6 6 6-6'/%3E%3C/svg%3E");
        background-repeat: no-repeat;
        background-position: right 10px center;
    }
    .target-select:focus {
        outline: none;
        border-color: var(--cyan);
        box-shadow: 0 0 0 2px rgba(0, 212, 255, 0.15);
    }
    .target-select option {
        background: #1a1a2e;
        color: var(--text);
    }

    .call-btn {
        background: transparent;
        border: 1px solid var(--border);
        border-radius: 10px;
        color: var(--text-dim);
        font-size: 13px;
        font-weight: 600;
        padding: 10px 14px;
        cursor: pointer;
        transition: all 0.2s;
    }
    .call-btn:hover:not(:disabled) {
        border-color: var(--cyan);
        color: var(--cyan);
    }
    .call-btn:disabled { opacity: 0.3; cursor: not-allowed; }
    .call-btn.calling {
        border-color: var(--success);
        color: var(--success);
        box-shadow: 0 0 8px rgba(16, 185, 129, 0.3);
    }

    .ptt-container {
        display: flex;
        justify-content: center;
        margin: 8px 0;
    }
    .ptt-button {
        background: transparent;
        border: none;
        cursor: pointer;
        padding: 12px;
        display: flex;
        align-items: center;
        justify-content: center;
        transition: all 0.2s;
        user-select: none;
        -webkit-user-select: none;
        touch-action: manipulation;
    }
    .ptt-button:active, .ptt-button.active { transform: scale(0.92); }
    .ptt-button.disabled { opacity: 0.3; cursor: not-allowed; }
    .ptt-icon {
        width: 100px; height: 100px;
        color: var(--text-dim);
        transition: color 0.2s, filter 0.2s;
    }
    .ptt-button:hover:not(.disabled) .ptt-icon { color: var(--text); }
    .ptt-button.transmitting .ptt-icon {
        color: var(--cyan);
        filter: drop-shadow(0 0 16px rgba(0, 212, 255, 0.6));
    }
    .ptt-button.receiving .ptt-icon {
        color: var(--success);
        filter: drop-shadow(0 0 16px rgba(16, 185, 129, 0.6));
    }
    .ptt-button.busy .ptt-icon {
        color: var(--warning);
        filter: drop-shadow(0 0 12px rgba(245, 158, 11, 0.5));
    }

    .instructions {
        text-align: center;
        color: var(--text-dim);
        font-size: 11px;
        margin: 4px 0 0;
    }

    .error-banner {
        background: rgba(239, 68, 68, 0.1);
        border: 1px solid rgba(239, 68, 68, 0.3);
        border-radius: 8px;
        padding: 10px 12px;
        margin-bottom: 12px;
        color: #FCA5A5;
        font-size: 13px;
        display: none;
    }
    .error-banner.visible { display: block; }

    .init-overlay {
        display: flex;
        flex-direction: column;
        align-items: center;
        justify-content: center;
        padding: 32px 20px;
    }
    .init-overlay.hidden { display: none; }
    .init-title {
        font-size: 20px;
        font-weight: 700;
        margin-bottom: 8px;
        background: var(--gradient);
        -webkit-background-clip: text;
        -webkit-text-fill-color: transparent;
        background-clip: text;
    }
    .init-subtitle {
        color: var(--text-dim);
        font-size: 13px;
        margin-bottom: 20px;
    }
    .init-button {
        padding: 12px 36px;
        font-size: 15px;
        border-radius: 50px;
        border: none;
        background: var(--gradient);
        color: white;
        cursor: pointer;
        font-weight: 600;
        transition: all 0.2s;
        box-shadow: 0 4px 16px rgba(0, 212, 255, 0.3);
    }
    .init-button:hover {
        transform: translateY(-1px);
        box-shadow: 0 6px 20px rgba(0, 212, 255, 0.4);
    }
    .main-ui { display: none; }
    .main-ui.visible { display: block; }
`;

const PTT_ICON_SVG = `<svg class="ptt-icon" viewBox="0 0 512 512" fill="none">
    <rect x="32" y="32" width="448" height="448" rx="72" stroke="currentColor" stroke-width="48"/>
    <rect x="116" y="140" width="36" height="140" rx="18" fill="currentColor"/>
    <rect x="178" y="110" width="36" height="200" rx="18" fill="currentColor"/>
    <rect x="238" y="95" width="36" height="230" rx="18" fill="currentColor"/>
    <rect x="298" y="110" width="36" height="200" rx="18" fill="currentColor"/>
    <rect x="360" y="140" width="36" height="140" rx="18" fill="currentColor"/>
    <path d="M140 370 Q256 440 372 370" stroke="currentColor" stroke-width="32" stroke-linecap="round" fill="none"/>
</svg>`;

class IntercomPTTCard extends HTMLElement {
    constructor() {
        super();
        this._config = {};
        this._hass = null;
        this._rendered = false;

        // Audio state
        this._audioContext = null;
        this._mediaStream = null;
        this._workletNode = null;
        this._mediaSource = null;
        this._websocket = null;
        this._isConnected = false;
        this._isTransmitting = false;
        this._isReceiving = false;
        this._isInitialized = false;
        this._micEnabled = false;
        this._nextPlayTime = 0;
        this._clientId = null;
        this._reconnectTimer = null;
        this._ingressSession = null;
    }

    static getConfigElement() {
        return document.createElement('intercom-ptt-card-editor');
    }

    static getStubConfig() {
        return { default_target: '', name: 'Intercom', show_call: true };
    }

    setConfig(config) {
        this._config = {
            default_target: config.default_target || '',
            name: config.name || 'Intercom',
            show_call: config.show_call !== false,
            ...config
        };
        if (this._rendered) this._updateHeader();
    }

    set hass(hass) {
        this._hass = hass;
        if (!this._rendered) {
            this._render();
            this._rendered = true;
        }
    }

    getCardSize() {
        return 4;
    }

    _render() {
        this.attachShadow({ mode: 'open' });

        this.shadowRoot.innerHTML = `
            <style>${CARD_STYLES}</style>
            <div class="card-container">
                <div class="init-overlay" id="initOverlay">
                    <div class="init-title">${this._config.name}</div>
                    <div class="init-subtitle">Push-to-talk communication</div>
                    <button class="init-button" id="initBtn">Tap to Connect</button>
                </div>
                <div class="main-ui" id="mainUI">
                    <div class="header">
                        <span class="title" id="cardTitle">${this._config.name}</span>
                        <span class="version" id="versionLabel">v--</span>
                    </div>
                    <div id="errorBanner" class="error-banner"></div>
                    <div class="status-bar">
                        <div class="status-item">
                            <span class="status-label">Conn</span>
                            <span class="status-dot dot-disconnected" id="connDot"></span>
                            <span class="text-disconnected" id="connText">Off</span>
                        </div>
                        <div class="status-item">
                            <span class="status-label">Status</span>
                            <span class="status-dot dot-idle" id="statusDot"></span>
                            <span class="text-idle" id="statusText">Idle</span>
                        </div>
                    </div>
                    <div class="target-row">
                        <select class="target-select" id="targetSelect">
                            <option value="all">All Rooms</option>
                        </select>
                        ${this._config.show_call ? '<button class="call-btn" id="callBtn" disabled>Call</button>' : ''}
                    </div>
                    <div class="ptt-container">
                        <button class="ptt-button disabled" id="pttBtn">
                            ${PTT_ICON_SVG}
                        </button>
                    </div>
                    <div class="instructions">Hold to talk &middot; Release to stop</div>
                </div>
            </div>
        `;

        // Bind elements
        this._els = {
            initOverlay: this.shadowRoot.getElementById('initOverlay'),
            mainUI: this.shadowRoot.getElementById('mainUI'),
            initBtn: this.shadowRoot.getElementById('initBtn'),
            pttBtn: this.shadowRoot.getElementById('pttBtn'),
            targetSelect: this.shadowRoot.getElementById('targetSelect'),
            callBtn: this.shadowRoot.getElementById('callBtn'),
            connDot: this.shadowRoot.getElementById('connDot'),
            connText: this.shadowRoot.getElementById('connText'),
            statusDot: this.shadowRoot.getElementById('statusDot'),
            statusText: this.shadowRoot.getElementById('statusText'),
            errorBanner: this.shadowRoot.getElementById('errorBanner'),
            versionLabel: this.shadowRoot.getElementById('versionLabel'),
            cardTitle: this.shadowRoot.getElementById('cardTitle'),
        };

        // Generate client ID
        this._clientId = this._getClientId();

        // Event listeners
        this._els.initBtn.addEventListener('click', () => this._initialize());

        // PTT - pointer events for unified touch/mouse
        this._els.pttBtn.addEventListener('pointerdown', (e) => {
            e.preventDefault();
            this._els.pttBtn.setPointerCapture(e.pointerId);
            this._startTransmit();
        });
        this._els.pttBtn.addEventListener('pointerup', (e) => {
            e.preventDefault();
            this._stopTransmit();
        });
        this._els.pttBtn.addEventListener('pointercancel', (e) => {
            e.preventDefault();
            this._stopTransmit();
        });
        this._els.pttBtn.addEventListener('pointerleave', (e) => {
            if (!this._els.pttBtn.hasPointerCapture(e.pointerId)) {
                this._stopTransmit();
            }
        });

        // Target selection
        this._els.targetSelect.addEventListener('change', () => this._sendTargetChange());

        // Call button
        if (this._els.callBtn) {
            this._els.callBtn.addEventListener('click', () => this._sendCall());
        }

        // Visibility change
        this._boundVisChange = () => {
            if (document.hidden) this._suspend();
            else this._resume();
        };
        document.addEventListener('visibilitychange', this._boundVisChange);
    }

    disconnectedCallback() {
        // Clean up when card is removed
        this._suspend();
        if (this._boundVisChange) {
            document.removeEventListener('visibilitychange', this._boundVisChange);
        }
    }

    _updateHeader() {
        if (this._els?.cardTitle) {
            this._els.cardTitle.textContent = this._config.name;
        }
    }

    _getClientId() {
        const key = 'intercom_card_client_id';
        let id = localStorage.getItem(key);
        if (!id) {
            const rand = Math.random().toString(16).substring(2, 6).toUpperCase();
            id = `Dashboard_${rand}`;
            localStorage.setItem(key, id);
        }
        return id;
    }

    // --- Ingress Session ---

    async _getIngressSession() {
        // Method 1: HA WebSocket API (most reliable — uses existing authenticated connection)
        try {
            const result = await this._hass.callWS({
                type: 'supervisor/api',
                endpoint: '/ingress/session',
                method: 'post',
                data: { addon: ADDON_SLUG }
            });
            if (result?.session) {
                console.log('Intercom card: got ingress session via WS');
                return result.session;
            }
        } catch (e) {
            console.warn('Intercom card: WS ingress method failed:', e.message || e);
        }

        // Method 2: REST API with auth token
        try {
            const token = this._hass?.auth?.data?.access_token;
            if (token) {
                const resp = await fetch('/api/hassio/ingress/session', {
                    method: 'POST',
                    headers: {
                        'Authorization': `Bearer ${token}`,
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify({ addon: ADDON_SLUG })
                });
                if (resp.ok) {
                    const data = await resp.json();
                    const session = data?.data?.session;
                    if (session) {
                        console.log('Intercom card: got ingress session via REST');
                        return session;
                    }
                }
                console.warn('Intercom card: REST ingress failed:', resp.status);
            }
        } catch (e) {
            console.warn('Intercom card: REST ingress method failed:', e.message || e);
        }

        throw new Error('Could not get ingress session. Are you logged in as an admin?');
    }

    // --- Initialization ---

    async _initialize() {
        try {
            // Get ingress session first
            this._ingressSession = await this._getIngressSession();

            // Setup mic if secure context
            if (window.isSecureContext) {
                await this._setupMic();
            } else {
                this._showError('HTTPS required for mic. Listen-only mode.');
                this._audioContext = new AudioContext({ sampleRate: 16000 });
            }

            // Show main UI
            this._els.initOverlay.classList.add('hidden');
            this._els.mainUI.classList.add('visible');
            this._isInitialized = true;

            // Connect WebSocket
            this._connectWebSocket();

        } catch (err) {
            console.error('Intercom init error:', err);
            if (err.name === 'NotAllowedError') {
                this._showError('Microphone permission denied.');
            } else {
                this._showError('Init failed: ' + err.message);
            }
            // Try receive-only
            this._els.initOverlay.classList.add('hidden');
            this._els.mainUI.classList.add('visible');
            this._isInitialized = true;
            this._micEnabled = false;
            try {
                this._ingressSession = this._ingressSession || await this._getIngressSession();
                this._audioContext = new AudioContext({ sampleRate: 16000 });
                this._connectWebSocket();
            } catch (e2) {
                console.error('Intercom card: all connection methods failed:', e2);
                this._showError('Cannot connect to intercom hub: ' + (e2.message || 'unknown error'));
            }
        }
    }

    // --- Audio Setup ---

    async _setupMic() {
        this._mediaStream = await navigator.mediaDevices.getUserMedia({
            audio: {
                echoCancellation: true,
                noiseSuppression: true,
                autoGainControl: true,
                sampleRate: 16000,
                channelCount: 1
            }
        });

        this._audioContext = new AudioContext({ sampleRate: 16000 });

        // Load AudioWorklet via Blob URL
        const blob = new Blob([PTT_PROCESSOR_CODE], { type: 'application/javascript' });
        const url = URL.createObjectURL(blob);
        await this._audioContext.audioWorklet.addModule(url);
        URL.revokeObjectURL(url);

        this._workletNode = new AudioWorkletNode(this._audioContext, 'ptt-processor');
        this._workletNode.port.onmessage = (event) => {
            if (event.data.type === 'audio' && this._isConnected && this._isTransmitting) {
                this._sendAudio(event.data.pcm);
            }
        };

        this._mediaSource = this._audioContext.createMediaStreamSource(this._mediaStream);
        this._mediaSource.connect(this._workletNode);
        this._micEnabled = true;
    }

    _releaseMic() {
        if (this._mediaSource) {
            this._mediaSource.disconnect();
            this._mediaSource = null;
        }
        if (this._workletNode) {
            this._workletNode.disconnect();
            this._workletNode = null;
        }
        if (this._mediaStream) {
            this._mediaStream.getTracks().forEach(t => t.stop());
            this._mediaStream = null;
        }
        this._micEnabled = false;
    }

    // --- WebSocket ---

    _connectWebSocket() {
        if (!this._ingressSession) return;

        const wsUrl = `wss://${location.host}/api/hassio_ingress/${this._ingressSession}/ws`;
        console.log('Intercom card: connecting to', wsUrl);

        this._websocket = new WebSocket(wsUrl);
        this._websocket.binaryType = 'arraybuffer';

        this._websocket.onopen = () => {
            console.log('Intercom card: WebSocket connected');
            this._isConnected = true;
            this._updateConnStatus(true);
            this._els.pttBtn.classList.remove('disabled');
            if (this._els.callBtn) this._els.callBtn.disabled = false;
            this._hideError();

            // Identify
            if (this._clientId) {
                this._websocket.send(JSON.stringify({
                    type: 'identify',
                    client_id: this._clientId
                }));
            }
            // Request state
            this._websocket.send(JSON.stringify({ type: 'get_state' }));
        };

        this._websocket.onclose = () => {
            console.log('Intercom card: WebSocket disconnected');
            this._isConnected = false;
            this._updateConnStatus(false);
            this._els.pttBtn.classList.add('disabled');
            if (this._els.callBtn) this._els.callBtn.disabled = true;
            this._updatePttStatus('idle');

            // Reconnect
            if (this._isInitialized && !document.hidden) {
                this._reconnectTimer = setTimeout(async () => {
                    try {
                        this._ingressSession = await this._getIngressSession();
                        this._connectWebSocket();
                    } catch (e) {
                        console.error('Intercom card: reconnect failed', e);
                        this._reconnectTimer = setTimeout(() => this._connectWebSocket(), 5000);
                    }
                }, 3000);
            }
        };

        this._websocket.onerror = (err) => {
            console.error('Intercom card: WebSocket error', err);
            this._showError('Connection error. Retrying...');
        };

        this._websocket.onmessage = (event) => {
            if (event.data instanceof ArrayBuffer) {
                this._playAudio(event.data);
            } else {
                try {
                    this._handleMessage(JSON.parse(event.data));
                } catch (e) {
                    console.error('Intercom card: parse error', e);
                }
            }
        };
    }

    _handleMessage(msg) {
        switch (msg.type) {
            case 'init':
                if (msg.version && this._els.versionLabel) {
                    this._els.versionLabel.textContent = `v${msg.version}`;
                }
                if (msg.status) this._updatePttStatus(msg.status);
                break;

            case 'state':
                this._updatePttStatus(msg.status);
                this._isReceiving = msg.status === 'receiving';
                break;

            case 'targets': {
                const select = this._els.targetSelect;
                select.innerHTML = '<option value="all">All Rooms</option>';
                if (msg.rooms) {
                    msg.rooms.forEach(room => {
                        const opt = document.createElement('option');
                        opt.value = room;
                        opt.textContent = room;
                        select.appendChild(opt);
                    });
                }
                // Apply default target from config
                if (this._config.default_target) {
                    const match = Array.from(select.options).find(
                        o => o.value.toLowerCase() === this._config.default_target.toLowerCase()
                    );
                    if (match) {
                        select.value = match.value;
                        this._sendTargetChange();
                    }
                }
                break;
            }

            case 'busy':
                this._els.pttBtn.classList.add('busy');
                this._updatePttStatus('busy');
                break;

            case 'error':
                this._showError(msg.message);
                break;
        }
    }

    // --- PTT ---

    _startTransmit() {
        if (!this._isConnected || this._isTransmitting) return;
        if (!this._micEnabled) {
            this._showError('Microphone not available.');
            return;
        }
        if (this._isReceiving) {
            this._els.pttBtn.classList.add('busy');
            return;
        }

        this._isTransmitting = true;
        this._els.pttBtn.classList.add('active', 'transmitting');
        this._updatePttStatus('transmitting');

        if (this._workletNode) {
            this._workletNode.port.postMessage({ type: 'ptt', active: true });
        }
        if (this._websocket?.readyState === WebSocket.OPEN) {
            this._websocket.send(JSON.stringify({
                type: 'ptt_start',
                target: this._els.targetSelect.value
            }));
        }
    }

    _stopTransmit() {
        this._els.pttBtn.classList.remove('active', 'transmitting', 'busy');
        if (!this._isTransmitting) return;

        this._isTransmitting = false;
        this._updatePttStatus('idle');

        if (this._workletNode) {
            this._workletNode.port.postMessage({ type: 'ptt', active: false });
        }
        if (this._websocket?.readyState === WebSocket.OPEN) {
            this._websocket.send(JSON.stringify({ type: 'ptt_stop' }));
        }
    }

    _sendAudio(pcmBuffer) {
        if (this._websocket?.readyState === WebSocket.OPEN) {
            this._websocket.send(pcmBuffer);
        }
    }

    _sendTargetChange() {
        if (this._websocket?.readyState === WebSocket.OPEN) {
            this._websocket.send(JSON.stringify({
                type: 'set_target',
                target: this._els.targetSelect.value
            }));
        }
    }

    _sendCall() {
        const target = this._els.targetSelect.value;
        if (!this._isConnected || target === 'all') return;

        if (this._websocket?.readyState === WebSocket.OPEN) {
            this._websocket.send(JSON.stringify({ type: 'call', target }));
            this._els.callBtn.classList.add('calling');
            setTimeout(() => this._els.callBtn.classList.remove('calling'), 1000);
        }
    }

    // --- Audio Playback ---

    _playAudio(pcmBuffer) {
        if (!this._audioContext) return;
        if (this._audioContext.state === 'suspended') {
            this._audioContext.resume();
        }

        const int16 = new Int16Array(pcmBuffer);
        const float32 = new Float32Array(int16.length);
        for (let i = 0; i < int16.length; i++) {
            float32[i] = int16[i] / 32768.0;
        }

        const buffer = this._audioContext.createBuffer(1, float32.length, 16000);
        buffer.getChannelData(0).set(float32);

        const source = this._audioContext.createBufferSource();
        source.buffer = buffer;
        source.connect(this._audioContext.destination);

        const now = this._audioContext.currentTime;
        if (this._nextPlayTime < now) {
            this._nextPlayTime = now + 0.02;
        }
        source.start(this._nextPlayTime);
        this._nextPlayTime += buffer.duration;
    }

    // --- Suspend / Resume ---

    _suspend() {
        if (!this._isInitialized) return;
        if (this._isTransmitting) this._stopTransmit();
        this._releaseMic();
        if (this._reconnectTimer) {
            clearTimeout(this._reconnectTimer);
            this._reconnectTimer = null;
        }
        if (this._websocket) {
            this._websocket.onclose = null;
            this._websocket.close();
            this._websocket = null;
        }
        this._isConnected = false;
        if (this._audioContext) {
            this._audioContext.close();
            this._audioContext = null;
        }
        this._updateConnStatus(false);
        this._els.pttBtn.classList.add('disabled');
    }

    async _resume() {
        if (!this._isInitialized) return;
        try {
            this._ingressSession = await this._getIngressSession();
            if (window.isSecureContext) {
                await this._setupMic();
            } else {
                this._audioContext = new AudioContext({ sampleRate: 16000 });
            }
            this._connectWebSocket();
        } catch (err) {
            console.error('Intercom card: resume error', err);
            this._showError('Failed to resume: ' + err.message);
            try {
                this._audioContext = new AudioContext({ sampleRate: 16000 });
                this._connectWebSocket();
            } catch (e2) { /* give up */ }
        }
    }

    // --- UI Updates ---

    _updateConnStatus(connected) {
        if (!this._els) return;
        const dot = this._els.connDot;
        const text = this._els.connText;
        dot.className = `status-dot ${connected ? 'dot-connected' : 'dot-disconnected'}`;
        text.className = connected ? 'text-connected' : 'text-disconnected';
        text.textContent = connected ? 'On' : 'Off';
    }

    _updatePttStatus(status) {
        if (!this._els) return;
        const dot = this._els.statusDot;
        const text = this._els.statusText;
        dot.className = `status-dot dot-${status}`;
        text.className = `text-${status}`;
        text.textContent = status.charAt(0).toUpperCase() + status.slice(1);

        this._els.pttBtn.classList.remove('transmitting', 'receiving', 'busy');
        if (status === 'transmitting') this._els.pttBtn.classList.add('transmitting');
        else if (status === 'receiving') this._els.pttBtn.classList.add('receiving');
    }

    _showError(msg) {
        if (!this._els?.errorBanner) return;
        this._els.errorBanner.textContent = msg;
        this._els.errorBanner.classList.add('visible');
    }

    _hideError() {
        if (!this._els?.errorBanner) return;
        this._els.errorBanner.classList.remove('visible');
    }
}

// --- Card Editor ---

class IntercomPTTCardEditor extends HTMLElement {
    setConfig(config) {
        this._config = config;
        this._render();
    }

    _render() {
        this.innerHTML = `
            <style>
                .editor { padding: 16px; }
                .editor label { display: block; margin-bottom: 4px; font-weight: 500; font-size: 14px; }
                .editor input, .editor select {
                    width: 100%; padding: 8px; margin-bottom: 12px;
                    border: 1px solid var(--divider-color, #e0e0e0);
                    border-radius: 4px; font-size: 14px;
                }
            </style>
            <div class="editor">
                <label>Card Title</label>
                <input id="name" value="${this._config.name || 'Intercom'}" placeholder="Intercom">
                <label>Default Target Room</label>
                <input id="default_target" value="${this._config.default_target || ''}" placeholder="e.g. Bedroom (leave empty for All Rooms)">
                <label>Show Call Button</label>
                <select id="show_call">
                    <option value="true" ${this._config.show_call !== false ? 'selected' : ''}>Yes</option>
                    <option value="false" ${this._config.show_call === false ? 'selected' : ''}>No</option>
                </select>
            </div>
        `;

        this.querySelector('#name').addEventListener('input', (e) => this._update('name', e.target.value));
        this.querySelector('#default_target').addEventListener('input', (e) => this._update('default_target', e.target.value));
        this.querySelector('#show_call').addEventListener('change', (e) => this._update('show_call', e.target.value === 'true'));
    }

    _update(key, value) {
        this._config = { ...this._config, [key]: value };
        const event = new CustomEvent('config-changed', { detail: { config: this._config } });
        this.dispatchEvent(event);
    }
}

// Register elements
customElements.define('intercom-ptt-card', IntercomPTTCard);
customElements.define('intercom-ptt-card-editor', IntercomPTTCardEditor);

// Register with HA card picker
window.customCards = window.customCards || [];
window.customCards.push({
    type: 'intercom-ptt-card',
    name: 'Intercom PTT',
    description: 'Push-to-talk intercom card for dashboard',
    preview: false,
});
