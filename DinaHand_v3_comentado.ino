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

// ------------------------------------------------------------
// LIBRERÍAS UTILIZADAS
// ------------------------------------------------------------
#include <WiFi.h>              // Conexión WiFi del ESP32 (modo estación)
#include <WebServer.h>         // Servidor HTTP embebido (sirve la página y responde a peticiones)
#include <time.h>              // Manejo de fecha/hora vía NTP (configTime, getLocalTime)
#include "HX711.h"             // Driver para el amplificador de la celda de carga (galga extensométrica)
#include <WebSocketsServer.h>  // Servidor WebSocket para enviar datos de fuerza en tiempo real al navegador
#include <Wire.h>              // Bus I2C, usado por la pantalla OLED
#include <Adafruit_GFX.h>      // Librería gráfica base (dibujo de texto/formas) usada por el driver del OLED
#include <Adafruit_SSD1306.h>  // Driver específico para la pantalla OLED SSD1306
#include <LittleFS.h>          // Sistema de archivos interno de la flash del ESP32 (guarda el historial en /historial.txt)

// ==================== OLED ====================
// Configuración de la pantalla OLED (128x64 píxeles) conectada por I2C
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1     // -1 = no se usa un pin de reset físico (comparte el reset del ESP32)
#define SCREEN_ADDRESS  0x3C   // Dirección I2C típica de los módulos SSD1306
#define I2C_SDA         8      // Pin de datos I2C (personalizado, no el default de la placa)
#define I2C_SCL         9      // Pin de reloj I2C (personalizado, no el default de la placa)

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledOK = false;   // Bandera: true si la pantalla respondió correctamente en el setup()

// ==================== CELDA DE CARGA ====================
// Pines y calibración del sensor de fuerza (celda de carga + módulo HX711)
const int   LOADCELL_DOUT_PIN    = 5;       // Pin de datos del HX711
const int   LOADCELL_SCK_PIN     = 4;       // Pin de reloj (clock) del HX711
const float FACTOR_CALIBRACION   = 10000.0; // Factor de escala obtenido al calibrar con un peso conocido.
                                             // positivo — el abs() en el loop evita lecturas negativas

HX711 scale;
bool  scaleOK = false;  // Bandera: true si el HX711 respondió a tiempo durante el setup()

// ==================== RED ====================
// Credenciales de la red WiFi a la que se conecta el ESP32.
// NOTA: quedan visibles en el código fuente; si este proyecto se comparte o sube a un
// repositorio público, conviene moverlas a un archivo de configuración no versionado.
const char* ssid     = "Tomasitoo";
const char* password = "Tomasito2008";

// ==================== NTP ====================
// Configuración para sincronizar la hora real por internet (Network Time Protocol)
const char* ntpServer          = "pool.ntp.org";
const long  gmtOffset_sec      = -14400;  // Desfase horario respecto a UTC en segundos (-4 horas)
const int   daylightOffset_sec = 3600;    // Ajuste por horario de verano (1 hora), si aplica

// ==================== SERVIDOR ====================
WebServer        server(80);   // Servidor HTTP: sirve la página web y las rutas /guardar, /historial, etc. (puerto 80)
WebSocketsServer webSocket(81);// Servidor WebSocket: transmite la fuerza en tiempo real al navegador (puerto 81)

const char* HISTORIAL_FILE = "/historial.txt"; // Ruta del archivo donde se guarda el historial en LittleFS

// ==================== TIMING ====================
unsigned long previousMillis = 0;  // Marca de tiempo de la última lectura enviada
const long    interval       = 100; // Intervalo entre lecturas/envíos, en milisegundos (10 lecturas por segundo)

