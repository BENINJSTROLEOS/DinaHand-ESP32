// ============================================================
//  DinaHand v3 - Monitor de Fuerza con OLED + WebSocket
//  Novedades v3:
//   - Selector Mano Izquierda / Mano Derecha al guardar
//   - Historial discrimina mano en columna y badge de color
//   - IA puede analizar por separado cada mano
//   - Módulo de Juego AR con 3 niveles (iPhone X, Coca-Cola, Maleta)
//   - QR generados dinámicamente via API (no dependen de imágenes externas)
//   - Niveles se desbloquean secuencialmente y persisten en localStorage
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include "HX711.h"
#include <WebSocketsServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <LittleFS.h>

// ==================== OLED ====================
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define SCREEN_ADDRESS  0x3C
#define I2C_SDA         8
#define I2C_SCL         9

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledOK = false;

// ==================== CELDA DE CARGA ====================
const int   LOADCELL_DOUT_PIN    = 5;
const int   LOADCELL_SCK_PIN     = 4;
const float FACTOR_CALIBRACION   = 51420.0;   // positivo — el abs() en el loop evita lecturas negativas

HX711 scale;
bool  scaleOK = false;

// ==================== RED ====================
const char* ssid     = "NOMBRE WIFI";
const char* password = "CONTRASEÑA";

// ==================== NTP ====================
const char* ntpServer          = "pool.ntp.org";
const long  gmtOffset_sec      = -14400;
const int   daylightOffset_sec = 3600;

// ==================== SERVIDOR ====================
WebServer        server(80);
WebSocketsServer webSocket(81);

const char* HISTORIAL_FILE = "/historial.txt";

// ==================== TIMING ====================
unsigned long previousMillis = 0;
const long    interval       = 100;