// ============================================================
//  HTML PRINCIPAL
//  Esta cadena gigante (guardada en PROGMEM, es decir, en memoria
//  flash y no en RAM) contiene TODA la interfaz web: HTML, CSS y
//  JavaScript. El ESP32 la envía completa cuando el navegador pide "/".
// ============================================================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>REGISTRO DINAHAND v3</title>
<style>
  /* ── Paleta de colores central del sitio (variables CSS) ──
     Definir los colores acá permite reutilizarlos en toda la hoja
     de estilos con var(--nombre) y cambiar el tema fácilmente. */
  :root {
    --bg:       #0b0f19;   /* fondo general (oscuro) */
    --surface:  #161b26;   /* fondo de tarjetas/paneles */
    --border:   #232a3b;   /* color de bordes sutiles */
    --deep:     #0f131c;   /* fondo "hundido" (displays, inputs) */
    --cyan:     #00f5d4;   /* color de acento principal */
    --blue:     #00b4d8;   /* color de acento secundario */
    --red:      #ef4444;   /* alertas / borrar */
    --yellow:   #f59e0b;   /* advertencias / nivel activo */
    --green:    #22c55e;   /* éxito / completado */
    --purple:   #a78bfa;   /* mano derecha (identidad de color) */
    --text:     #f8fafc;   /* texto principal */
    --muted:    #94a3b8;   /* texto secundario/atenuado */
  }

  * { box-sizing: border-box; } /* padding y border no aumentan el ancho/alto declarado */
  body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: var(--bg); margin: 0; padding: 20px; color: var(--text); }

  /* ── TÍTULO ── */
  .main-title { color: var(--cyan); text-align: center; margin-bottom: 25px; font-weight: 700; letter-spacing: 2px; font-size: 2.2rem; text-transform: uppercase; border-bottom: 3px solid var(--blue); display: inline-block; padding-bottom: 5px; position: relative; left: 50%; transform: translateX(-50%); }

  /* ── LAYOUT ──
     Estructura en dos columnas: izquierda (medición/controles) y
     derecha (historial + juego AR), usando flexbox. */
  .dashboard { display: flex; gap: 25px; max-width: 1350px; margin: auto; align-items: flex-start; }
  .col-izquierda { flex: 7; display: flex; flex-direction: column; gap: 20px; }
  .col-derecha   { flex: 5; background: var(--surface); padding: 20px; border-radius: 14px; box-shadow: 0 8px 24px rgba(0,0,0,.3); max-height: 90vh; overflow-y: auto; border: 1px solid var(--border); }

  /* ── CARDS (tarjetas genéricas reutilizadas en varios paneles) ── */
  .card { background: var(--surface); padding: 20px; border-radius: 14px; box-shadow: 0 6px 20px rgba(0,0,0,.2); border: 1px solid var(--border); }

  /* ── DISPLAY FUERZA (número grande tipo "pantalla digital") ── */
  .display-fuerza { text-align: center; background: var(--deep); color: var(--cyan); padding: 25px; border-radius: 12px; font-size: 4rem; font-family: 'Courier New', monospace; font-weight: bold; box-shadow: inset 0 0 20px rgba(0,0,0,.8); border: 3px solid var(--border); transition: border-color .4s; }
  .display-fuerza small { font-size: 1.8rem; color: var(--blue); margin-left: 8px; }

  /* ── GRÁFICO (contenedor del canvas donde se dibuja la curva de fuerza) ── */
  .chart-container { background: var(--deep); border-radius: 12px; padding: 15px; border: 1px solid var(--border); }
  canvas { width: 100%; height: auto; display: block; }

  /* ── SELECTOR DE MANO (botones Izquierda/Derecha) ── */
  .hand-selector { display: flex; gap: 10px; }
  .hand-btn { flex: 1; padding: 12px; font-size: 15px; font-weight: 700; border: 2px solid var(--border); border-radius: 10px; cursor: pointer; background: var(--deep); color: var(--muted); transition: all .2s; text-transform: uppercase; letter-spacing: 1px; }
  .hand-btn.active-left  { border-color: #3b82f6; color: #3b82f6; background: rgba(59,130,246,.12); }  /* estado activo: mano izquierda (azul) */
  .hand-btn.active-right { border-color: var(--purple); color: var(--purple); background: rgba(167,139,250,.12); } /* estado activo: mano derecha (morado) */

  /* ── CONTROLES (input de nombre, botones guardar/borrar/tare) ── */
  .control-panel { display: flex; flex-direction: column; gap: 12px; }
  input[type="text"] { padding: 14px; font-size: 15px; background: var(--deep); border: 1px solid var(--border); border-radius: 8px; color: #fff; width: 100%; transition: border .3s; }
  input[type="text"]:focus { border-color: var(--cyan); outline: none; }
  .btn-group { display: flex; gap: 12px; }
  button { padding: 14px 20px; font-size: 14px; color: var(--bg); border: none; border-radius: 8px; cursor: pointer; font-weight: bold; transition: all .2s; flex: 1; text-transform: uppercase; letter-spacing: .5px; }
  .btn-guardar { background: var(--cyan); }
  .btn-guardar:hover { background: #00d1b2; transform: translateY(-1px); }
  .btn-borrar { background: var(--red); color: #fff; }
  .btn-borrar:hover { background: #dc2626; transform: translateY(-1px); }

  /* ── HISTORIAL (tabla con los registros guardados) ── */
  .historial-header { font-size: 1.3rem; color: var(--cyan); margin: 0 0 15px; font-weight: 600; text-align: center; border-bottom: 2px solid var(--border); padding-bottom: 12px; text-transform: uppercase; letter-spacing: 1px; }
  table { width: 100%; border-collapse: collapse; font-size: 12.5px; }
  th, td { padding: 10px 8px; text-align: center; border-bottom: 1px solid var(--border); }
  th { background: var(--deep); color: var(--blue); font-weight: 600; }
  tr:hover { background: #1e2536; }
  .fecha-col { color: var(--muted); font-size: 10.5px; }
  .fuerza-badge { background: rgba(0,245,212,.15); color: var(--cyan); padding: 4px 9px; border-radius: 20px; font-weight: bold; border: 1px solid rgba(0,245,212,.3); display: inline-block; }
  .hand-badge-L { background: rgba(59,130,246,.15); color: #60a5fa; padding: 3px 8px; border-radius: 12px; font-size: 11px; font-weight: 700; border: 1px solid rgba(59,130,246,.3); }  /* badge mano izquierda */
  .hand-badge-R { background: rgba(167,139,250,.15); color: #c4b5fd; padding: 3px 8px; border-radius: 12px; font-size: 11px; font-weight: 700; border: 1px solid rgba(167,139,250,.3); } /* badge mano derecha */

  /* ── SECCIÓN AR (juego con niveles de fuerza objetivo) ── */
  .ar-section { margin-top: 28px; }
  .ar-title { font-size: 1.1rem; color: var(--yellow); font-weight: 700; text-transform: uppercase; letter-spacing: 1px; margin-bottom: 14px; display: flex; align-items: center; gap: 8px; }
  .ar-title span { font-size: 1.2rem; }

  .niveles-grid { display: flex; flex-direction: column; gap: 12px; }

  /* Tarjeta de cada nivel: cambia de aspecto según su estado (bloqueado/activo/completado) */
  .nivel-card {
    background: var(--deep);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 14px 16px;
    transition: border-color .3s;
    position: relative;
    overflow: hidden;
  }
  .nivel-card.bloqueado { opacity: .4; pointer-events: none; }   /* nivel aún no alcanzado: atenuado y sin clics */
  .nivel-card.completado { border-color: var(--green); }         /* nivel ya superado: borde verde */
  .nivel-card.activo     { border-color: var(--yellow); }        /* nivel actual disponible: borde amarillo */

  .nivel-header { display: flex; align-items: center; gap: 10px; margin-bottom: 10px; }
  .nivel-num { width: 30px; height: 30px; border-radius: 50%; display: flex; align-items: center; justify-content: center; font-weight: 900; font-size: 14px; flex-shrink: 0; }
  .nivel-num.bloqueado  { background: #374151; color: var(--muted); }
  .nivel-num.activo     { background: rgba(245,158,11,.2); color: var(--yellow); border: 2px solid var(--yellow); }
  .nivel-num.completado { background: rgba(34,197,94,.2); color: var(--green); border: 2px solid var(--green); }

  .nivel-info h4 { margin: 0 0 2px; font-size: 14px; color: var(--text); }
  .nivel-info p  { margin: 0; font-size: 11.5px; color: var(--muted); }

  .nivel-badge-lock { position: absolute; top: 12px; right: 14px; font-size: 18px; } /* ícono 🔒 o ✅ en la esquina */

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

  /* ── CHAT IA FLOTANTE (botón redondo + ventana de chat) ── */
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
<body onload="init()"> <!-- Al cargar la página se ejecuta init(): trae el historial y dibuja los niveles AR -->

<div class="main-title">Registro DinaHand</div>

<div class="dashboard">

  <!-- ══ COLUMNA IZQUIERDA: medición en vivo y controles ══ -->
  <div class="col-izquierda">

    <!-- Display fuerza: número grande que se actualiza vía WebSocket -->
    <div id="wrapperFuerza" class="display-fuerza">
      <span id="valorActual">0.00</span><small>N</small>
    </div>

    <!-- Gráfico: canvas donde se dibuja la curva de fuerza en tiempo real -->
    <div class="chart-container">
      <canvas id="graficoPresion" width="800" height="230"></canvas>
    </div>

    <!-- Controles + selector de mano -->
    <div class="card control-panel">

      <!-- Selector Mano: define con qué mano se etiquetará el próximo registro guardado -->
      <div class="hand-selector">
        <button class="hand-btn active-left" id="btnManoIzq" onclick="seleccionarMano('I')">🖐 Mano Izquierda</button>
        <button class="hand-btn"             id="btnManoDer" onclick="seleccionarMano('D')">Mano Derecha 🤚</button>
      </div>

      <!-- Campo de texto para identificar al paciente. El placeholder se autoborra al hacer foco. -->
      <input type="text" id="nombrePaciente" placeholder="Nombre - Rut paciente" value="Nombre - Rut paciente"
             onfocus="if(this.value=='Nombre - Rut paciente')this.value=''">

      <div class="btn-group">
        <button class="btn-guardar" onclick="guardarRegistro()">Guardar en Memoria</button>
        <button class="btn-borrar"  onclick="borrarTodoElHistorial()">Borrar Todo</button>
      </div>

      <!-- Tare (poner la celda en cero) + indicador de estado de la conexión WebSocket -->
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

  <!-- ══ COLUMNA DERECHA: historial + juego AR ══ -->
  <div class="col-derecha">

    <!-- HISTORIAL: tabla llenada dinámicamente desde /historial -->
    <div class="historial-header">Historial Permanente</div>
    <table>
      <thead>
        <tr><th>Fecha/Hora</th><th>Paciente</th><th>Mano</th><th>Fuerza</th></tr>
      </thead>
      <tbody id="cuerpoTabla"></tbody>
    </table>

    <!-- JUEGO AR: niveles generados dinámicamente por JS a partir del arreglo NIVELES -->
    <div class="ar-section">
      <div class="ar-title"><span>🎮</span> Desafío AR — Rehabilitación</div>

      <div class="niveles-grid" id="nivelesGrid">
        <!-- generado por JS -->
      </div>
    </div>

  </div><!-- /col-derecha -->

</div><!-- /dashboard -->

<!-- ══ MODAL QR GRANDE: se abre al pulsar "Mostrar QR para celular" en un nivel ══ -->
<div id="modalQR" style="
  display:none; position:fixed; inset:0; background:rgba(0,0,0,.85);
  z-index:2000; align-items:center; justify-content:center; flex-direction:column; gap:20px;">

  <div style="background:#161b26; border:2px solid #00b4d8; border-radius:18px;
              padding:30px 36px; text-align:center; max-width:380px; width:90%;
              box-shadow:0 20px 60px rgba(0,0,0,.6); position:relative;">

    <!-- Botón para cerrar el modal -->
    <button onclick="cerrarQR()" style="
      position:absolute; top:12px; right:16px; background:none; border:none;
      color:#ef4444; font-size:22px; cursor:pointer; font-weight:900; padding:0; flex:none;">✕</button>

    <!-- Título del nivel (se rellena con JS al abrir el modal) -->
    <div id="qrModalEmoji" style="font-size:2.5rem; margin-bottom:6px;"></div>
    <div id="qrModalTitulo" style="color:#00f5d4; font-weight:700; font-size:1.2rem;
                                    text-transform:uppercase; letter-spacing:1px; margin-bottom:4px;"></div>
    <div id="qrModalSub" style="color:#94a3b8; font-size:13px; margin-bottom:20px;"></div>

    <!-- QR grande generado dinámicamente (imagen desde la API de qrserver.com) -->
    <div style="background:#ffffff; border-radius:12px; padding:14px; display:inline-block; margin-bottom:18px;">
      <img id="qrModalImg" src="" alt="QR"
           style="width:220px; height:220px; display:block; object-fit:contain;">
    </div>

    <!-- Instrucción para el usuario -->
    <div style="background:#0f131c; border-radius:10px; padding:12px 16px;
                font-size:13px; color:#cbd5e1; line-height:1.6; border:1px solid #232a3b;">
      📲 <strong style="color:#00f5d4;">Escanea este QR</strong> con la cámara de tu celular.<br>
      Se abrirá la experiencia de Realidad Aumentada directamente en el navegador.<br>
      <span style="color:#94a3b8; font-size:11px;">No necesitas instalar ninguna app.</span>
    </div>

    <!-- Link directo por si el QR falla (fallback accesible) -->
    <div style="margin-top:14px;">
      <a id="qrModalLink" href="#" target="_blank"
         style="color:#00b4d8; font-size:12px; text-decoration:underline;">
        ¿No funciona el QR? Abre el link directo aquí →
      </a>
    </div>

  </div>
</div>

<!-- Botón flotante que abre/cierra el chat con la IA -->
<div class="ia-floating-btn" onclick="toggleChatIA()">🤖</div>

<!-- Modal del chat IA -->
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
//  ⚠️ ADVERTENCIA DE SEGURIDAD: esta clave queda expuesta en el
//  HTML/JS que se envía al navegador de cualquiera que visite la
//  página. Cualquier persona puede leerla con "Ver código fuente" y
//  usarla por su cuenta. Para un despliegue real conviene mover esta
//  llamada a la IA a un backend/proxy que guarde la clave en el
//  servidor, no en el cliente.
// ════════════════════════════════════════
const GEMINI_API_KEY = "AQ.Ab8RN6J4WNhPT_iyNfTbLlEL7VpVcnSr9YV5NYTxRNutd6hKLQ";

// ════════════════════════════════════════
//  HELPER: genera la URL de un QR dinámico
//  Usa la API pública de goqr.me / qrserver.com (no requiere CORS)
//  para no depender de imágenes ni librerías locales de generación
//  de QR.
// ════════════════════════════════════════
function qrUrl(arLink, size) {
  return 'https://api.qrserver.com/v1/create-qr-code/?size=' + size + 'x' + size +
         '&ecc=H&data=' + encodeURIComponent(arLink);
}

// ════════════════════════════════════════
//  ESTADO GLOBAL (variables compartidas por toda la app en el navegador)
// ════════════════════════════════════════
let historialCompletoIA = [];    // Copia del historial traído del servidor, usada como contexto para la IA
let manoSeleccionada    = 'I';   // Mano actualmente elegida ('I' = izquierda, 'D' = derecha)
let datosActuales       = { presion: 0, hora: "" }; // Última lectura recibida por WebSocket
let datosFuerza         = [];    // Buffer con los últimos valores de fuerza, usado para dibujar el gráfico
const maxPuntos         = 120;   // Cantidad máxima de puntos que se mantienen en el gráfico (ventana deslizante)

const wrapperFuerza = document.getElementById('wrapperFuerza');
const valorActualEl = document.getElementById('valorActual');
const canvas        = document.getElementById('graficoPresion');
const ctx           = canvas.getContext('2d');

// ════════════════════════════════════════
//  DEFINICIÓN DE NIVELES AR
//  Cada nivel define el objeto a "levantar" en la experiencia AR,
//  la fuerza mínima requerida (umbral, en Newtons) y el link a la
//  experiencia de Realidad Aumentada. El QR se genera al vuelo a
//  partir de arUrl, no hace falta guardar imágenes de QR.
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
//  PERSISTENCIA DE NIVELES (localStorage del navegador)
//  Guarda qué niveles están completados y hasta cuál se ha
//  desbloqueado, para que el progreso del juego no se pierda al
//  recargar la página.
// ════════════════════════════════════════
function cargarNivelesStorage() {
  try {
    const saved = localStorage.getItem('dinahand_niveles');
    if (saved) return JSON.parse(saved);
  } catch(e) {}
  // Valor por defecto si no hay nada guardado o si localStorage falla:
  // solo el nivel 1 desbloqueado y ningún nivel completado.
  return { maxDesbloqueado: 1, completados: [] };
}

function guardarNivelesStorage(estado) {
  try { localStorage.setItem('dinahand_niveles', JSON.stringify(estado)); } catch(e) {}
}

let estadoNiveles = cargarNivelesStorage(); // Se carga una sola vez al iniciar el script

// ════════════════════════════════════════
//  RENDERIZAR NIVELES AR
//  Reconstruye por completo el HTML de la grilla de niveles según
//  el estado actual (bloqueado / activo / completado) y la fuerza
//  medida en este momento.
// ════════════════════════════════════════
function renderizarNiveles() {
  const grid = document.getElementById('nivelesGrid');
  grid.innerHTML = ''; // Limpia la grilla antes de volver a dibujarla

  NIVELES.forEach(nivel => {
    const estaCompletado = estadoNiveles.completados.includes(nivel.id);
    const estaDesbloq    = nivel.id <= estadoNiveles.maxDesbloqueado;
    const estaBloqueado  = !estaDesbloq;

    // Determina la clase CSS según el estado del nivel (mutuamente excluyentes)
    let estadoCard = estaBloqueado  ? 'bloqueado'  :
                     estaCompletado ? 'completado' : 'activo';
    let estadoNum  = estadoCard;

    const card = document.createElement('div');
    card.className = 'nivel-card ' + estadoCard;
    card.id = 'nivel-card-' + nivel.id;

    const fuerzaActualStr = datosActuales.presion.toFixed(1);
    const superaUmbral    = datosActuales.presion >= nivel.umbral;

    // QR pequeño (80px) que se muestra directamente dentro de la tarjeta
    const qrSmall = qrUrl(nivel.arUrl, 80);

    // Construcción manual del HTML interno de la tarjeta (concatenación de strings)
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
//  Se llama al presionar "Lo logré". Agrega el nivel a la lista de
//  completados y, si corresponde, desbloquea el siguiente. Luego
//  guarda el nuevo estado y vuelve a dibujar la grilla.
// ════════════════════════════════════════
function marcarLogrado(nivelId) {
  if (!estadoNiveles.completados.includes(nivelId)) {
    estadoNiveles.completados.push(nivelId);
  }
  // Solo avanza el desbloqueo si el nivel logrado es el "de la frontera"
  // y no es ya el último nivel disponible.
  if (nivelId >= estadoNiveles.maxDesbloqueado && nivelId < NIVELES.length) {
    estadoNiveles.maxDesbloqueado = nivelId + 1;
  }
  guardarNivelesStorage(estadoNiveles);
  renderizarNiveles();
}

// ════════════════════════════════════════
//  ACTUALIZAR FUERZA EN TARJETAS AR
//  Se ejecuta en cada mensaje del WebSocket para refrescar solo el
//  texto de "fuerza actual" en cada tarjeta, sin tener que volver a
//  dibujar toda la grilla (más eficiente que llamar renderizarNiveles()).
// ════════════════════════════════════════
function actualizarFuerzaEnNiveles() {
  NIVELES.forEach(nivel => {
    const el = document.getElementById('fuerza-disp-' + nivel.id);
    if (el) {
      const supera = datosActuales.presion >= nivel.umbral;
      el.textContent = datosActuales.presion.toFixed(1) + ' N';
      // Actualiza el ✅ (nodo de texto justo después del span) según si se supera el umbral
      const parent = el.parentElement;
      const lastNode = parent.lastChild;
      if (lastNode && lastNode.nodeType === 3) { // nodeType 3 = nodo de texto
        lastNode.textContent = supera ? ' ✅' : '';
      }
    }
  });
}

// ════════════════════════════════════════
//  SELECTOR DE MANO
//  Cambia la mano activa y actualiza el estilo visual de los dos
//  botones (izquierda/derecha) para reflejar cuál está seleccionada.
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
//  Dibuja a mano (con la API Canvas 2D) una grilla de fondo y la
//  curva de fuerza como un área rellena + línea. No usa ninguna
//  librería externa de gráficos.
// ════════════════════════════════════════
function redibujarGrafico() {
  const w = canvas.width, h = canvas.height;
  ctx.clearRect(0, 0, w, h); // Limpia el canvas completo antes de redibujar

  // Grilla de fondo: 4 líneas horizontales y 9 verticales
  ctx.strokeStyle = '#232a3b'; ctx.lineWidth = 1;
  for (let i = 1; i < 5; i++) { let y=(h/5)*i; ctx.beginPath(); ctx.moveTo(0,y); ctx.lineTo(w,y); ctx.stroke(); }
  for (let i = 1; i < 10; i++) { let x=(w/10)*i; ctx.beginPath(); ctx.moveTo(x,0); ctx.lineTo(x,h); ctx.stroke(); }

  if (datosFuerza.length === 0) return; // Nada que dibujar todavía

  let maxVal = Math.max(...datosFuerza, 10); // Escala vertical dinámica (mínimo 10 para que no se vea plano)
  let pasoX  = w / (maxPuntos - 1);           // Distancia horizontal entre cada punto

  // Área rellena bajo la curva (efecto degradado sutil)
  ctx.beginPath(); ctx.fillStyle = 'rgba(0,245,212,0.08)'; ctx.moveTo(0,h);
  for (let i=0;i<datosFuerza.length;i++){let x=i*pasoX,y=h-(datosFuerza[i]/maxVal)*(h-30)-15;ctx.lineTo(x,y);}
  ctx.lineTo((datosFuerza.length-1)*pasoX,h); ctx.closePath(); ctx.fill();

  // Línea de la curva propiamente dicha
  ctx.beginPath(); ctx.strokeStyle='#00f5d4'; ctx.lineWidth=3;
  for (let i=0;i<datosFuerza.length;i++){let x=i*pasoX,y=h-(datosFuerza[i]/maxVal)*(h-30)-15;if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);}
  ctx.stroke();
}
redibujarGrafico(); // Dibuja la grilla vacía apenas carga el script (antes de recibir datos)

// ════════════════════════════════════════
//  WEBSOCKET
//  Se conecta al servidor WebSocket del ESP32 (puerto 81) para
//  recibir la fuerza medida en tiempo real, sin necesidad de estar
//  refrescando la página con peticiones HTTP repetidas.
// ════════════════════════════════════════
function setWsStatus(ok) {
  document.getElementById('wsDot').style.background = ok ? '#22c55e' : '#ef4444';
  document.getElementById('wsTxt').textContent = ok ? 'Báscula conectada' : 'Sin conexión';
}

// window.location.hostname toma automáticamente la IP del ESP32 (la misma
// desde la que se cargó la página), así no hay que escribirla a mano.
const connection = new WebSocket('ws://' + window.location.hostname + ':81/');
connection.onopen  = () => { wrapperFuerza.style.borderColor = '#00f5d4'; setWsStatus(true); };
connection.onerror = () => { wrapperFuerza.style.borderColor = '#ef4444'; setWsStatus(false); };
connection.onclose = () => { wrapperFuerza.style.borderColor = '#ef4444'; setWsStatus(false); };

// Se ejecuta cada vez que llega un mensaje nuevo del ESP32 (cada 100 ms aprox.)
connection.onmessage = (e) => {
  try {
    const data = JSON.parse(e.data);       // Formato esperado: {"p": fuerza, "h": "fecha/hora"}
    datosActuales.presion = data.p;
    datosActuales.hora    = data.h;
    valorActualEl.innerText = data.p.toFixed(2);   // Actualiza el número grande en pantalla
    datosFuerza.push(data.p);
    if (datosFuerza.length > maxPuntos) datosFuerza.shift(); // Mantiene el buffer con tamaño fijo (ventana deslizante)
    redibujarGrafico();
    actualizarFuerzaEnNiveles();
  } catch(err) { console.warn("JSON inválido:", e.data); }
};

// ════════════════════════════════════════
//  TARE MANUAL
//  Pide al ESP32 que ponga la celda de carga en cero (por ejemplo,
//  para descontar el peso del propio dinamómetro antes de medir).
// ════════════════════════════════════════
function hacerTare() {
  fetch('/tare')
    .then(r => r.text())
    .then(t => alert('✅ ' + t + ' — la báscula quedó en cero'))
    .catch(() => alert('❌ No se pudo conectar al ESP32 para hacer tare'));
}

// ════════════════════════════════════════
//  GUARDAR REGISTRO (con mano)
//  Valida que se haya escrito un nombre de paciente y envía el
//  registro actual (paciente, fuerza, fecha y mano) al servidor
//  mediante una petición GET a /guardar con parámetros en la URL.
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
    .then(r => r.text()).then(() => cargarHistorialServidor()); // Recarga la tabla tras guardar
}

// ════════════════════════════════════════
//  CARGAR HISTORIAL
//  Pide al servidor el historial completo en JSON y reconstruye la
//  tabla, mostrando los registros más recientes primero.
// ════════════════════════════════════════
function cargarHistorialServidor() {
  fetch('/historial').then(r => r.json()).then(data => {
    historialCompletoIA = data; // Se guarda también para usarlo como contexto del chat IA
    const cuerpo = document.getElementById('cuerpoTabla');
    cuerpo.innerHTML = '';
    [...data].reverse().forEach(reg => { // reverse(): del más nuevo al más antiguo
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

// Pide confirmación y, si se acepta, borra todo el historial en el servidor
function borrarTodoElHistorial() {
  if (confirm('¿Está seguro de eliminar de forma permanente todo el historial?')) {
    fetch('/borrar').then(() => cargarHistorialServidor());
  }
}

// ════════════════════════════════════════
//  ASISTENTE IA (con contexto por mano)
//  Abre/cierra la ventana flotante del chat.
// ════════════════════════════════════════
function toggleChatIA() {
  const modal = document.getElementById('modalChatIA');
  if (modal.style.display === 'flex') { modal.style.display = 'none'; }
  else { modal.style.display = 'flex'; document.getElementById('iaPregunta').focus(); }
}

// Envía la pregunta del usuario + el historial (separado por mano) a la
// API de Gemini (Google) y muestra la respuesta en el chat.
function preguntarIA() {
  const pregunta = document.getElementById('iaPregunta').value;
  const resp     = document.getElementById('iaRespuesta');
  if (!pregunta.trim()) return;
  resp.innerText = 'Analizando... 🧠';

  // Separa el historial por mano para poder responder preguntas específicas
  const histIzq = historialCompletoIA.filter(r => (r.mano||'') === 'IZQ');
  const histDer = historialCompletoIA.filter(r => (r.mano||'') === 'DER');

  // "Prompt" que le da a la IA su rol y los datos disponibles
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
//  Muestra en pantalla completa (modal) el QR de 220px de un nivel
//  específico, generado dinámicamente a partir de su arUrl.
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

// Cierra el modal también si se hace clic fuera de la tarjeta (en el fondo oscuro)
document.getElementById('modalQR').addEventListener('click', function(e) {
  if (e.target === this) cerrarQR();
});

// ════════════════════════════════════════
//  INIT
//  Punto de entrada del frontend: se ejecuta al cargar el <body>.
// ════════════════════════════════════════
function init() {
  cargarHistorialServidor(); // Trae y muestra el historial guardado
  renderizarNiveles();       // Dibuja la grilla de niveles AR según el progreso guardado
}
</script>
</body>
</html>
)rawliteral";


// ============================================================
//  FUNCIONES DEL SERVIDOR
//  Cada una está asociada a una ruta HTTP mediante server.on(...)
//  en el setup(), y responde a las peticiones que hace el JavaScript
//  del navegador (fetch).
// ============================================================

// Ruta "/" — Sirve la página completa (HTML+CSS+JS) almacenada en index_html
void manejarRaiz() { server.send(200, "text/html", index_html); }

// Ruta "/guardar" — Recibe paciente, fuerza, fecha y mano por query string
// y agrega una línea nueva al archivo de historial en LittleFS.
void manejarGuardar() {
    String paciente = server.arg("paciente");
    String fuerza   = server.arg("fuerza");
    String fecha    = server.arg("fecha");
    String mano     = server.arg("mano");

    // Se reemplaza el carácter "|" por "-" porque "|" se usa como separador
    // de campos al guardar la línea; si el nombre del paciente lo trajera,
    // rompería el formato del archivo.
    paciente.replace("|", "-");
    fecha.replace("|", "-");
    mano.replace("|", "-");
    if (mano.length() == 0) mano = "?"; // Por si llega sin especificar mano

    // FILE_APPEND: agrega la línea al final del archivo sin borrar lo anterior
    File file = LittleFS.open(HISTORIAL_FILE, FILE_APPEND);
    if (file) {
        file.println(fecha + "|" + paciente + "|" + fuerza + "|" + mano);
        file.close();
        server.send(200, "text/plain", "OK");
    } else {
        server.send(500, "text/plain", "Error al abrir archivo");
    }
}

// Ruta "/historial" — Lee el archivo línea por línea, separa los 4 campos
// (fecha|paciente|fuerza|mano) y arma manualmente un string JSON con todos
// los registros, para que el frontend lo pueda parsear con JSON.parse().
void manejarHistorial() {
    File file = LittleFS.open(HISTORIAL_FILE, FILE_READ);
    String json = "[";
    if (file) {
        bool primero = true;
        while (file.available()) {
            String linea = file.readStringUntil('\n');
            linea.trim();
            if (linea.length() == 0) continue; // Ignora líneas vacías

            // Ubica las posiciones de los separadores "|" para partir la línea en campos
            int p1 = linea.indexOf('|');
            int p2 = linea.indexOf('|', p1 + 1);
            int p3 = linea.indexOf('|', p2 + 1);

            if (p1 != -1 && p2 != -1) {
                String fecha    = linea.substring(0, p1);
                String paciente = linea.substring(p1 + 1, p2);
                String fuerza   = linea.substring(p2 + 1, p3 != -1 ? p3 : linea.length());
                // Compatibilidad con registros antiguos que no tenían el campo "mano" (p3 == -1)
                String mano     = (p3 != -1) ? linea.substring(p3 + 1) : "?";

                // Escapa comillas dobles para no romper la sintaxis del JSON generado a mano
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

// Ruta "/borrar" — Elimina completamente el archivo de historial
void manejarBorrar() {
    LittleFS.remove(HISTORIAL_FILE);
    server.send(200, "text/plain", "Borrado");
}

// Ruta "/tare" — Pone en cero la celda de carga (descuenta cualquier peso
// que esté aplicado actualmente como si fuera el punto de referencia)
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
//  Funciones auxiliares para mostrar distintas pantallas de estado
//  y datos en la pantallita OLED física del dispositivo.
// ============================================================

// Muestra un mensaje de estado genérico durante el arranque (setup),
// por ejemplo "Conectando WiFi... [ OK ]" o "[ERROR]".
void oledStatus(const char* titulo, const char* estado, bool ok) {
    if (!oledOK) return; // Si la pantalla no está disponible, no hace nada
    display.clearDisplay();
    display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0); display.println("-- DinaHand v3 --");
    display.println(""); display.println(titulo);
    display.print("> "); display.println(estado);
    display.println(""); display.print(ok ? "[ OK ]" : "[ERROR]");
    display.display();
}

// Muestra un resumen final del diagnóstico de arranque: estado de la
// celda, del OLED (siempre OK si llegó hasta acá), del WiFi y la IP.
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

// Muestra en la OLED la fuerza medida en el momento (número grande)
// junto con la fecha/hora, durante el funcionamiento normal (loop).
void oledMostrarDatos(float fuerzaN, const char* fechaHora) {
    if (!oledOK) return;
    display.clearDisplay();
    display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0); display.print("MONITOR DE FUERZA");
    display.setTextSize(3); display.setCursor(0, 20); display.print(fuerzaN, 1); // 1 decimal, letra grande
    display.setTextSize(2); display.print(" N");
    display.setTextSize(1); display.setCursor(0, 55); display.print(fechaHora);
    display.display();
}


// ============================================================
//  SETUP
//  Se ejecuta una sola vez al encender o reiniciar el ESP32.
//  Inicializa cada componente en orden y muestra el progreso tanto
//  por el puerto Serial como en la pantalla OLED.
// ============================================================
void setup() {
    Serial.begin(115200); // Inicia la comunicación serial para depuración (monitor serie)
    delay(300);            // Pequeña pausa para que el puerto serial se estabilice

    Serial.println("\n========================================");
    Serial.println("   DinaHand v3 - Iniciando sistema...");
    Serial.println("========================================");

    // --- Sistema de archivos (para guardar el historial de forma persistente) ---
    if (!LittleFS.begin(true)) { // true = formatea automáticamente si no puede montar
        Serial.println("[FS] ERROR: No se pudo montar LittleFS");
    } else {
        Serial.println("[FS] LittleFS montado OK");
    }

    // --- Pantalla OLED ---
    Wire.begin(I2C_SDA, I2C_SCL); // Inicializa el bus I2C con los pines personalizados
    if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        Serial.println("[OLED] ERROR: Pantalla no detectada en 0x3C");
        oledOK = false;
    } else {
        oledOK = true;
        Serial.println("[OLED] Pantalla OK - encendida");
        oledStatus("Iniciando...", "Buscando celda", true);
    }

    // --- Celda de carga (HX711) ---
    scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
    scale.set_scale(FACTOR_CALIBRACION); // Aplica el factor de calibración definido arriba

    Serial.print("[CELDA] Esperando respuesta HX711");
    int intentos = 0;
    // Espera hasta 2 segundos (20 intentos x 100 ms) a que el HX711 esté listo
    while (!scale.is_ready() && intentos < 20) {
        Serial.print("."); delay(100); intentos++;
    }

    if (scale.is_ready()) {
        scale.tare(); // Pone la celda en cero automáticamente al arrancar
        scaleOK = true;
        Serial.println("\n[CELDA] HX711 OK - Tare aplicado");
        oledStatus("Celda de carga:", "HX711 OK - Tared", true);
    } else {
        scaleOK = false;
        Serial.println("\n[CELDA] ERROR: HX711 no responde. Revisa pines 4 y 5");
        oledStatus("Celda de carga:", "ERROR - sin resp.", false);
    }
    delay(600); // Da tiempo a leer el mensaje en la OLED antes de pasar a WiFi

    // --- Conexión WiFi ---
    oledStatus("Conectando WiFi:", ssid, true);
    WiFi.begin(ssid, password);
    Serial.print("[WiFi] Conectando a: "); Serial.print(ssid);

    unsigned long t0 = millis();
    // Espera hasta 15 segundos a que se conecte, sin bloquear indefinidamente
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(500); Serial.print(".");
    }

    bool wifiOK = (WiFi.status() == WL_CONNECTED);
    if (wifiOK) {
        Serial.println("\n[WiFi] Conectado!");
        Serial.print("[WiFi] IP: "); Serial.println(WiFi.localIP());
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer); // Sincroniza la hora real por NTP
        Serial.println("[NTP ] Sincronizando hora...");
    } else {
        Serial.println("\n[WiFi] ERROR: No se pudo conectar.");
    }

    // --- Resumen final en la OLED ---
    String ipStr = wifiOK ? WiFi.localIP().toString() : "sin WiFi";
    oledBootSummary(scaleOK, wifiOK, ipStr.c_str());
    delay(1500); // Deja el resumen visible unos segundos antes de pasar al modo normal

    // --- Diagnóstico final por consola ---
    Serial.println("\n========================================");
    Serial.print("[DIAG] OLED:   "); Serial.println(oledOK  ? "OK" : "ERROR");
    Serial.print("[DIAG] CELDA:  "); Serial.println(scaleOK ? "OK" : "ERROR");
    Serial.print("[DIAG] WiFi:   "); Serial.println(wifiOK  ? "OK" : "ERROR");
    if (wifiOK) { Serial.print("[DIAG] IP:     "); Serial.println(WiFi.localIP()); }
    Serial.println("========================================\n");

    // --- Registro de rutas del servidor HTTP ---
    server.on("/",          manejarRaiz);
    server.on("/guardar",   manejarGuardar);
    server.on("/historial", manejarHistorial);
    server.on("/borrar",    manejarBorrar);
    server.on("/tare",      manejarTare);
    server.begin();
    Serial.println("[HTTP] Servidor iniciado en puerto 80");

    // --- Arranque del servidor WebSocket ---
    webSocket.begin();
    Serial.println("[WSS ] WebSocket iniciado en puerto 81");
    Serial.println("[SYS ] Sistema listo.\n");
}


// ============================================================
//  LOOP
//  Se ejecuta continuamente mientras el ESP32 está encendido.
//  Atiende las peticiones HTTP/WebSocket, lee la celda de carga
//  cada "interval" ms, y transmite/muestra la fuerza medida.
// ============================================================
void loop() {
    server.handleClient(); // Procesa peticiones HTTP pendientes (no bloqueante)
    webSocket.loop();      // Procesa eventos del WebSocket (conexiones, mensajes, etc.)

    // Control de tiempo no bloqueante: solo continúa si ya pasó el intervalo
    // definido (100 ms), en vez de usar delay() que congelaría el servidor.
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis < interval) return;
    previousMillis = currentMillis;

    float fuerzaNewton = 0.0;
    if (scaleOK && scale.is_ready()) {
        float masaKg = scale.get_units(1); // Promedio de 1 lectura cruda, convertida a "unidades" (kg según calibración)

        // Debug: imprime valor crudo cada 10 lecturas para no saturar el serial
        static int debugCount = 0;
        if (++debugCount >= 10) {
            debugCount = 0;
            Serial.print("[HX711] masa_kg="); Serial.print(masaKg, 4);
            Serial.print("  fuerza_N="); Serial.println(masaKg * 9.81, 2);
        }

        if (masaKg < 0.0) masaKg = -masaKg;   // valor absoluto por si el factor queda invertido
        fuerzaNewton = masaKg * 9.81;         // Conversión de masa (kg) a fuerza (Newton): F = m * g
    } else if (!scaleOK) {
        // Si la celda nunca quedó lista, avisa por serial cada cierto número de ciclos
        // (50 ciclos x 100 ms ≈ cada 5 segundos) en vez de saturar la consola.
        static int errCount = 0;
        if (++errCount >= 50) { errCount = 0; Serial.println("[HX711] ERROR: celda no disponible"); }
    }

    // Obtiene la fecha/hora actual (sincronizada por NTP) y la formatea como texto
    struct tm timeinfo;
    char bufferFechaHora[30];
    if (!getLocalTime(&timeinfo)) {
        strcpy(bufferFechaHora, "--/-- --:--:--"); // Sin hora válida (por ejemplo, sin WiFi)
    } else {
        strftime(bufferFechaHora, sizeof(bufferFechaHora), "%d %b, %H:%M:%S", &timeinfo);
    }

    // Arma el mensaje JSON con la fuerza y la hora, y lo transmite a todos los
    // clientes WebSocket conectados (el navegador lo recibe en connection.onmessage)
    String jsonPayload = "{\"p\":" + String(fuerzaNewton, 2) + ",\"h\":\"" + String(bufferFechaHora) + "\"}";
    webSocket.broadcastTXT(jsonPayload);

    // Actualiza la pantalla OLED física con el mismo dato
    oledMostrarDatos(fuerzaNewton, bufferFechaHora);
}