// ============================================================
//  HTML PRINCIPAL
// ============================================================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>REGISTRO DINAHAND v3</title>
<style>
  :root {
    --bg:       #0b0f19;
    --surface:  #161b26;
    --border:   #232a3b;
    --deep:     #0f131c;
    --cyan:     #00f5d4;
    --blue:     #00b4d8;
    --red:      #ef4444;
    --yellow:   #f59e0b;
    --green:    #22c55e;
    --purple:   #a78bfa;
    --text:     #f8fafc;
    --muted:    #94a3b8;
  }

  * { box-sizing: border-box; }
  body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: var(--bg); margin: 0; padding: 20px; color: var(--text); }

  /* ── TÍTULO ── */
  .main-title { color: var(--cyan); text-align: center; margin-bottom: 25px; font-weight: 700; letter-spacing: 2px; font-size: 2.2rem; text-transform: uppercase; border-bottom: 3px solid var(--blue); display: inline-block; padding-bottom: 5px; position: relative; left: 50%; transform: translateX(-50%); }

  /* ── LAYOUT ── */
  .dashboard { display: flex; gap: 25px; max-width: 1350px; margin: auto; align-items: flex-start; }
  .col-izquierda { flex: 7; display: flex; flex-direction: column; gap: 20px; }
  .col-derecha   { flex: 5; background: var(--surface); padding: 20px; border-radius: 14px; box-shadow: 0 8px 24px rgba(0,0,0,.3); max-height: 90vh; overflow-y: auto; border: 1px solid var(--border); }

  /* ── CARDS ── */
  .card { background: var(--surface); padding: 20px; border-radius: 14px; box-shadow: 0 6px 20px rgba(0,0,0,.2); border: 1px solid var(--border); }

  /* ── DISPLAY FUERZA ── */
  .display-fuerza { text-align: center; background: var(--deep); color: var(--cyan); padding: 25px; border-radius: 12px; font-size: 4rem; font-family: 'Courier New', monospace; font-weight: bold; box-shadow: inset 0 0 20px rgba(0,0,0,.8); border: 3px solid var(--border); transition: border-color .4s; }
  .display-fuerza small { font-size: 1.8rem; color: var(--blue); margin-left: 8px; }

  /* ── GRÁFICO ── */
  .chart-container { background: var(--deep); border-radius: 12px; padding: 15px; border: 1px solid var(--border); }
  canvas { width: 100%; height: auto; display: block; }

  /* ── SELECTOR DE MANO ── */
  .hand-selector { display: flex; gap: 10px; }
  .hand-btn { flex: 1; padding: 12px; font-size: 15px; font-weight: 700; border: 2px solid var(--border); border-radius: 10px; cursor: pointer; background: var(--deep); color: var(--muted); transition: all .2s; text-transform: uppercase; letter-spacing: 1px; }
  .hand-btn.active-left  { border-color: #3b82f6; color: #3b82f6; background: rgba(59,130,246,.12); }
  .hand-btn.active-right { border-color: var(--purple); color: var(--purple); background: rgba(167,139,250,.12); }

  /* ── CONTROLES ── */
  .control-panel { display: flex; flex-direction: column; gap: 12px; }
  input[type="text"] { padding: 14px; font-size: 15px; background: var(--deep); border: 1px solid var(--border); border-radius: 8px; color: #fff; width: 100%; transition: border .3s; }
  input[type="text"]:focus { border-color: var(--cyan); outline: none; }
  .btn-group { display: flex; gap: 12px; }
  button { padding: 14px 20px; font-size: 14px; color: var(--bg); border: none; border-radius: 8px; cursor: pointer; font-weight: bold; transition: all .2s; flex: 1; text-transform: uppercase; letter-spacing: .5px; }
  .btn-guardar { background: var(--cyan); }
  .btn-guardar:hover { background: #00d1b2; transform: translateY(-1px); }
  .btn-borrar { background: var(--red); color: #fff; }
  .btn-borrar:hover { background: #dc2626; transform: translateY(-1px); }

  /* ── HISTORIAL ── */
  .historial-header { font-size: 1.3rem; color: var(--cyan); margin: 0 0 15px; font-weight: 600; text-align: center; border-bottom: 2px solid var(--border); padding-bottom: 12px; text-transform: uppercase; letter-spacing: 1px; }
  table { width: 100%; border-collapse: collapse; font-size: 12.5px; }
  th, td { padding: 10px 8px; text-align: center; border-bottom: 1px solid var(--border); }
  th { background: var(--deep); color: var(--blue); font-weight: 600; }
  tr:hover { background: #1e2536; }
  .fecha-col { color: var(--muted); font-size: 10.5px; }
  .fuerza-badge { background: rgba(0,245,212,.15); color: var(--cyan); padding: 4px 9px; border-radius: 20px; font-weight: bold; border: 1px solid rgba(0,245,212,.3); display: inline-block; }
  .hand-badge-L { background: rgba(59,130,246,.15); color: #60a5fa; padding: 3px 8px; border-radius: 12px; font-size: 11px; font-weight: 700; border: 1px solid rgba(59,130,246,.3); }
  .hand-badge-R { background: rgba(167,139,250,.15); color: #c4b5fd; padding: 3px 8px; border-radius: 12px; font-size: 11px; font-weight: 700; border: 1px solid rgba(167,139,250,.3); }

  /* ── SECCIÓN AR ── */
  .ar-section { margin-top: 28px; }
  .ar-title { font-size: 1.1rem; color: var(--yellow); font-weight: 700; text-transform: uppercase; letter-spacing: 1px; margin-bottom: 14px; display: flex; align-items: center; gap: 8px; }
  .ar-title span { font-size: 1.2rem; }

  .niveles-grid { display: flex; flex-direction: column; gap: 12px; }

  .nivel-card {
    background: var(--deep);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 14px 16px;
    transition: border-color .3s;
    position: relative;
    overflow: hidden;
  }
  .nivel-card.bloqueado { opacity: .4; pointer-events: none; }
  .nivel-card.completado { border-color: var(--green); }
  .nivel-card.activo     { border-color: var(--yellow); }

  .nivel-header { display: flex; align-items: center; gap: 10px; margin-bottom: 10px; }
  .nivel-num { width: 30px; height: 30px; border-radius: 50%; display: flex; align-items: center; justify-content: center; font-weight: 900; font-size: 14px; flex-shrink: 0; }
  .nivel-num.bloqueado  { background: #374151; color: var(--muted); }
  .nivel-num.activo     { background: rgba(245,158,11,.2); color: var(--yellow); border: 2px solid var(--yellow); }
  .nivel-num.completado { background: rgba(34,197,94,.2); color: var(--green); border: 2px solid var(--green); }

  .nivel-info h4 { margin: 0 0 2px; font-size: 14px; color: var(--text); }
  .nivel-info p  { margin: 0; font-size: 11.5px; color: var(--muted); }

  .nivel-badge-lock { position: absolute; top: 12px; right: 14px; font-size: 18px; }

  .nivel-body { display: flex; gap: 12px; align-items: center; }

  /* ── QR generado dinámicamente ── */
  .qr-img {
    width: 80px;
    height: 80px;
    border-radius: 8px;
    border: 2px solid var(--border);
    object-fit: cover;
    background: #fff;
    flex-shrink: 0;
  }

  .nivel-detail { flex: 1; }
  .umbral-txt { font-size: 12px; color: var(--muted); margin-bottom: 6px; }
  .umbral-val { font-size: 22px; font-weight: 900; color: var(--cyan); font-family: 'Courier New', monospace; }
  .umbral-val small { font-size: 13px; color: var(--blue); }
  .fuerza-actual-txt { font-size: 11px; color: var(--muted); margin-top: 4px; }
  .fuerza-actual-val { color: var(--cyan); font-weight: 700; }

  .nivel-acciones { display: flex; gap: 8px; margin-top: 10px; flex-wrap: wrap; }
  .btn-ar-link { padding: 8px 14px; font-size: 12px; background: rgba(0,180,216,.15); color: var(--blue); border: 1px solid rgba(0,180,216,.4); border-radius: 7px; cursor: pointer; font-weight: 600; text-decoration: none; display: inline-block; transition: all .2s; flex: none; letter-spacing: normal; text-transform: none; }
  .btn-ar-link:hover { background: rgba(0,180,216,.3); transform: none; }
  .btn-logrado { padding: 8px 14px; font-size: 12px; background: rgba(34,197,94,.15); color: var(--green); border: 1px solid rgba(34,197,94,.4); border-radius: 7px; cursor: pointer; font-weight: 700; transition: all .2s; flex: 1; }
  .btn-logrado:hover { background: rgba(34,197,94,.3); }
  .btn-logrado:disabled { opacity: .4; cursor: not-allowed; }

  .completado-tag { display: inline-flex; align-items: center; gap: 5px; font-size: 12px; color: var(--green); font-weight: 700; margin-top: 8px; }

  /* ── CHAT IA FLOTANTE ── */
  .ia-floating-btn { position: fixed; bottom: 25px; right: 25px; width: 60px; height: 60px; background: var(--cyan); border-radius: 50%; display: flex; justify-content: center; align-items: center; font-size: 30px; cursor: pointer; box-shadow: 0 4px 15px rgba(0,245,212,.4); z-index: 1000; transition: transform .2s; }
  .ia-floating-btn:hover { transform: scale(1.1); }
  .ia-modal-chat { position: fixed; bottom: 95px; right: 25px; width: 390px; background: var(--surface); border: 2px solid var(--blue); border-radius: 14px; box-shadow: 0 10px 30px rgba(0,0,0,.5); z-index: 1000; display: none; flex-direction: column; overflow: hidden; animation: fadeIn .3s ease; }
  @keyframes fadeIn { from { opacity:0; transform: translateY(15px); } to { opacity:1; transform: translateY(0); } }
  .ia-modal-header { background: var(--deep); padding: 12px 15px; font-weight: bold; color: var(--cyan); display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid var(--border); }
  .ia-modal-close { cursor: pointer; font-size: 20px; color: var(--red); font-weight: bold; }
  .ia-chat-body { padding: 15px; }
  .ia-chat-box { background: var(--deep); border: 1px solid var(--border); border-radius: 8px; padding: 12px; min-height: 130px; max-height: 260px; overflow-y: auto; margin-bottom: 12px; font-size: 13.5px; line-height: 1.6; color: #e2e8f0; white-space: pre-wrap; }
  .ia-input-group { display: flex; gap: 8px; }
  .btn-ia-send { background: var(--blue); color: var(--bg); font-size: 13px; padding: 10px 14px; flex: none; border-radius: 7px; border: none; cursor: pointer; font-weight: 700; }
  .btn-ia-send:hover { background: #0096b4; }
</style>
</head>
<body onload="init()">

<div class="main-title">Registro DinaHand</div>

<div class="dashboard">

  <!-- ══ COLUMNA IZQUIERDA ══ -->
  <div class="col-izquierda">

    <!-- Display fuerza -->
    <div id="wrapperFuerza" class="display-fuerza">
      <span id="valorActual">0.00</span><small>N</small>
    </div>

    <!-- Gráfico -->
    <div class="chart-container">
      <canvas id="graficoPresion" width="800" height="230"></canvas>
    </div>

    <!-- Controles + selector de mano -->
    <div class="card control-panel">

      <!-- Selector Mano -->
      <div class="hand-selector">
        <button class="hand-btn active-left" id="btnManoIzq" onclick="seleccionarMano('I')">🖐 Mano Izquierda</button>
        <button class="hand-btn"             id="btnManoDer" onclick="seleccionarMano('D')">Mano Derecha 🤚</button>
      </div>

      <input type="text" id="nombrePaciente" placeholder="Nombre - Rut paciente" value="Nombre - Rut paciente"
             onfocus="if(this.value=='Nombre - Rut paciente')this.value=''">

      <div class="btn-group">
        <button class="btn-guardar" onclick="guardarRegistro()">Guardar en Memoria</button>
        <button class="btn-borrar"  onclick="borrarTodoElHistorial()">Borrar Todo</button>
      </div>

      <!-- Tare + estado WS -->
      <div class="btn-group" style="margin-top:4px;">
        <button onclick="hacerTare()" style="background:#f59e0b; color:#0b0f19; font-size:13px;">
          ⚖️ Tare (poner a cero)
        </button>
        <div id="wsStatus" style="
          flex:1; display:flex; align-items:center; justify-content:center; gap:8px;
          background:#0f131c; border:1px solid #232a3b; border-radius:8px;
          font-size:12px; color:#94a3b8; font-weight:600;">
          <span id="wsDot" style="width:10px;height:10px;border-radius:50%;background:#ef4444;display:inline-block;"></span>
          <span id="wsTxt">Sin conexión</span>
        </div>
      </div>
    </div>

  </div><!-- /col-izquierda -->

  <!-- ══ COLUMNA DERECHA ══ -->
  <div class="col-derecha">

    <!-- HISTORIAL -->
    <div class="historial-header">Historial Permanente</div>
    <table>
      <thead>
        <tr><th>Fecha/Hora</th><th>Paciente</th><th>Mano</th><th>Fuerza</th></tr>
      </thead>
      <tbody id="cuerpoTabla"></tbody>
    </table>

    <!-- JUEGO AR -->
    <div class="ar-section">
      <div class="ar-title"><span>🎮</span> Desafío AR — Rehabilitación</div>

      <div class="niveles-grid" id="nivelesGrid">
        <!-- generado por JS -->
      </div>
    </div>

  </div><!-- /col-derecha -->

</div><!-- /dashboard -->

<!-- ══ MODAL QR GRANDE ══ -->
<div id="modalQR" style="
  display:none; position:fixed; inset:0; background:rgba(0,0,0,.85);
  z-index:2000; align-items:center; justify-content:center; flex-direction:column; gap:20px;">

  <div style="background:#161b26; border:2px solid #00b4d8; border-radius:18px;
              padding:30px 36px; text-align:center; max-width:380px; width:90%;
              box-shadow:0 20px 60px rgba(0,0,0,.6); position:relative;">

    <!-- Cerrar -->
    <button onclick="cerrarQR()" style="
      position:absolute; top:12px; right:16px; background:none; border:none;
      color:#ef4444; font-size:22px; cursor:pointer; font-weight:900; padding:0; flex:none;">✕</button>

    <!-- Título -->
    <div id="qrModalEmoji" style="font-size:2.5rem; margin-bottom:6px;"></div>
    <div id="qrModalTitulo" style="color:#00f5d4; font-weight:700; font-size:1.2rem;
                                    text-transform:uppercase; letter-spacing:1px; margin-bottom:4px;"></div>
    <div id="qrModalSub" style="color:#94a3b8; font-size:13px; margin-bottom:20px;"></div>

    <!-- QR grande generado dinámicamente -->
    <div style="background:#ffffff; border-radius:12px; padding:14px; display:inline-block; margin-bottom:18px;">
      <img id="qrModalImg" src="" alt="QR"
           style="width:220px; height:220px; display:block; object-fit:contain;">
    </div>

    <!-- Instrucción -->
    <div style="background:#0f131c; border-radius:10px; padding:12px 16px;
                font-size:13px; color:#cbd5e1; line-height:1.6; border:1px solid #232a3b;">
      📲 <strong style="color:#00f5d4;">Escanea este QR</strong> con la cámara de tu celular.<br>
      Se abrirá la experiencia de Realidad Aumentada directamente en el navegador.<br>
      <span style="color:#94a3b8; font-size:11px;">No necesitas instalar ninguna app.</span>
    </div>

    <!-- Link directo por si el QR falla -->
    <div style="margin-top:14px;">
      <a id="qrModalLink" href="#" target="_blank"
         style="color:#00b4d8; font-size:12px; text-decoration:underline;">
        ¿No funciona el QR? Abre el link directo aquí →
      </a>
    </div>

  </div>
</div>

<!-- Botón flotante IA -->
<div class="ia-floating-btn" onclick="toggleChatIA()">🤖</div>

<!-- Modal IA -->
<div id="modalChatIA" class="ia-modal-chat">
  <div class="ia-modal-header">
    <span>🤖 DinaHand Asistente IA</span>
    <span class="ia-modal-close" onclick="toggleChatIA()">×</span>
  </div>
  <div class="ia-chat-body">
    <div id="iaRespuesta" class="ia-chat-box">Hola, soy tu asistente de rehabilitación. Puedo analizar el historial por mano izquierda y derecha. Pregúntame cómo va el avance de cada mano, o pide un resumen general del progreso.</div>
    <div class="ia-input-group">
      <input type="text" id="iaPregunta" placeholder="¿Cómo va la mano izquierda?">
      <button class="btn-ia-send" onclick="preguntarIA()">Enviar</button>
    </div>
  </div>
</div>

<script>
// ════════════════════════════════════════
//  CONFIGURACIÓN IA
// ════════════════════════════════════════
const GEMINI_API_KEY = "AQ.Ab8RN6J4WNhPT_iyNfTbLlEL7VpVcnSr9YV5NYTxRNutd6hKLQ";

// ════════════════════════════════════════
//  HELPER: genera URL de QR dinámico
//  Usa goqr.me (sin CORS) como API pública
// ════════════════════════════════════════
function qrUrl(arLink, size) {
  return 'https://api.qrserver.com/v1/create-qr-code/?size=' + size + 'x' + size +
         '&ecc=H&data=' + encodeURIComponent(arLink);
}

// ════════════════════════════════════════
//  ESTADO GLOBAL
// ════════════════════════════════════════
let historialCompletoIA = [];
let manoSeleccionada    = 'I';   // 'I' o 'D'
let datosActuales       = { presion: 0, hora: "" };
let datosFuerza         = [];
const maxPuntos         = 120;

const wrapperFuerza = document.getElementById('wrapperFuerza');
const valorActualEl = document.getElementById('valorActual');
const canvas        = document.getElementById('graficoPresion');
const ctx           = canvas.getContext('2d');

// ════════════════════════════════════════
//  DEFINICIÓN DE NIVELES AR
//  qrUrl se genera dinámicamente desde arUrl
// ════════════════════════════════════════
const NIVELES = [
  {
    id:     1,
    nombre: "iPhone X",
    objeto: "Sostener y levantar un teléfono",
    umbral: 15,
    arUrl:  "https://mywebar.com/p/Project_0_tnwf1elhnw",
    emoji:  "📱"
  },
  {
    id:     2,
    nombre: "Lata de Coca-Cola",
    objeto: "Apretar y sostener una lata",
    umbral: 30,
    arUrl:  "https://mywebar.com/p/Project_3_mi0a88zv0i",
    emoji:  "🥤"
  },
  {
    id:     3,
    nombre: "Maleta",
    objeto: "Cargar y transportar una maleta",
    umbral: 55,
    arUrl:  "https://mywebar.com/p/Project_4_73n6b98qw3",
    emoji:  "🧳"
  }
];

// ════════════════════════════════════════
//  PERSISTENCIA DE NIVELES (localStorage)
// ════════════════════════════════════════
function cargarNivelesStorage() {
  try {
    const saved = localStorage.getItem('dinahand_niveles');
    if (saved) return JSON.parse(saved);
  } catch(e) {}
  return { maxDesbloqueado: 1, completados: [] };
}

function guardarNivelesStorage(estado) {
  try { localStorage.setItem('dinahand_niveles', JSON.stringify(estado)); } catch(e) {}
}

let estadoNiveles = cargarNivelesStorage();

// ════════════════════════════════════════
//  RENDERIZAR NIVELES AR
// ════════════════════════════════════════
function renderizarNiveles() {
  const grid = document.getElementById('nivelesGrid');
  grid.innerHTML = '';

  NIVELES.forEach(nivel => {
    const estaCompletado = estadoNiveles.completados.includes(nivel.id);
    const estaDesbloq    = nivel.id <= estadoNiveles.maxDesbloqueado;
    const estaBloqueado  = !estaDesbloq;

    let estadoCard = estaBloqueado  ? 'bloqueado'  :
                     estaCompletado ? 'completado' : 'activo';
    let estadoNum  = estadoCard;

    const card = document.createElement('div');
    card.className = 'nivel-card ' + estadoCard;
    card.id = 'nivel-card-' + nivel.id;

    const fuerzaActualStr = datosActuales.presion.toFixed(1);
    const superaUmbral    = datosActuales.presion >= nivel.umbral;

    // QR pequeño (80px) generado dinámicamente
    const qrSmall = qrUrl(nivel.arUrl, 80);

    card.innerHTML =
      '<div class="nivel-header">' +
        '<div class="nivel-num ' + estadoNum + '">' + nivel.id + '</div>' +
        '<div class="nivel-info">' +
          '<h4>' + nivel.emoji + ' Nivel ' + nivel.id + ': ' + nivel.nombre + '</h4>' +
          '<p>' + nivel.objeto + '</p>' +
        '</div>' +
        (estaBloqueado  ? '<div class="nivel-badge-lock">🔒</div>' : '') +
        (estaCompletado ? '<div class="nivel-badge-lock">✅</div>' : '') +
      '</div>' +

      '<div class="nivel-body">' +
        '<img class="qr-img" src="' + qrSmall + '" alt="QR Nivel ' + nivel.id + '" ' +
             'title="Escanea para abrir la experiencia AR">' +
        '<div class="nivel-detail">' +
          '<div class="umbral-txt">Fuerza necesaria para superar:</div>' +
          '<div class="umbral-val">' + nivel.umbral + '<small> N</small></div>' +
          '<div class="fuerza-actual-txt">' +
            'Tu fuerza actual: <span class="fuerza-actual-val" id="fuerza-disp-' + nivel.id + '">' +
              fuerzaActualStr + ' N</span>' +
            (superaUmbral && !estaBloqueado ? ' ✅' : '') +
          '</div>' +
        '</div>' +
      '</div>' +

      '<div class="nivel-acciones">' +
        '<button class="btn-ar-link" onclick="mostrarQR(' + nivel.id + ')" ' +
          (estaBloqueado ? 'disabled style="opacity:.4;cursor:not-allowed"' : '') + '>' +
          '📱 Mostrar QR para celular' +
        '</button>' +
        (estaCompletado
          ? '<div class="completado-tag">✅ Nivel completado</div>'
          : '<button class="btn-logrado" onclick="marcarLogrado(' + nivel.id + ')" ' +
              (estaBloqueado ? 'disabled' : '') + '>' +
              '🏆 Lo logré — desbloquear siguiente' +
            '</button>'
        ) +
      '</div>';

    grid.appendChild(card);
  });
}

// ════════════════════════════════════════
//  MARCAR NIVEL COMO LOGRADO
// ════════════════════════════════════════
function marcarLogrado(nivelId) {
  if (!estadoNiveles.completados.includes(nivelId)) {
    estadoNiveles.completados.push(nivelId);
  }
  if (nivelId >= estadoNiveles.maxDesbloqueado && nivelId < NIVELES.length) {
    estadoNiveles.maxDesbloqueado = nivelId + 1;
  }
  guardarNivelesStorage(estadoNiveles);
  renderizarNiveles();
}

// ════════════════════════════════════════
//  ACTUALIZAR FUERZA EN TARJETAS AR
// ════════════════════════════════════════
function actualizarFuerzaEnNiveles() {
  NIVELES.forEach(nivel => {
    const el = document.getElementById('fuerza-disp-' + nivel.id);
    if (el) {
      const supera = datosActuales.presion >= nivel.umbral;
      el.textContent = datosActuales.presion.toFixed(1) + ' N';
      const parent = el.parentElement;
      const lastNode = parent.lastChild;
      if (lastNode && lastNode.nodeType === 3) {
        lastNode.textContent = supera ? ' ✅' : '';
      }
    }
  });
}

// ════════════════════════════════════════
//  SELECTOR DE MANO
// ════════════════════════════════════════
function seleccionarMano(mano) {
  manoSeleccionada = mano;
  document.getElementById('btnManoIzq').className =
    'hand-btn' + (mano === 'I' ? ' active-left'  : '');
  document.getElementById('btnManoDer').className =
    'hand-btn' + (mano === 'D' ? ' active-right' : '');
}

// ════════════════════════════════════════
//  GRÁFICO
// ════════════════════════════════════════
function redibujarGrafico() {
  const w = canvas.width, h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  ctx.strokeStyle = '#232a3b'; ctx.lineWidth = 1;
  for (let i = 1; i < 5; i++) { let y=(h/5)*i; ctx.beginPath(); ctx.moveTo(0,y); ctx.lineTo(w,y); ctx.stroke(); }
  for (let i = 1; i < 10; i++) { let x=(w/10)*i; ctx.beginPath(); ctx.moveTo(x,0); ctx.lineTo(x,h); ctx.stroke(); }
  if (datosFuerza.length === 0) return;
  let maxVal = Math.max(...datosFuerza, 10);
  let pasoX  = w / (maxPuntos - 1);
  ctx.beginPath(); ctx.fillStyle = 'rgba(0,245,212,0.08)'; ctx.moveTo(0,h);
  for (let i=0;i<datosFuerza.length;i++){let x=i*pasoX,y=h-(datosFuerza[i]/maxVal)*(h-30)-15;ctx.lineTo(x,y);}
  ctx.lineTo((datosFuerza.length-1)*pasoX,h); ctx.closePath(); ctx.fill();
  ctx.beginPath(); ctx.strokeStyle='#00f5d4'; ctx.lineWidth=3;
  for (let i=0;i<datosFuerza.length;i++){let x=i*pasoX,y=h-(datosFuerza[i]/maxVal)*(h-30)-15;if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);}
  ctx.stroke();
}
redibujarGrafico();

// ════════════════════════════════════════
//  WEBSOCKET
// ════════════════════════════════════════
function setWsStatus(ok) {
  document.getElementById('wsDot').style.background = ok ? '#22c55e' : '#ef4444';
  document.getElementById('wsTxt').textContent = ok ? 'Báscula conectada' : 'Sin conexión';
}

const connection = new WebSocket('ws://' + window.location.hostname + ':81/');
connection.onopen  = () => { wrapperFuerza.style.borderColor = '#00f5d4'; setWsStatus(true); };
connection.onerror = () => { wrapperFuerza.style.borderColor = '#ef4444'; setWsStatus(false); };
connection.onclose = () => { wrapperFuerza.style.borderColor = '#ef4444'; setWsStatus(false); };
connection.onmessage = (e) => {
  try {
    const data = JSON.parse(e.data);
    datosActuales.presion = data.p;
    datosActuales.hora    = data.h;
    valorActualEl.innerText = data.p.toFixed(2);
    datosFuerza.push(data.p);
    if (datosFuerza.length > maxPuntos) datosFuerza.shift();
    redibujarGrafico();
    actualizarFuerzaEnNiveles();
  } catch(err) { console.warn("JSON inválido:", e.data); }
};

// ════════════════════════════════════════
//  TARE MANUAL
// ════════════════════════════════════════
function hacerTare() {
  fetch('/tare')
    .then(r => r.text())
    .then(t => alert('✅ ' + t + ' — la báscula quedó en cero'))
    .catch(() => alert('❌ No se pudo conectar al ESP32 para hacer tare'));
}

// ════════════════════════════════════════
//  GUARDAR REGISTRO (con mano)
// ════════════════════════════════════════
function guardarRegistro() {
  const nombre = document.getElementById('nombrePaciente').value;
  if (!nombre.trim() || nombre === 'Nombre - Rut paciente') {
    alert('Por favor, ingrese el Nombre y RUT del paciente'); return;
  }
  const manoLabel = manoSeleccionada === 'I' ? 'IZQ' : 'DER';
  fetch('/guardar?paciente=' + encodeURIComponent(nombre) +
        '&fuerza=' + datosActuales.presion.toFixed(2) +
        '&fecha='  + encodeURIComponent(datosActuales.hora) +
        '&mano='   + manoLabel)
    .then(r => r.text()).then(() => cargarHistorialServidor());
}

// ════════════════════════════════════════
//  CARGAR HISTORIAL
// ════════════════════════════════════════
function cargarHistorialServidor() {
  fetch('/historial').then(r => r.json()).then(data => {
    historialCompletoIA = data;
    const cuerpo = document.getElementById('cuerpoTabla');
    cuerpo.innerHTML = '';
    [...data].reverse().forEach(reg => {
      const fila = cuerpo.insertRow();
      const cf = fila.insertCell(0); cf.innerText = reg.fecha; cf.className = 'fecha-col';
      fila.insertCell(1).innerText = reg.paciente;
      const cm = fila.insertCell(2);
      const mano = reg.mano || '?';
      cm.innerHTML = mano === 'IZQ'
        ? '<span class="hand-badge-L">🖐 IZQ</span>'
        : '<span class="hand-badge-R">🤚 DER</span>';
      const cfz = fila.insertCell(3);
      cfz.innerHTML = '<span class="fuerza-badge">' + reg.fuerza + ' N</span>';
    });
  });
}

function borrarTodoElHistorial() {
  if (confirm('¿Está seguro de eliminar de forma permanente todo el historial?')) {
    fetch('/borrar').then(() => cargarHistorialServidor());
  }
}

// ════════════════════════════════════════
//  ASISTENTE IA (con contexto por mano)
// ════════════════════════════════════════
function toggleChatIA() {
  const modal = document.getElementById('modalChatIA');
  if (modal.style.display === 'flex') { modal.style.display = 'none'; }
  else { modal.style.display = 'flex'; document.getElementById('iaPregunta').focus(); }
}

function preguntarIA() {
  const pregunta = document.getElementById('iaPregunta').value;
  const resp     = document.getElementById('iaRespuesta');
  if (!pregunta.trim()) return;
  resp.innerText = 'Analizando... 🧠';

  const histIzq = historialCompletoIA.filter(r => (r.mano||'') === 'IZQ');
  const histDer = historialCompletoIA.filter(r => (r.mano||'') === 'DER');

  const contexto =
    'Eres un Kinesiólogo experto en rehabilitación de mano.\n' +
    'Tienes acceso al historial clínico de fuerza de presión (en Newtons) separado por mano:\n\n' +
    'MANO IZQUIERDA (' + histIzq.length + ' registros):\n' + JSON.stringify(histIzq) + '\n\n' +
    'MANO DERECHA (' + histDer.length + ' registros):\n' + JSON.stringify(histDer) + '\n\n' +
    'HISTORIAL COMPLETO (ambas manos, cronológico):\n' + JSON.stringify(historialCompletoIA) + '\n\n' +
    'Responde de forma amigable, clara y clínica en máximo 5 líneas.\n' +
    'Cuando el usuario pregunte por una mano específica, analiza solo esos datos.\n' +
    'Si hay diferencia entre ambas manos, coméntala.';

  const url = 'https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=' + GEMINI_API_KEY;
  const payload = {
    contents: [{
      role: 'user',
      parts: [{ text: contexto }, { text: 'Pregunta: ' + pregunta }]
    }]
  };

  fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) })
    .then(r => { if(!r.ok) return r.json().then(e => { throw new Error(e.error.message); }); return r.json(); })
    .then(d => {
      try {
        resp.innerText = d.candidates[0].content.parts[0].text;
        document.getElementById('iaPregunta').value = '';
      } catch(e) { resp.innerText = 'Error procesando la respuesta.'; }
    })
    .catch(err => { resp.innerText = 'Error: ' + err.message; });
}

// ════════════════════════════════════════
//  MODAL QR GRANDE
//  QR de 220px generado dinámicamente
// ════════════════════════════════════════
function mostrarQR(nivelId) {
  const nivel = NIVELES.find(n => n.id === nivelId);
  if (!nivel) return;

  document.getElementById('qrModalEmoji').textContent  = nivel.emoji;
  document.getElementById('qrModalTitulo').textContent = 'Nivel ' + nivel.id + ': ' + nivel.nombre;
  document.getElementById('qrModalSub').textContent    = nivel.objeto;
  document.getElementById('qrModalImg').src            = qrUrl(nivel.arUrl, 220);
  document.getElementById('qrModalLink').href          = nivel.arUrl;

  const modal = document.getElementById('modalQR');
  modal.style.display = 'flex';
}

function cerrarQR() {
  document.getElementById('modalQR').style.display = 'none';
}

document.getElementById('modalQR').addEventListener('click', function(e) {
  if (e.target === this) cerrarQR();
});

// ════════════════════════════════════════
//  INIT
// ════════════════════════════════════════
function init() {
  cargarHistorialServidor();
  renderizarNiveles();
}
</script>
</body>
</html>
)rawliteral";


// ============================================================
//  FUNCIONES DEL SERVIDOR
// ============================================================

void manejarRaiz() { server.send(200, "text/html", index_html); }

void manejarGuardar() {
    String paciente = server.arg("paciente");
    String fuerza   = server.arg("fuerza");
    String fecha    = server.arg("fecha");
    String mano     = server.arg("mano");

    paciente.replace("|", "-");
    fecha.replace("|", "-");
    mano.replace("|", "-");
    if (mano.length() == 0) mano = "?";

    File file = LittleFS.open(HISTORIAL_FILE, FILE_APPEND);
    if (file) {
        file.println(fecha + "|" + paciente + "|" + fuerza + "|" + mano);
        file.close();
        server.send(200, "text/plain", "OK");
    } else {
        server.send(500, "text/plain", "Error al abrir archivo");
    }
}

void manejarHistorial() {
    File file = LittleFS.open(HISTORIAL_FILE, FILE_READ);
    String json = "[";
    if (file) {
        bool primero = true;
        while (file.available()) {
            String linea = file.readStringUntil('\n');
            linea.trim();
            if (linea.length() == 0) continue;

            int p1 = linea.indexOf('|');
            int p2 = linea.indexOf('|', p1 + 1);
            int p3 = linea.indexOf('|', p2 + 1);

            if (p1 != -1 && p2 != -1) {
                String fecha    = linea.substring(0, p1);
                String paciente = linea.substring(p1 + 1, p2);
                String fuerza   = linea.substring(p2 + 1, p3 != -1 ? p3 : linea.length());
                String mano     = (p3 != -1) ? linea.substring(p3 + 1) : "?";

                paciente.replace("\"", "'");
                fecha.replace("\"", "'");
                mano.replace("\"", "'");

                if (!primero) json += ",";
                json += "{\"fecha\":\"" + fecha + "\",\"paciente\":\"" + paciente +
                        "\",\"fuerza\":" + fuerza + ",\"mano\":\"" + mano + "\"}";
                primero = false;
            }
        }
        file.close();
    }
    json += "]";
    server.send(200, "application/json", json);
}

void manejarBorrar() {
    LittleFS.remove(HISTORIAL_FILE);
    server.send(200, "text/plain", "Borrado");
}

void manejarTare() {
    if (scaleOK) {
        scale.tare();
        Serial.println("[TARE] Tare manual desde la web OK");
        server.send(200, "text/plain", "Tare OK");
    } else {
        server.send(500, "text/plain", "Celda no disponible");
    }
}


// ============================================================
//  HELPERS OLED
// ============================================================

void oledStatus(const char* titulo, const char* estado, bool ok) {
    if (!oledOK) return;
    display.clearDisplay();
    display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0); display.println("-- DinaHand v3 --");
    display.println(""); display.println(titulo);
    display.print("> "); display.println(estado);
    display.println(""); display.print(ok ? "[ OK ]" : "[ERROR]");
    display.display();
}

void oledBootSummary(bool celdaOk, bool wifiOk, const char* ip) {
    if (!oledOK) return;
    display.clearDisplay();
    display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("-- DinaHand v3 --");
    display.print("Celda:  "); display.println(celdaOk ? "OK" : "ERROR");
    display.println("OLED:   OK");
    display.print("WiFi:   "); display.println(wifiOk ? "OK" : "ERROR");
    if (wifiOk) { display.print("IP: "); display.println(ip); }
    display.println("Esperando datos...");
    display.display();
}

void oledMostrarDatos(float fuerzaN, const char* fechaHora) {
    if (!oledOK) return;
    display.clearDisplay();
    display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0); display.print("MONITOR DE FUERZA");
    display.setTextSize(3); display.setCursor(0, 20); display.print(fuerzaN, 1);
    display.setTextSize(2); display.print(" N");
    display.setTextSize(1); display.setCursor(0, 55); display.print(fechaHora);
    display.display();
}


// ============================================================
//  SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(300);

    Serial.println("\n========================================");
    Serial.println("   DinaHand v3 - Iniciando sistema...");
    Serial.println("========================================");

    if (!LittleFS.begin(true)) {
        Serial.println("[FS] ERROR: No se pudo montar LittleFS");
    } else {
        Serial.println("[FS] LittleFS montado OK");
    }

    Wire.begin(I2C_SDA, I2C_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        Serial.println("[OLED] ERROR: Pantalla no detectada en 0x3C");
        oledOK = false;
    } else {
        oledOK = true;
        Serial.println("[OLED] Pantalla OK - encendida");
        oledStatus("Iniciando...", "Buscando celda", true);
    }

    scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
    scale.set_scale(FACTOR_CALIBRACION);

    Serial.print("[CELDA] Esperando respuesta HX711");
    int intentos = 0;
    while (!scale.is_ready() && intentos < 20) {
        Serial.print("."); delay(100); intentos++;
    }

    if (scale.is_ready()) {
        scale.tare();
        scaleOK = true;
        Serial.println("\n[CELDA] HX711 OK - Tare aplicado");
        oledStatus("Celda de carga:", "HX711 OK - Tared", true);
    } else {
        scaleOK = false;
        Serial.println("\n[CELDA] ERROR: HX711 no responde. Revisa pines 4 y 5");
        oledStatus("Celda de carga:", "ERROR - sin resp.", false);
    }
    delay(600);

    oledStatus("Conectando WiFi:", ssid, true);
    WiFi.begin(ssid, password);
    Serial.print("[WiFi] Conectando a: "); Serial.print(ssid);

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(500); Serial.print(".");
    }

    bool wifiOK = (WiFi.status() == WL_CONNECTED);
    if (wifiOK) {
        Serial.println("\n[WiFi] Conectado!");
        Serial.print("[WiFi] IP: "); Serial.println(WiFi.localIP());
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
        Serial.println("[NTP ] Sincronizando hora...");
    } else {
        Serial.println("\n[WiFi] ERROR: No se pudo conectar.");
    }

    String ipStr = wifiOK ? WiFi.localIP().toString() : "sin WiFi";
    oledBootSummary(scaleOK, wifiOK, ipStr.c_str());
    delay(1500);

    Serial.println("\n========================================");
    Serial.print("[DIAG] OLED:   "); Serial.println(oledOK  ? "OK" : "ERROR");
    Serial.print("[DIAG] CELDA:  "); Serial.println(scaleOK ? "OK" : "ERROR");
    Serial.print("[DIAG] WiFi:   "); Serial.println(wifiOK  ? "OK" : "ERROR");
    if (wifiOK) { Serial.print("[DIAG] IP:     "); Serial.println(WiFi.localIP()); }
    Serial.println("========================================\n");

    server.on("/",          manejarRaiz);
    server.on("/guardar",   manejarGuardar);
    server.on("/historial", manejarHistorial);
    server.on("/borrar",    manejarBorrar);
    server.on("/tare",      manejarTare);
    server.begin();
    Serial.println("[HTTP] Servidor iniciado en puerto 80");

    webSocket.begin();
    Serial.println("[WSS ] WebSocket iniciado en puerto 81");
    Serial.println("[SYS ] Sistema listo.\n");
}


// ============================================================
//  LOOP
// ============================================================
void loop() {
    server.handleClient();
    webSocket.loop();

    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis < interval) return;
    previousMillis = currentMillis;

    float fuerzaNewton = 0.0;
    if (scaleOK && scale.is_ready()) {
        float masaKg = scale.get_units(1);
        // Debug: imprime valor crudo cada 10 lecturas para no saturar el serial
        static int debugCount = 0;
        if (++debugCount >= 10) {
            debugCount = 0;
            Serial.print("[HX711] masa_kg="); Serial.print(masaKg, 4);
            Serial.print("  fuerza_N="); Serial.println(masaKg * 9.81, 2);
        }
        if (masaKg < 0.0) masaKg = -masaKg;   // valor absoluto por si el factor queda invertido
        fuerzaNewton = masaKg * 9.81;
    } else if (!scaleOK) {
        static int errCount = 0;
        if (++errCount >= 50) { errCount = 0; Serial.println("[HX711] ERROR: celda no disponible"); }
    }

    struct tm timeinfo;
    char bufferFechaHora[30];
    if (!getLocalTime(&timeinfo)) {
        strcpy(bufferFechaHora, "--/-- --:--:--");
    } else {
        strftime(bufferFechaHora, sizeof(bufferFechaHora), "%d %b, %H:%M:%S", &timeinfo);
    }

    String jsonPayload = "{\"p\":" + String(fuerzaNewton, 2) + ",\"h\":\"" + String(bufferFechaHora) + "\"}";
    webSocket.broadcastTXT(jsonPayload);

    oledMostrarDatos(fuerzaNewton, bufferFechaHora);
}
